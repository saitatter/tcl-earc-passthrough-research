/*
 * tcl_earc_mat.ko - Production buffered real-time kthread driver for TCL C9K (MT5896 / V670)
 * internal Android -> eARC TX lossless TrueHD Dolby Atmos MAT 2.0 bypass.
 *
 * Exposes /dev/earc_mat_bypass:
 * - O_RDONLY open: stateless, non-exclusive <5 us hardware probe & live telemetry reader
 *   (verifies Vivaldi RIU eARC NPCM2 mux + Non-PCM + unmute + multi-slot MAT 2.0 headers
 *   without touching DDR or spawning a kthread; never conflicts with active writer).
 * - O_WRONLY / O_RDWR open: exclusive streaming session (atomic g_open guard) with:
 *   - 8-frame (160 ms) lock-free SPSC jitter buffer + binary completion wakeup (no done-counter spin).
 *   - PAN-safe f_op->write_iter (_copy_from_iter) and lock-free f_op->read_iter (_copy_to_iter)
 *     with FMODE_STREAM (no f_pos_lock contention between concurrent write() and read()).
 *   - f_op->fsync for lossless end-of-stream drain (waits until SPSC queue is committed to DDR).
 *   - SCHED_FIFO kthread (sched_set_fifo_low) + ktime_get() PLL phase-locked TASK_IDLE sleep
 *     cutting hrtimer interrupts to ~250/s while keeping already_ready=0 under heavy 4K UI load.
 *   - Outer-Shareable / System ARM64 memory barriers (dsb st / dmb oshld) + 128-bit pair copies
 *     for deterministic visibility to external MIU/EMI masters (DEC-R2 & SE DSP).
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef long s64;
typedef long ssize_t;
typedef long loff_t;
typedef unsigned long size_t;

struct file;
struct inode;
struct kiocb;
struct iov_iter;
struct task_struct;

extern int _printk(const char *fmt, ...);
extern void *__ioremap(u64 phys_addr, u64 size, u64 prot);
extern void iounmap(volatile void *addr);
extern void *vmalloc(u64 size);
extern void vfree(const void *addr);
extern void usleep_range_state(unsigned long min, unsigned long max, unsigned int state);
extern size_t _copy_from_iter(void *addr, size_t bytes, struct iov_iter *i);
extern size_t _copy_to_iter(const void *addr, size_t bytes, struct iov_iter *i);
extern int misc_register(void *misc);
extern void misc_deregister(void *misc);
extern struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
                                                  void *data, int node,
                                                  const char namefmt[], ...);
extern int wake_up_process(struct task_struct *p);
extern void __init_swait_queue_head(void *q, const char *name, void *key);
extern void complete(void *x);
extern void complete_all(void *x);
extern long wait_for_completion_interruptible_timeout(void *x, unsigned long timeout);
extern s64 ktime_get(void);
extern void sched_set_fifo_low(struct task_struct *p);
extern u8 arm64_use_ng_mappings;

__attribute__((section(".plt"), used)) void __plt_stub(void) {}
__attribute__((section(".init.plt"), used)) void __init_plt_stub(void) {}
__attribute__((section(".text.ftrace_trampoline"), used)) void __ftrace_trampoline_stub(void) {}

__attribute__((aligned(4096), used))
void __cfi_check(u64 id, void *ptr, void *diag) { (void)id; (void)ptr; (void)diag; }

/* SE DSP Non-PCM MAT 2.0 DDR ring (SE_MAD_BASE 0x21B10000 + 0x0098C000) */
#define RING_PHYS          0x2249C000ULL
#define RING_SIZE          0x001A4000ULL /* 28 * 0xF000 = 1,720,320 bytes (560 ms) */
#define SLOT_SIZE          0x0000F000ULL /* 61,440 bytes (20.000 ms @ 192 kHz 8-ch 16-bit) */
#define NUM_SLOTS          28U
#define SENTINEL           0xCAFEBABEU
#define MAT_SYNC_W0        0x4e1ff872U   /* LE u32: Pa=0xF872, Pb=0x4E1F */
#define MAT_SYNC_W1        0xeff00016U   /* LE u32: Pc=0x0016 (MAT), Pd=0xEFF0 (61,424 B) */
#define MIN_VALID_SLOTS    16U
#define REPLACE_WINDOW_NS  1500000ULL    /* 1.5 ms conservative write deadline from last sentinel observation */

