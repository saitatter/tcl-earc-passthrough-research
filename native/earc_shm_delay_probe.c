/*
 * One-shot read-only probe: locate SND-R2 shared memory and dump channel delays (V670).
 * HAL_SND_R2_Set_SHM_PARAM ids 0xC9..0xCD store at SHM+0x198..+0x1A8 (ch8 = 0xCD = +0x1A8).
 * SHM is identified by SER2_HDMI_NONPCM_SIZE (+0x8B0) == 0x1A4000 and OFFSET (+0x8B4) == 0x98C000.
 * init returns -EAGAIN so the module never stays loaded.
 */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

extern int _printk(const char *fmt, ...);
extern void *__ioremap(u64 phys_addr, u64 size, u64 prot);
extern void iounmap(volatile void *addr);
extern u8 arm64_use_ng_mappings;

__attribute__((section(".plt"), used)) void __plt_stub(void) {}
__attribute__((section(".init.plt"), used)) void __init_plt_stub(void) {}
__attribute__((section(".text.ftrace_trampoline"), used)) void __ftrace_trampoline_stub(void) {}
__attribute__((aligned(4096), used)) void __cfi_check(u64 id, void *ptr, void *diag) { (void)id; (void)ptr; (void)diag; }

int init_module(void)
{
    u64 prot = 0x006800000000070bULL | (arm64_use_ng_mappings ? 0x800ULL : 0ULL);
    u64 phys_base = 0x20200000ULL, size = 0x03400000ULL; /* MI_MAD_ADV_BUF */
    volatile u8 *base = (volatile u8 *)__ioremap(phys_base, size, prot);
    if (!base)
        return -12;
    u32 hits = 0;
    for (u64 off = 0; off + 0x1000 <= size; off += 0x1000) {
        volatile u32 *shm = (volatile u32 *)(base + off);
        if ((shm[0x8b0 / 4] == 0x1A4000U && shm[0x8b4 / 4] == 0x98C000U) ||
            (shm[0x198 / 4] == 286U && shm[0x1a0 / 4] == 286U)) {
            hits++;
            _printk("\0016[earc_shm_probe] SHM phys=0x%llx delays 198=%u 19c=%u 1a0=%u 1a4=%d 1a8=%u wrptr=0x%x sz=0x%x off=0x%x\n",
                    phys_base + off, shm[0x198 / 4], shm[0x19c / 4], shm[0x1a0 / 4], (int)shm[0x1a4 / 4],
                    shm[0x1a8 / 4], shm[0x8ac / 4], shm[0x8b0 / 4], shm[0x8b4 / 4]);
        }
    }
    _printk("\0016[earc_shm_probe] scan done hits=%u\n", hits);
    iounmap(base);
    return -11;
}

void cleanup_module(void) {}

struct v670_module { u8 p0[0x18]; char name[56]; u8 p1[0x178-0x50]; int (*init)(void); u8 p2[0x378-0x180]; void (*exit)(void); u8 p3[0x40]; };
__attribute__((section(".gnu.linkonce.this_module"), used)) struct v670_module __this_module = { .name = "earc_shm_probe", .init = init_module, .exit = cleanup_module };
struct modversion_info { u64 crc; char name[56]; };
__attribute__((section("__versions"), used, aligned(8))) static const struct modversion_info ____versions[] = {
    { 0xb82e8f7bULL, "module_layout" }, { 0x92997ed8ULL, "_printk" },
    { 0x6b4b2933ULL, "__ioremap" }, { 0xedc03953ULL, "iounmap" }, { 0xaf56600aULL, "arm64_use_ng_mappings" },
};
__attribute__((section(".modinfo"), used, aligned(1))) static const char m1[] = "license=GPL v2";
__attribute__((section(".modinfo"), used, aligned(1))) static const char m2[] = "vermagic=5.15.192-android14-11 SMP preempt mod_unload modversions aarch64";
__attribute__((section(".modinfo"), used, aligned(1))) static const char m3[] = "name=earc_shm_probe";
__attribute__((section(".modinfo"), used, aligned(1))) static const char m4[] = "depends=";