/* Vivaldi3 (0x950300) & eARC Digital TX cfg (0x951100) MMIO windows from runtime FDT */
#define VIVALDI3_PHYS      0x1D2A0600ULL
#define VIVALDI3_SIZE      0x00000200ULL
#define VIVALDI3_MUTE_HI   0x019U        /* RIU 0x95030d: bit 7 = ARC/eARC mute (0 = unmuted) */
#define VIVALDI3_NPCM_HI   0x11DU        /* RIU 0x95038f: bit 1 = eARCConfig Non-PCM (1 = on) */

#define EARC_CFG_PHYS      0x1D2A2200ULL
#define EARC_CFG_SIZE      0x00000200ULL
#define EARC_CFG10_HI      0x029U        /* RIU 0x951115: bits [6:4] = 0x20 (SE DSP NPCM2) */

#define Q_SLOTS            8U            /* 8 * 20 ms = 160 ms SPSC jitter buffer (power of 2) */
#define Q_MASK             (Q_SLOTS - 1U)

#define TASK_UNINTERRUPTIBLE 2U
#define TASK_IDLE            0x402U      /* TASK_UNINTERRUPTIBLE | TASK_NOLOAD */
#define NUMA_NO_NODE         (-1)

/* BTF-verified struct file offsets on V670 5.15.192-android14-11 */
#define FILE_OFF_F_FLAGS   0x040U
#define FILE_OFF_F_MODE    0x044U
#define FILE_OFF_PRIV      0x0D8U
#define FMODE_WRITE_BIT    0x00000002U
#define FMODE_STREAM_SET   0x00200000U
#define FMODE_STREAM_CLR   0xFFDF7FE3U   /* clears FMODE_LSEEK|PREAD|PWRITE|ATOMIC_POS */

/* Fixed-layout status ABI shared between 64-bit kernel and 32-bit Audio HAL shim (40 bytes) */
struct earc_mat_status {
    u64 frames_out;
    u64 last_slot_mono_ns;
    u32 q_depth;
    u32 underruns;
    u32 already_ready;
    u32 relocks;
    u32 ring_active;
    u32 header_changes;
};

static volatile u8 *g_ring;
static volatile u8 *g_riu_vivaldi3;
static volatile u8 *g_riu_earc_cfg;
static u8 *g_q_buf;

static volatile u32 g_q_head;
static volatile u32 g_q_tail;
static u32 g_partial;

#include "earc_mat_control.h"
static int g_open;
static int g_paused;
static int g_pump_committing;
static volatile int g_stop_req;
static volatile int g_thread_alive;
static int g_prelocked;

struct v670_completion {
    u32 done;
    u32 pad;
    u8  swait_head[128];
};
static struct v670_completion g_space_comp;
static struct v670_completion g_drain_comp;
static u8 g_space_lock_key[16];
static u8 g_drain_lock_key[16];

static u32 g_base_off;
static u32 g_next_slot;
static u64 g_frames_out;
static u64 g_last_slot_ns;
static u32 g_underruns;
static u32 g_already_ready;
static u32 g_relocks;
static u32 g_header_changes;

static inline void dsp_wmb(void)
{
    /* Drain CPU store/write-combining buffers out to EMI/MIU DDR for SE DSP & DEC-R2 */
    __asm__ volatile("dsb st" ::: "memory");
}

static inline void dsp_rmb(void)
{
    /* Outer-shareable load barrier for DDR written by external DEC-R2 master */
    __asm__ volatile("dmb oshld" ::: "memory");
}

static inline void smp_full_mb(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline u32 slot_start_off(u32 s)
{
    return (g_base_off + s * (u32)SLOT_SIZE) % (u32)RING_SIZE;
}

static inline u32 slot_end_off(u32 s)
{
    return (g_base_off + s * (u32)SLOT_SIZE + (u32)SLOT_SIZE - 4U) % (u32)RING_SIZE;
}

static inline u32 ring_read32(u32 off)
{
    return *(volatile u32 *)(g_ring + off);
}

static inline void ring_write32(u32 off, u32 val)
{
    *(volatile u32 *)(g_ring + off) = val;
}

static inline int slot_has_mat_header(u32 s)
{
    u32 off = slot_start_off(s);
    return ring_read32(off) == MAT_SYNC_W0 &&
           ring_read32((off + 4U) % (u32)RING_SIZE) == MAT_SYNC_W1;
}

static void copy_blocks64(u8 *dst, const u8 *src, u32 bytes)
{
    const u64 *s = (const u64 *)src;
    u64 *d = (u64 *)dst;
    u32 n = bytes / 8U;
    u32 i = 0;
    for (; i + 4U <= n; i += 4U) {
        u64 a = s[i], b = s[i + 1U], c = s[i + 2U], e = s[i + 3U];
        __asm__ volatile(
            "stp %0, %1, [%4]\n\t"
            "stp %2, %3, [%4, #16]"
            :
            : "r"(a), "r"(b), "r"(c), "r"(e), "r"(d + i)
            : "memory");
    }
    for (; i < n; i++)
        *(volatile u64 *)(d + i) = s[i];
}

static void ring_write_slot(u32 s, const u8 *src)
{
    u32 off = slot_start_off(s);
    u32 first = (u32)RING_SIZE - off;
    if (first > (u32)SLOT_SIZE)
        first = (u32)SLOT_SIZE;

    copy_blocks64((u8 *)(g_ring + off), src, first);

    u32 rem = (u32)SLOT_SIZE - first;
    if (rem > 0U)
        copy_blocks64((u8 *)g_ring, src + first, rem);

    dsp_wmb();
}

static int earc_hw_npcm_active(void)
{
    if (!g_riu_vivaldi3 || !g_riu_earc_cfg)
        return 1;

    u8 cfg10_hi = *(volatile u8 *)(g_riu_earc_cfg + EARC_CFG10_HI);
    u8 npcm_hi  = *(volatile u8 *)(g_riu_vivaldi3 + VIVALDI3_NPCM_HI);
    u8 mute_hi  = *(volatile u8 *)(g_riu_vivaldi3 + VIVALDI3_MUTE_HI);
    dsp_rmb();

    /* Require eARC TX mux == SE DSP NPCM2 (0x20), Non-PCM bit set (0x02), and eARC unmuted (!0x80) */
    if ((cfg10_hi & 0x70U) != 0x20U)
        return 0;
    if ((npcm_hi & 0x02U) == 0U)
        return 0;
    if ((mute_hi & 0x80U) != 0U)
        return 0;
    return 1;
}

static u32 find_valid_ring_base(void)
{
    if (!g_ring)
        return 0xffffffffU;

    dsp_rmb();
    for (u32 off = 0; off < (u32)SLOT_SIZE; off += 4U) {
        if (ring_read32(off) != MAT_SYNC_W0)
            continue;
        if (ring_read32((off + 4U) % (u32)RING_SIZE) != MAT_SYNC_W1)
            continue;

        u32 valid = 0;
        for (u32 s = 0; s < NUM_SLOTS; s++) {
            u32 soff = (off + s * (u32)SLOT_SIZE) % (u32)RING_SIZE;
            if (ring_read32(soff) == MAT_SYNC_W0 &&
                ring_read32((soff + 4U) % (u32)RING_SIZE) == MAT_SYNC_W1) {
                valid++;
            }
        }
        if (valid >= MIN_VALID_SLOTS)
            return off;
    }
    return 0xffffffffU;
}

static inline void signal_binary_comp(struct v670_completion *c)
{
    smp_full_mb();
    if (!*(volatile u32 *)&c->done)
        complete(c);
}

#ifdef EARC_MAT_TRACE_SYNC
/* V670 stock debug handler logs the DSP SRAM read in the kernel log. */
extern u8 MApi_AUDIO_offload_Debug_Cmd_Write(char *cmd, u32 len);

static void trace_sync_slot(u32 slot)
{
    char cmd[64] = "read_dsp_sram_type=1 addr=0x1113 len=1\n";
    s64 before = ktime_get();
    _printk("\0016[tcl_earc_mat] sync probe slot=%u offset=0x%x mono_ns=%lld\n",
            slot, slot_start_off(slot), (long long)before);
    int rc = MApi_AUDIO_offload_Debug_Cmd_Write(cmd, sizeof(cmd));
    _printk("\0016[tcl_earc_mat] sync probe end rc=%d elapsed_ns=%lld\n", rc,
            (long long)(ktime_get() - before));
}
#endif

static int lock_onto_ring_timeout(u32 max_ticks)
{
    if (!earc_hw_npcm_active())
        return -19; /* -ENODEV */

    u32 base = find_valid_ring_base();
    if (base == 0xffffffffU)
        return -19; /* -ENODEV */
    g_base_off = base;

    for (u32 s = 0; s < NUM_SLOTS; s++)
        ring_write32(slot_end_off(s), SENTINEL);
    dsp_wmb();

    for (u32 t = 0; t < max_ticks && !g_stop_req; t++) {
        usleep_range_state(200, 300, TASK_IDLE);
        dsp_rmb();
        for (u32 s = 0; s < NUM_SLOTS; s++) {
            if (ring_read32(slot_end_off(s)) != SENTINEL) {
                g_next_slot = (s + 1U) % NUM_SLOTS;
                for (u32 k = 0; k < NUM_SLOTS; k++) {
                    if (k != g_next_slot && ring_read32(slot_end_off(k)) == SENTINEL)
                        ring_write32(slot_end_off(k), 0);
                }
                dsp_wmb();
#ifdef EARC_MAT_TRACE_SYNC
                trace_sync_slot(s);
#endif
                return 0;
            }
        }
    }

    for (u32 k = 0; k < NUM_SLOTS; k++) {
        if (ring_read32(slot_end_off(k)) == SENTINEL)
            ring_write32(slot_end_off(k), 0);
    }
    dsp_wmb();
    return -110; /* -ETIMEDOUT */
}

static int mat_pump_thread(void *arg)
{
    (void)arg;
    struct task_struct *self;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(self));
    sched_set_fifo_low(self);

    int locked = g_prelocked;
    int started = 0;
    unsigned long target_lead_us = 17500UL; /* 17.5ms initial sleep leaves 2.5ms SCHED_FIFO margin (auto-backs off on late) */

    if (locked) {
        _printk("\0016[tcl_earc_mat] pump locked (SCHED_FIFO): base_off=0x%x next_slot=%u\n",
                g_base_off, g_next_slot);
    }

    while (!g_stop_req) {
        if (!locked) {
            if (!earc_hw_npcm_active()) {
                _printk("\0016[tcl_earc_mat] eARC NPCM2 became inactive; stopping pump\n");
                break;
            }
            if (lock_onto_ring_timeout(200) == 0) {
                locked = 1;
                _printk("\0016[tcl_earc_mat] pump relocked: base_off=0x%x next_slot=%u\n",
                        g_base_off, g_next_slot);
            } else {
                if (!earc_hw_npcm_active())
                    break;
                usleep_range_state(10000, 15000, TASK_IDLE);
                continue;
            }
        }

        u32 end_off = slot_end_off(g_next_slot);
        u64 sample_ns = (u64)ktime_get();
        dsp_rmb();
        int ready = 0;
        int was_late = 0;
        u64 replace_before_ns = 0;
        if (ring_read32(end_off) != SENTINEL) {
            __atomic_fetch_add(&g_already_ready, 1U, __ATOMIC_RELAXED);
            ready = 1;
            was_late = 1;
            if (target_lead_us > 12000UL)
                target_lead_us -= 500UL;
        } else {
            u64 marker_seen_ns = sample_ns;
            for (u32 t = 0; t < 400 && !g_stop_req; t++) {
                usleep_range_state(200, 280, TASK_IDLE);
                sample_ns = (u64)ktime_get();
                dsp_rmb();
                if (ring_read32(end_off) != SENTINEL) {
                    replace_before_ns = marker_seen_ns + REPLACE_WINDOW_NS;
                    ready = 1;
                    break;
                }
                marker_seen_ns = sample_ns;
            }
        }

        if (g_stop_req)
            break;

        if (!ready) {
            __atomic_fetch_add(&g_relocks, 1U, __ATOMIC_RELAXED);
            locked = 0;
            continue;
        }

        if (!earc_hw_npcm_active()) {
            _printk("\0016[tcl_earc_mat] eARC NPCM2 became inactive; stopping pump\n");
            break;
        }

        u64 flip_ns = (u64)ktime_get();

        u32 tail = __atomic_load_n(&g_q_tail, __ATOMIC_RELAXED);
        u32 head = __atomic_load_n(&g_q_head, __ATOMIC_ACQUIRE);
        __atomic_store_n(&g_pump_committing, 1, __ATOMIC_SEQ_CST);
        int paused = __atomic_load_n(&g_paused, __ATOMIC_SEQ_CST);
        if (!paused && tail != head) {
            /* Verify live MAT header before checking the final write deadline */
            if (!slot_has_mat_header(g_next_slot)) {
                __atomic_fetch_add(&g_header_changes, 1U, __ATOMIC_RELAXED);
            } else if (was_late || !replace_before_ns || (u64)ktime_get() > replace_before_ns) {
                if (!was_late) {
                    __atomic_fetch_add(&g_already_ready, 1U, __ATOMIC_RELAXED);
                    was_late = 1;
                    if (target_lead_us > 12000UL)
                        target_lead_us -= 500UL;
                }
            } else {
                const u8 *src = g_q_buf + (u64)(tail & Q_MASK) * SLOT_SIZE;
                ring_write_slot(g_next_slot, src);
                u32 next_tail = tail + 1U;
                __atomic_store_n(&g_q_tail, next_tail, __ATOMIC_RELEASE);
                __atomic_store_n(&g_last_slot_ns, flip_ns, __ATOMIC_RELEASE);
                __atomic_fetch_add(&g_frames_out, 1ULL, __ATOMIC_RELEASE);
                signal_binary_comp(&g_space_comp);
                if (next_tail == head)
                    signal_binary_comp(&g_drain_comp);
                started = 1;
            }
        } else if (!paused) {
            signal_binary_comp(&g_drain_comp);
            if (started) {
                /* Leave DEC-R2's silent MAT frame untouched on underrun */
                __atomic_fetch_add(&g_underruns, 1U, __ATOMIC_RELAXED);
            }
        }

        __atomic_store_n(&g_pump_committing, 0, __ATOMIC_SEQ_CST);
        g_next_slot = (g_next_slot + 1U) % NUM_SLOTS;
        ring_write32(slot_end_off(g_next_slot), SENTINEL);
        dsp_wmb();

        if (!g_stop_req) {
            if (was_late) {
                /* Resync phase cleanly after a late wakeup */
                usleep_range_state(12000, 12500, TASK_IDLE);
            } else {
                u64 after_ns = (u64)ktime_get();
                u64 work_us = (after_ns > flip_ns) ? ((after_ns - flip_ns) / 1000ULL) : 0ULL;
                if (work_us + 1000ULL < target_lead_us) {
                    unsigned long slp_max = target_lead_us - (unsigned long)work_us;
                    unsigned long slp_min = (slp_max > 250UL) ? (slp_max - 250UL) : slp_max;
                    usleep_range_state(slp_min, slp_max, TASK_IDLE);
                }
            }
        }
    }

    if (locked && ring_read32(slot_end_off(g_next_slot)) == SENTINEL) {
        ring_write32(slot_end_off(g_next_slot), 0);
        dsp_wmb();
    }

    __atomic_store_n(&g_thread_alive, 0, __ATOMIC_RELEASE);
    complete_all(&g_space_comp);
    complete_all(&g_drain_comp);
    return 0;
}

static void stop_pump_and_wait(void)
{
    __atomic_store_n(&g_stop_req, 1, __ATOMIC_RELEASE);
    complete_all(&g_space_comp);
    complete_all(&g_drain_comp);

    for (u32 w = 0; w < 400; w++) {
        if (!__atomic_load_n(&g_thread_alive, __ATOMIC_ACQUIRE))
            break;
        usleep_range_state(500, 1000, TASK_UNINTERRUPTIBLE);
    }
}

static int dev_open(struct inode *inode, struct file *filp)
{
    (void)inode;

    /* Mark file as non-positional stream so concurrent read() and write() never contend on f_pos_lock */
    u32 *p_mode = (u32 *)((u8 *)filp + FILE_OFF_F_MODE);
    *p_mode = (*p_mode & FMODE_STREAM_CLR) | FMODE_STREAM_SET;
    *(void **)((u8 *)filp + FILE_OFF_PRIV) = (void *)0;

    /* O_RDONLY: stateless, non-exclusive <5 us hardware + ring probe & telemetry fd */
    if ((*p_mode & FMODE_WRITE_BIT) == 0U) {
        if (!earc_hw_npcm_active() || find_valid_ring_base() == 0xffffffffU)
            return -19; /* -ENODEV */
        return 0;
    }

    /* O_WRONLY / O_RDWR: exclusive streaming writer with atomic acquire + brief close-handoff wait */
    int acquired = 0;
    for (u32 w = 0; w < 100; w++) {
        if (__atomic_exchange_n(&g_open, 1, __ATOMIC_ACQ_REL) == 0) {
            acquired = 1;
            break;
        }
        usleep_range_state(2000, 3000, TASK_UNINTERRUPTIBLE);
    }
    if (!acquired)
        return -16; /* -EBUSY */

    g_q_head = 0;
    g_q_tail = 0;
    g_partial = 0;
    g_paused = 0;
    g_frames_out = 0;
    g_last_slot_ns = 0;
    g_underruns = 0;
    g_already_ready = 0;
    g_relocks = 0;
    g_header_changes = 0;
    g_stop_req = 0;
    g_prelocked = 0;
    g_space_comp.done = 0;
    g_drain_comp.done = 0;
    smp_full_mb();

    /* Fast ~35ms lock onto DEC-R2's live 20ms slot cadence (fails immediately with -ENODEV if inactive) */
    if (lock_onto_ring_timeout(160) != 0) {
        _printk("\0016[tcl_earc_mat] open rejected: eARC MAT ring inactive (-ENODEV)\n");
        __atomic_store_n(&g_open, 0, __ATOMIC_RELEASE);
        return -19; /* -ENODEV */
    }
    g_prelocked = 1;
    __atomic_store_n(&g_thread_alive, 1, __ATOMIC_RELEASE);

    struct task_struct *t = kthread_create_on_node(mat_pump_thread, 0, NUMA_NO_NODE, "earc_mat_pump");
    if ((unsigned long)t >= (unsigned long)-4095L) {
        if (ring_read32(slot_end_off(g_next_slot)) == SENTINEL) {
            ring_write32(slot_end_off(g_next_slot), 0);
            dsp_wmb();
        }
        __atomic_store_n(&g_thread_alive, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_open, 0, __ATOMIC_RELEASE);
        return (int)(long)t;
    }

    *(void **)((u8 *)filp + FILE_OFF_PRIV) = (void *)1UL;
    wake_up_process(t);
    return 0;
}

static int dev_release(struct inode *inode, struct file *filp)
{
    (void)inode;
    if (*(void **)((u8 *)filp + FILE_OFF_PRIV) != (void *)1UL)
        return 0;

    /* Stop immediately on close() so pause/seek silences within <20ms */
    stop_pump_and_wait();

    _printk("\0016[tcl_earc_mat] closed: frames=%llu underruns=%u already_ready=%u relocks=%u header_changes=%u\n",
            (unsigned long long)g_frames_out, g_underruns, g_already_ready, g_relocks, g_header_changes);
    *(void **)((u8 *)filp + FILE_OFF_PRIV) = (void *)0;
    __atomic_store_n(&g_open, 0, __ATOMIC_RELEASE);
    return 0;
}

static int dev_fsync(struct file *filp, loff_t start, loff_t end, int datasync)
{
    (void)start;
    (void)end;
    (void)datasync;
    if (*(void **)((u8 *)filp + FILE_OFF_PRIV) != (void *)1UL)
        return 0;

    /* Wait until earc_mat_pump commits all queued complete MAT frames to the hardware DDR ring */
    for (u32 iter = 0; iter < 4; iter++) {
        u32 head = __atomic_load_n(&g_q_head, __ATOMIC_ACQUIRE);
        u32 tail = __atomic_load_n(&g_q_tail, __ATOMIC_ACQUIRE);
        if (tail == head || !__atomic_load_n(&g_thread_alive, __ATOMIC_ACQUIRE) || g_stop_req)
            return 0;

        *(volatile u32 *)&g_drain_comp.done = 0;
        smp_full_mb();
        head = __atomic_load_n(&g_q_head, __ATOMIC_ACQUIRE);
        tail = __atomic_load_n(&g_q_tail, __ATOMIC_ACQUIRE);
        if (tail == head || !__atomic_load_n(&g_thread_alive, __ATOMIC_ACQUIRE) || g_stop_req)
            return 0;

        long rem = wait_for_completion_interruptible_timeout(&g_drain_comp, 125);
        if (rem < 0)
            return (int)rem;
    }
    return 0;
}

static ssize_t dev_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    (void)iocb;
    struct earc_mat_status st;
    u32 head = __atomic_load_n(&g_q_head, __ATOMIC_ACQUIRE);
    u32 tail = __atomic_load_n(&g_q_tail, __ATOMIC_ACQUIRE);

    st.frames_out        = __atomic_load_n(&g_frames_out, __ATOMIC_ACQUIRE);
    st.last_slot_mono_ns = __atomic_load_n(&g_last_slot_ns, __ATOMIC_ACQUIRE);
    st.q_depth           = head - tail;
    st.underruns         = __atomic_load_n(&g_underruns, __ATOMIC_RELAXED);
    st.already_ready     = __atomic_load_n(&g_already_ready, __ATOMIC_RELAXED);
    st.relocks           = __atomic_load_n(&g_relocks, __ATOMIC_RELAXED);
    st.ring_active       = (__atomic_load_n(&g_thread_alive, __ATOMIC_ACQUIRE) && earc_hw_npcm_active()) ? 1U : 0U;
    st.header_changes    = __atomic_load_n(&g_header_changes, __ATOMIC_RELAXED);

    size_t copied = _copy_to_iter(&st, sizeof(st), to);
    return (ssize_t)copied;
}

static ssize_t dev_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    (void)iocb;
    size_t total = 0;

    if (!__atomic_load_n(&g_thread_alive, __ATOMIC_ACQUIRE))
        return -19; /* -ENODEV */

    while (1) {
        while ((u32)(__atomic_load_n(&g_q_head, __ATOMIC_RELAXED) -
                     __atomic_load_n(&g_q_tail, __ATOMIC_ACQUIRE)) >= Q_SLOTS) {
            /* Binary-armed sleep: prevents completion->done accumulation spin after long playback */
            *(volatile u32 *)&g_space_comp.done = 0;
            smp_full_mb();
            if (!__atomic_load_n(&g_thread_alive, __ATOMIC_ACQUIRE) || g_stop_req)
                return total ? (ssize_t)total : -19;
            if ((u32)(__atomic_load_n(&g_q_head, __ATOMIC_RELAXED) -
                      __atomic_load_n(&g_q_tail, __ATOMIC_ACQUIRE)) < Q_SLOTS)
                break;

            long rem = wait_for_completion_interruptible_timeout(&g_space_comp, 125);
            if (rem < 0)
                return total ? (ssize_t)total : (ssize_t)rem;
            if (!__atomic_load_n(&g_thread_alive, __ATOMIC_ACQUIRE) || g_stop_req)
                return total ? (ssize_t)total : -19;
            if (rem == 0 &&
                (u32)(__atomic_load_n(&g_q_head, __ATOMIC_RELAXED) -
                      __atomic_load_n(&g_q_tail, __ATOMIC_ACQUIRE)) >= Q_SLOTS)
                return total ? (ssize_t)total : -110; /* -ETIMEDOUT */
        }

        u32 head = __atomic_load_n(&g_q_head, __ATOMIC_RELAXED);
        u8 *dst = g_q_buf + (u64)(head & Q_MASK) * SLOT_SIZE + g_partial;
        size_t want = (size_t)SLOT_SIZE - g_partial;
        size_t n = _copy_from_iter(dst, want, from);
        if (n == 0)
            break;

        total += n;
        g_partial += (u32)n;
        if (g_partial == (u32)SLOT_SIZE) {
            __atomic_store_n(&g_q_head, head + 1U, __ATOMIC_RELEASE);
            g_partial = 0;
        }
    }

    return (ssize_t)total;
}

static long dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    (void)arg;
    if (cmd != EARC_MAT_PAUSE && cmd != EARC_MAT_RESUME)
        return -25; /* ENOTTY */
    if (*(void **)((u8 *)filp + FILE_OFF_PRIV) != (void *)1UL)
        return -9; /* EBADF */
    __atomic_store_n(&g_paused, cmd == EARC_MAT_PAUSE, __ATOMIC_SEQ_CST);
    while (cmd == EARC_MAT_PAUSE && __atomic_load_n(&g_pump_committing, __ATOMIC_SEQ_CST))
        usleep_range_state(100, 200, TASK_IDLE);
    return 0;
}

struct v670_module {
    u8 pad0[0x18];
    char name[56];
    u8 pad1[0x178 - 0x18 - 56];
    int (*init)(void);
    u8 pad2[0x378 - 0x178 - 8];
    void (*exit)(void);
    u8 pad3[0x3c0 - 0x378 - 8];
};

extern struct v670_module __this_module;

/* Exact BTF-verified struct file_operations (size=0x120) for V670 5.15.192-android14-11 */
struct v670_file_operations {
    struct v670_module *owner;                                     /* +0x000 */
    u64 llseek;                                                    /* +0x008 */
    u64 read;                                                      /* +0x010 */
    u64 write;                                                     /* +0x018 */
    ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);       /* +0x020 */
    ssize_t (*write_iter)(struct kiocb *, struct iov_iter *);      /* +0x028 */
    u8 pad1[0x50 - 0x30];
    long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
    long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
    u8 mmap_fields[0x70 - 0x60];                                          /* +0x030..+0x06f */
    int (*open)(struct inode *, struct file *);                    /* +0x070 */
    u64 flush;                                                     /* +0x078 */
    int (*release)(struct inode *, struct file *);                 /* +0x080 */
    int (*fsync)(struct file *, loff_t, loff_t, int);              /* +0x088 */
    u8 pad2[0x120 - 0x90];                                         /* +0x090..+0x11f */
};

static const struct v670_file_operations earc_fops = {
    .owner      = &__this_module,
    .read_iter  = dev_read_iter,
    .write_iter = dev_write_iter,
    .unlocked_ioctl = dev_ioctl,
    .compat_ioctl = dev_ioctl,
    .open       = dev_open,
    .release    = dev_release,
    .fsync      = dev_fsync,
};

struct v670_miscdevice {
    int minor;
    u32 pad0;
    const char *name;
    const struct v670_file_operations *fops;
    u8 rest[0x50 - 0x18];
};

static struct v670_miscdevice earc_miscdev = {
    .minor = 255,
    .name  = "earc_mat_bypass",
    .fops  = &earc_fops,
};

int init_module(void)
{
    u64 prot_nc  = 0x006800000000070bULL; /* MT_NORMAL_NC */
    u64 prot_dev = 0x0068000000000713ULL; /* MT_DEVICE_nGnRnE (matches MDrv_MMIO_Get_AU_BASE) */
    if (arm64_use_ng_mappings) {
        prot_nc  |= 0x800ULL;
        prot_dev |= 0x800ULL;
    }

    g_ring = (volatile u8 *)__ioremap(RING_PHYS, RING_SIZE, prot_nc);
    if (!g_ring)
        return -12;

    g_riu_vivaldi3 = (volatile u8 *)__ioremap(VIVALDI3_PHYS, VIVALDI3_SIZE, prot_dev);
    g_riu_earc_cfg = (volatile u8 *)__ioremap(EARC_CFG_PHYS, EARC_CFG_SIZE, prot_dev);

    g_q_buf = (u8 *)vmalloc((u64)Q_SLOTS * SLOT_SIZE);
    if (!g_q_buf) {
        if (g_riu_earc_cfg) { iounmap(g_riu_earc_cfg); g_riu_earc_cfg = 0; }
        if (g_riu_vivaldi3) { iounmap(g_riu_vivaldi3); g_riu_vivaldi3 = 0; }
        iounmap(g_ring);
        g_ring = 0;
        return -12;
    }

    g_space_comp.done = 0;
    g_drain_comp.done = 0;
    __init_swait_queue_head(g_space_comp.swait_head, "earc_mat_q", g_space_lock_key);
    __init_swait_queue_head(g_drain_comp.swait_head, "earc_mat_d", g_drain_lock_key);

    int rc = misc_register(&earc_miscdev);
    if (rc) {
        vfree(g_q_buf);
        if (g_riu_earc_cfg) { iounmap(g_riu_earc_cfg); g_riu_earc_cfg = 0; }
        if (g_riu_vivaldi3) { iounmap(g_riu_vivaldi3); g_riu_vivaldi3 = 0; }
        iounmap(g_ring);
        g_q_buf = 0;
        g_ring = 0;
        return rc;
    }

    _printk("\0016[tcl_earc_mat] READY: /dev/earc_mat_bypass (queue=%u frames / %ums, riu_guard=%d)\n",
            Q_SLOTS, Q_SLOTS * 20U, (g_riu_vivaldi3 && g_riu_earc_cfg) ? 1 : 0);
    return 0;
}

void cleanup_module(void)
{
    stop_pump_and_wait();
    misc_deregister(&earc_miscdev);
    if (g_q_buf) {
        vfree(g_q_buf);
        g_q_buf = 0;
    }
    if (g_riu_earc_cfg) {
        iounmap(g_riu_earc_cfg);
        g_riu_earc_cfg = 0;
    }
    if (g_riu_vivaldi3) {
        iounmap(g_riu_vivaldi3);
        g_riu_vivaldi3 = 0;
    }
    if (g_ring) {
        iounmap(g_ring);
        g_ring = 0;
    }
    _printk("\0016[tcl_earc_mat] unloaded\n");
}

__attribute__((section(".gnu.linkonce.this_module"), used, aligned(64)))
struct v670_module __this_module = {
    .name = "tcl_earc_mat",
    .init = init_module,
    .exit = cleanup_module,
};

struct modversion_info {
    u64 crc;
    char name[56];
};

__attribute__((section("__versions"), used, aligned(8)))
static const struct modversion_info ____versions[] = {
    { 0xb82e8f7bULL, "module_layout" },
    { 0x92997ed8ULL, "_printk" },
    { 0x6b4b2933ULL, "__ioremap" },
    { 0xedc03953ULL, "iounmap" },
    { 0xd6ee688fULL, "vmalloc" },
    { 0x999e8297ULL, "vfree" },
    { 0xc3055d20ULL, "usleep_range_state" },
    { 0xaf56600aULL, "arm64_use_ng_mappings" },
    { 0x1e85aa79ULL, "misc_register" },
    { 0xe1d7aeb8ULL, "misc_deregister" },
    { 0x4efa65a3ULL, "_copy_from_iter" },
    { 0x43ff96bfULL, "_copy_to_iter" },
    { 0x96517739ULL, "kthread_create_on_node" },
    { 0xb268478cULL, "wake_up_process" },
    { 0x608741b5ULL, "__init_swait_queue_head" },
    { 0xa6257a2fULL, "complete" },
    { 0x93d6dd8cULL, "complete_all" },
    { 0xf02aa937ULL, "wait_for_completion_interruptible_timeout" },
    { 0xb43f9365ULL, "ktime_get" },
    { 0x14a6b645ULL, "sched_set_fifo_low" },
};

__attribute__((section(".modinfo"), used)) static const char __mi1[] = "license=GPL";
__attribute__((section(".modinfo"), used)) static const char __mi2[] = "name=tcl_earc_mat";
__attribute__((section(".modinfo"), used)) static const char __mi3[] = "vermagic=5.15.192-android14-11 SMP preempt mod_unload modversions aarch64";
