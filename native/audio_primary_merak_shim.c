/*
 * audio.primary.merak.so shim for TCL C9K (MT5896 / V670, 32-bit ARM Android 14).
 *
 * Wraps the stock vendor HAL (/vendor/lib/hw/audio.primary.merak.orig.so):
 * 1. Intercepts AUDIO_FORMAT_IEC61937 (0x0D000000) 192 kHz 8-ch (MAT 2.0 HBR) output
 *    streams (Kodi AudioTrack IEC, AudioProbe, etc.) -> /dev/earc_mat_bypass directly.
 * 2. Intercepts AUDIO_FORMAT_DOLBY_TRUEHD (0x0E000000) raw TrueHD streams (48/96/192 kHz,
 *    Moonfin, Jellyfin, Plex, Stremio, ExoPlayer, AudioProbe TRUEHD_RAW) -> packs TrueHD
 *    access units dynamically via IEC 61937-9 / Dolby MAT 2.0 input_timing & output_timing
 *    (supporting UHD Blu-ray peak AUs > 2,560 B up to 8,190 B across MAT code boundaries)
 *    into bit-exact 61,440-byte MAT 2.0 bursts -> /dev/earc_mat_bypass.
 * 3. Reads hardware-clocked slot telemetry (frames_out + ktime_get() CLOCK_MONOTONIC ns)
 *    directly from /dev/earc_mat_bypass for queue-depth-independent A/V lip-sync.
 * 4. Multi-stream safe registry (handles gapless / overlapping open_output_stream before
 *    close_output_stream without SIGSEGV or false stock-HAL fallback), plus lossless
 *    end-of-stream drain via partial MAT tail flush + fsync().
 * 5. Passes all other streams (PCM, AC3, E-AC-3, DTS, 44.1k-family TrueHD, UI, mic)
 *    untouched to the stock vendor HAL.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

#include <sys/ioctl.h>
#include "earc_mat_control.h"
#define LOG_TAG "earc_mat_hal"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define ORIG_HAL_PATH "/vendor/lib/hw/audio.primary.merak.orig.so"
#define BYPASS_DEV    "/dev/earc_mat_bypass"

#define HARDWARE_MODULE_TAG          0x48574d54U /* 'TMWH' */
#define AUDIO_MODULE_API_VERSION_0_1 0x0001
#define HARDWARE_HAL_API_VERSION     0x0100

#define AUDIO_FORMAT_IEC61937        0x0D000000U
#define AUDIO_FORMAT_DOLBY_TRUEHD    0x0E000000U
#define AUDIO_CHANNEL_OUT_7POINT1    0x0000063FU
#define AUDIO_DEVICE_OUT_HDMI_EARC   0x00040001U

#define MAT_FRAME_BYTES              61440U
#define MAT_PAYLOAD_BYTES            61424U
#define MAT_BYTES_PER_FRAME          16U     /* 8 ch * 16-bit */
#define MAT_SAMPLE_RATE              192000U
#define TRUEHD_NOMINAL_AU_SPACING    2560U
#define TRUEHD_MAX_AU_BYTES          8190U   /* 12-bit word length max: 4095 * 2 */
#define TRUEHD_ACC_MAX               16384U
#define MAX_SHIM_STREAMS             8U
#define MAT_STREAM_MAGIC             0x4D415453U /* 'MATS' */

#define KERNEL_Q_BURSTS              32U     /* matches Q_SLOTS=32 in tcl_earc_mat.c */
#define DEFAULT_PIPE_DELAY_MS        0       /* manual trim; the video A/V delay is added automatically */
#define STATUS_V1_BYTES              40
#define STATUS_V2_BYTES              48

/* Bit-exact IEC61937-9 / Dolby MAT 2.0 constants (pre-swapped 16-bit LE, verified against FFmpeg spdifenc.c) */
static const uint8_t IEC61937_MAT_HDR[8] = {
    0x72, 0xf8, 0x1f, 0x4e, 0x16, 0x00, 0xf0, 0xef
};
static const uint8_t MAT_START_CODE[20] = {
    0x9e, 0x07, 0x03, 0x00, 0x01, 0x84, 0x01, 0x01,
    0x00, 0x80, 0xa5, 0x56, 0xf4, 0x3b, 0x83, 0x81,
    0x80, 0x49, 0xe0, 0x77
};
static const uint8_t MAT_MIDDLE_CODE[12] = {
    0xc1, 0xc3, 0x49, 0x42, 0xfa, 0x3b, 0x83, 0x82,
    0x80, 0x49, 0xe0, 0x77
};
static const uint8_t MAT_END_CODE[16] = {
    0xc2, 0xc3, 0xc4, 0xc0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x97
};

struct mat_code_desc {
    uint32_t pos;
    uint32_t len;
    const uint8_t *code;
};

static const struct mat_code_desc MAT_CODES[3] = {
    { 0U,     sizeof(MAT_START_CODE),  MAT_START_CODE  },
    { 30708U, sizeof(MAT_MIDDLE_CODE), MAT_MIDDLE_CODE },
    { 61408U, sizeof(MAT_END_CODE),    MAT_END_CODE    },
};

/* Fixed-layout status ABI returned by read(/dev/earc_mat_bypass): 40 bytes (v1) or 48 bytes (v2) */
struct earc_mat_status {
    uint64_t frames_out;
    uint64_t last_slot_mono_ns;
    uint32_t q_depth;
    uint32_t underruns;
    uint32_t already_ready;
    uint32_t relocks;
    uint32_t ring_active;
    uint32_t header_changes;
    uint32_t av_delay_ms;        /* v2: stock A/V lip-sync delay the TV applies to PCM while video plays */
    uint32_t reserved;
};

/* Minimal 32-bit AOSP hardware/hardware.h + hardware/audio.h ABI (verified against audio.primary.merak.so) */
struct hw_module_t;
struct hw_device_t;

struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id, struct hw_device_t **device);
};

struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint32_t reserved[32 - 7];
};

struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint32_t reserved[12];
    int (*close)(struct hw_device_t *device);
};

struct audio_config {
    uint32_t sample_rate;
    uint32_t channel_mask;
    uint32_t format;
    uint8_t  offload_info[64];
    uint32_t frame_count;
};

struct audio_stream {
    uint32_t (*get_sample_rate)(const struct audio_stream *stream);
    int      (*set_sample_rate)(struct audio_stream *stream, uint32_t rate);
    size_t   (*get_buffer_size)(const struct audio_stream *stream);
    uint32_t (*get_channels)(const struct audio_stream *stream);
    uint32_t (*get_format)(const struct audio_stream *stream);
    int      (*set_format)(struct audio_stream *stream, uint32_t format);
    int      (*standby)(struct audio_stream *stream);
    int      (*dump)(const struct audio_stream *stream, int fd);
    uint32_t (*get_device)(const struct audio_stream *stream);
    int      (*set_device)(struct audio_stream *stream, uint32_t device);
    int      (*set_parameters)(struct audio_stream *stream, const char *kv_pairs);
    char *   (*get_parameters)(const struct audio_stream *stream, const char *keys);
    int      (*add_audio_effect)(const struct audio_stream *stream, void *effect);
    int      (*remove_audio_effect)(const struct audio_stream *stream, void *effect);
};

struct audio_stream_out {
    struct audio_stream common;                                                         /* +0x00 .. +0x37 */
    uint32_t (*get_latency)(const struct audio_stream_out *stream);                     /* +0x38 */
    int      (*set_volume)(struct audio_stream_out *stream, float left, float right);   /* +0x3c */
    ssize_t  (*write)(struct audio_stream_out *stream, const void *buffer, size_t bytes);/* +0x40 */
    int      (*get_render_position)(const struct audio_stream_out *stream, uint32_t *dsp_frames); /* +0x44 */
    int      (*get_next_write_timestamp)(const struct audio_stream_out *stream, int64_t *ts);     /* +0x48 */
    int      (*set_callback)(struct audio_stream_out *stream, void *cb, void *cookie);  /* +0x4c */
    int      (*pause)(struct audio_stream_out *stream);                                 /* +0x50 */
    int      (*resume)(struct audio_stream_out *stream);                                /* +0x54 */
    int      (*drain)(struct audio_stream_out *stream, int type);                       /* +0x58 */
    int      (*flush)(struct audio_stream_out *stream);                                 /* +0x5c */
    int      (*get_presentation_position)(const struct audio_stream_out *stream,
                                          uint64_t *frames, struct timespec *timestamp);/* +0x60 */
    void    *extra[40];
};

struct audio_hw_device {
    struct hw_device_t common;                                                          /* +0x00 .. +0x3f */
    uint32_t (*get_supported_devices)(const struct audio_hw_device *dev);               /* +0x40 */
    int      (*init_check)(const struct audio_hw_device *dev);                          /* +0x44 */
    int      (*set_voice_volume)(struct audio_hw_device *dev, float volume);            /* +0x48 */
    int      (*set_master_volume)(struct audio_hw_device *dev, float volume);           /* +0x4c */
    int      (*get_master_volume)(struct audio_hw_device *dev, float *volume);          /* +0x50 */
    int      (*set_mode)(struct audio_hw_device *dev, int mode);                        /* +0x54 */
    int      (*set_mic_mute)(struct audio_hw_device *dev, int state);                   /* +0x58 */
    int      (*get_mic_mute)(const struct audio_hw_device *dev, int *state);            /* +0x5c */
    int      (*set_parameters)(struct audio_hw_device *dev, const char *kv_pairs);      /* +0x60 */
    char *   (*get_parameters)(const struct audio_hw_device *dev, const char *keys);    /* +0x64 */
    size_t   (*get_input_buffer_size)(const struct audio_hw_device *dev,
                                      const struct audio_config *config);               /* +0x68 */
    int      (*open_output_stream)(struct audio_hw_device *dev, int handle,
                                   uint32_t devices, int flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address);                                /* +0x6c */
    void     (*close_output_stream)(struct audio_hw_device *dev,
                                    struct audio_stream_out *stream_out);               /* +0x70 */
};

struct mat_stream_out {
    struct audio_stream_out stream;
    uint32_t magic;
    pthread_mutex_t lock;
    pthread_cond_t io_idle;
    int fd;
    int can_read_status;
    int is_raw_truehd;
    uint32_t sample_rate;
    uint32_t channel_mask;
    uint32_t format;
    uint32_t devices;

    /* Hardware-clocked presentation position tracking (monotonic across pause/resume) */
    uint64_t written_frames;
    uint64_t base_frames;        /* preserved across pause/resume; reset only on flush() */
    uint64_t last_presented;     /* monotonic clamp */
    uint64_t session_bursts;     /* 20ms MAT bursts written since open/resume */
    uint64_t hw_consumed_bursts; /* bursts pumped into DDR by earc_mat_pump */
    uint64_t last_hw_sync_ns;    /* CLOCK_MONOTONIC ns of last pumped DDR slot */
    uint64_t session_start_ns;   /* CLOCK_MONOTONIC at first write of session */
    uint64_t resume_ns;          /* CLOCK_MONOTONIC of last resume (interpolation floor) */
    int      auto_av_ms;         /* stock A/V delay latched for this session */
    int      auto_av_latched;
    uint32_t last_k_av_delay;
    uint32_t clamp_hits;         /* presentation clamped at written_frames (offset deeper than queue) */
    uint32_t resync_skips;       /* count of skipped bytes in TrueHD AU parser */
    uint32_t last_k_qdepth;
    uint32_t last_k_underruns;
    uint32_t last_k_late;
    uint32_t last_k_relocks;
    uint32_t last_k_hdr_changes;
    int in_write;                /* >0 while blocking write()/fsync() runs unlocked */
    int paused;
    int started;

    /* Dynamic IEC 61937-9 / Dolby MAT 2.0 input_timing & output_timing packer state */
    uint8_t  mat_buf[MAT_FRAME_BYTES]; /* [0..7]=IEC hdr, [8..61431]=MAT payload, [61432..61439]=0 */
    uint32_t mat_filled;               /* bytes filled in [0 .. MAT_PAYLOAD_BYTES) */
    uint16_t thd_prev_time;
    int      thd_prev_size;
    int      thd_samples_per_frame;
    uint16_t thd_output_timing;
    int      thd_output_timing_valid;
    int      thd_oi_delta;
    uint8_t  thd_acc[TRUEHD_ACC_MAX];
    size_t   thd_acc_len;
    int      thd_synced;
    uint32_t mat_bursts_emitted;
};

static void *g_orig_handle = NULL;
static struct hw_module_t *g_orig_hmi = NULL;
static char *(*g_orig_get_parameters)(const struct audio_hw_device *, const char *) = NULL;
static int   (*g_orig_open_output_stream)(struct audio_hw_device *, int, uint32_t, int,
                                          struct audio_config *, struct audio_stream_out **,
                                          const char *) = NULL;
static void  (*g_orig_close_output_stream)(struct audio_hw_device *, struct audio_stream_out *) = NULL;

static pthread_mutex_t g_active_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mat_stream_out *g_mat_streams[MAX_SHIM_STREAMS];

static void register_shim_stream(struct mat_stream_out *m)
{
    pthread_mutex_lock(&g_active_lock);
    for (uint32_t i = 0; i < MAX_SHIM_STREAMS; i++) {
        if (g_mat_streams[i] == NULL) {
            g_mat_streams[i] = m;
            break;
        }
    }
    pthread_mutex_unlock(&g_active_lock);
}

static struct mat_stream_out *unregister_shim_stream(struct audio_stream_out *out)
{
    struct mat_stream_out *found = NULL;
    pthread_mutex_lock(&g_active_lock);
    for (uint32_t i = 0; i < MAX_SHIM_STREAMS; i++) {
        if (g_mat_streams[i] && &g_mat_streams[i]->stream == out &&
            g_mat_streams[i]->magic == MAT_STREAM_MAGIC) {
            found = g_mat_streams[i];
            g_mat_streams[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_active_lock);
    return found;
}

static uint64_t now_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void mat_reset_packer_locked(struct mat_stream_out *m)
{
    memset(m->mat_buf, 0, MAT_FRAME_BYTES);
    m->mat_filled = 0;
    m->thd_prev_time = 0;
    m->thd_prev_size = 0;
    m->thd_samples_per_frame = 0;
    m->thd_output_timing = 0;
    m->thd_output_timing_valid = 0;
    m->thd_oi_delta = 0;
    m->thd_acc_len = 0;
    m->thd_synced = 0;
}

static int mat_get_pipe_delay_ms(void)
{
    char val[PROP_VALUE_MAX];
    if (__system_property_get("persist.tcl.earc_sync_ms", val) > 0)
        return atoi(val);
    if (__system_property_get("debug.earc_sync_ms", val) > 0)
        return atoi(val);
    return DEFAULT_PIPE_DELAY_MS;
}

/* persist.tcl.earc_auto_av=0 disables following the TV's video A/V delay (default on) */
static int mat_auto_av_enabled(void)
{
    char val[PROP_VALUE_MAX];
    if (__system_property_get("persist.tcl.earc_auto_av", val) > 0)
        return atoi(val) != 0;
    return 1;
}

/* Positive = report less presented (audio later is assumed); negative = report audio further ahead
   so video is rendered earlier to cover the TV's video processing delay. */
static int mat_effective_delay_ms(const struct mat_stream_out *m)
{
    return mat_get_pipe_delay_ms() - (m->auto_av_latched ? m->auto_av_ms : 0);
}

static uint64_t mat_frames_per_burst(const struct mat_stream_out *m)
{
    return m->is_raw_truehd ? (uint64_t)(m->sample_rate / 50U)
                            : (uint64_t)(MAT_FRAME_BYTES / MAT_BYTES_PER_FRAME);
}

static void mat_poll_kernel_status_locked(struct mat_stream_out *m)
{
    if (m->fd < 0 || !m->can_read_status)
        return;

    struct earc_mat_status st;
    memset(&st, 0, sizeof(st));
    ssize_t r = read(m->fd, &st, sizeof(st));
    if (r == (ssize_t)STATUS_V1_BYTES || r == (ssize_t)STATUS_V2_BYTES) {
        if (r == (ssize_t)STATUS_V2_BYTES) {
            m->last_k_av_delay = st.av_delay_ms;
            if (!m->auto_av_latched) {
                /* Latch per session/resume so a mid-stream change never yanks the clock backwards */
                m->auto_av_ms = mat_auto_av_enabled() ? (int)st.av_delay_ms : 0;
                m->auto_av_latched = 1;
                ALOGI("auto A/V delay latched: %d ms (stock=%u trim=%d effective=%d)",
                      m->auto_av_ms, st.av_delay_ms, mat_get_pipe_delay_ms(), mat_effective_delay_ms(m));
            }
        }
        m->hw_consumed_bursts = st.frames_out;
        if (st.last_slot_mono_ns != 0)
            m->last_hw_sync_ns = st.last_slot_mono_ns;
        m->last_k_qdepth    = st.q_depth;
        m->last_k_underruns = st.underruns;
        m->last_k_late        = st.already_ready;
        m->last_k_relocks     = st.relocks;
        m->last_k_hdr_changes = st.header_changes;
    } else if (r < 0 && (errno == EACCES || errno == EBADF || errno == EINVAL)) {
        m->can_read_status = 0;
    }
}

static uint64_t mat_calc_presented_locked(struct mat_stream_out *m, uint64_t now_ns)
{
    uint64_t pos = m->base_frames;
    if (m->paused)
        return m->last_presented;
    if (m->started) {
        mat_poll_kernel_status_locked(m);

        uint64_t fpb = mat_frames_per_burst(m);
        int64_t pipe_delay_frames = ((int64_t)mat_effective_delay_ms(m) * (int64_t)m->sample_rate) / 1000LL;
        int64_t raw_frames = (int64_t)(m->hw_consumed_bursts * fpb);
        /* After resume the kernel's last slot stamp predates the pause: interpolate from resume instead,
           otherwise the clock jumps a full burst forward and then stalls. */
        uint64_t anchor_ns = m->last_hw_sync_ns;
        if (m->resume_ns > anchor_ns)
            anchor_ns = m->resume_ns;
        if (m->hw_consumed_bursts > 0 && anchor_ns > 0 && now_ns > anchor_ns) {
            uint64_t dt_ns = now_ns - anchor_ns;
            uint64_t interp = (dt_ns * (uint64_t)m->sample_rate) / 1000000000ULL;
            if (interp > fpb)
                interp = fpb;
            raw_frames += (int64_t)interp;
        }
        int64_t adjusted = raw_frames - pipe_delay_frames;
        if (adjusted < 0)
            adjusted = 0;
        pos = m->base_frames + (uint64_t)adjusted;
    }
    if (pos > m->written_frames) {
        if (m->started && !m->paused && m->hw_consumed_bursts > 0)
            m->clamp_hits++;
        pos = m->written_frames;
    }
    if (pos < m->last_presented)
        pos = m->last_presented;
    m->last_presented = pos;
    return pos;
}

static void mat_close_fd_locked(struct mat_stream_out *m)
{
    /* Wait briefly if mat_write / mat_drain is currently unlocked inside write()/fsync() */
    for (int w = 0; w < 60 && m->in_write > 0; w++) {
        pthread_mutex_unlock(&m->lock);
        usleep(2000);
        pthread_mutex_lock(&m->lock);
    }
    if (m->started) {
        m->base_frames = m->written_frames;
        m->last_presented = m->base_frames;
    }
    if (m->fd >= 0) {
        close(m->fd);
        m->fd = -1;
        ALOGI("closed %s (mode=%s written=%llu presented=%llu mat_bursts=%u skips=%u k_underruns=%u k_late=%u)",
              BYPASS_DEV, m->is_raw_truehd ? "TRUEHD_RAW" : "IEC61937",
              (unsigned long long)m->written_frames,
              (unsigned long long)m->base_frames,
              m->mat_bursts_emitted, m->resync_skips,
              m->last_k_underruns, m->last_k_late);
    }
    m->paused = 0;
    m->started = 0;
    m->session_bursts = 0;
    m->hw_consumed_bursts = 0;
    m->last_hw_sync_ns = 0;
    m->session_start_ns = 0;
    m->resume_ns = 0;
    m->auto_av_latched = 0;
    m->last_k_qdepth = 0;
    /* Do NOT wipe mat_buf/thd_acc/timing state on pause/standby: AudioTrack resumes mid-AU without flushing!
       mat_reset_packer_locked() is called only on mat_flush() (seek/stop) or mat_drain() completion. */
}

static int mat_write_all_locked(struct mat_stream_out *m, const uint8_t *p, size_t left)
{
    if (m->fd < 0) {
        int can_read = 1;
        int fd_open = open(BYPASS_DEV, O_RDWR);
        if (fd_open < 0 && (errno == EACCES || errno == EPERM)) {
            fd_open = open(BYPASS_DEV, O_WRONLY);
            can_read = 0;
        }
        if (fd_open < 0) {
            int err = errno;
            ALOGE("failed to open %s: %s (%d)", BYPASS_DEV, strerror(err), err);
            pthread_mutex_unlock(&m->lock);
            usleep(20000);
            pthread_mutex_lock(&m->lock);
            return -err;
        }
        m->fd = fd_open;
        m->can_read_status = can_read;
        m->session_start_ns = now_monotonic_ns();
        m->auto_av_latched = 0;
        m->clamp_hits = 0;
        ALOGI("opened %s (%s) for %s stream (rate=%u ch=0x%x base=%llu)",
              BYPASS_DEV, can_read ? "O_RDWR" : "O_WRONLY",
              m->is_raw_truehd ? "TRUEHD_RAW->MAT" : "IEC61937_MAT",
              m->sample_rate, m->channel_mask, (unsigned long long)m->base_frames);
        mat_poll_kernel_status_locked(m); /* latch the A/V delay before the first position query */
    }

    int fd = m->fd;
    size_t total = left;

    /* Release m->lock around blocking write() so get_presentation_position/pause/binder never stall */
    m->in_write++;
    pthread_mutex_unlock(&m->lock);
    int io_err = 0;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            io_err = errno;
            break;
        }
        if (w == 0) break;
        p += (size_t)w;
        left -= (size_t)w;
    }
    pthread_mutex_lock(&m->lock);
    m->in_write--;
    pthread_cond_broadcast(&m->io_idle);

    if (io_err) {
        if (m->fd == fd) {
            ALOGE("write(%s) failed: %s", BYPASS_DEV, strerror(io_err));
            mat_close_fd_locked(m);
        }
        return -EPIPE;
    }

    if (m->fd == fd) {
        uint64_t bursts_now = (total >= MAT_FRAME_BYTES) ? (total / MAT_FRAME_BYTES) : 1ULL;
        m->session_bursts += bursts_now;
        m->started = 1;
        if (!m->can_read_status && m->session_bursts > KERNEL_Q_BURSTS) {
            m->hw_consumed_bursts = m->session_bursts - KERNEL_Q_BURSTS;
            m->last_hw_sync_ns = now_monotonic_ns();
        }
        if ((m->session_bursts % 500ULL) == 0ULL) {
            uint64_t now_ns = now_monotonic_ns();
            uint64_t wall_ms = (now_ns > m->session_start_ns) ? ((now_ns - m->session_start_ns) / 1000000ULL) : 0ULL;
            uint64_t aud_ms = m->session_bursts * 20ULL;
            uint64_t pres = mat_calc_presented_locked(m, now_ns);
            ALOGI("sync-diag: bursts=%llu k_out=%llu q=%u aud_ms=%llu wall_ms=%llu pres=%llu skips=%u u=%u late=%u av=%d/%u eff=%d clamp=%u",
                  (unsigned long long)m->session_bursts,
                  (unsigned long long)m->hw_consumed_bursts,
                  m->last_k_qdepth,
                  (unsigned long long)aud_ms,
                  (unsigned long long)wall_ms,
                  (unsigned long long)pres,
                  m->resync_skips,
                  m->last_k_underruns,
                  m->last_k_late,
                  m->auto_av_ms, m->last_k_av_delay,
                  mat_effective_delay_ms(m), m->clamp_hits);
        }
    }
    return 0;
}

/* Copy TrueHD AU bytes into MAT payload with 16-bit LE byte swap (32-bit rev16 fast path) */
static void copy_swap16(uint8_t *dst, const uint8_t *src, size_t len)
{
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t v;
        memcpy(&v, src + i, 4);
        v = ((v & 0x00ff00ffU) << 8) | ((v & 0xff00ff00U) >> 8);
        memcpy(dst + i, &v, 4);
    }
    for (; i + 1 < len; i += 2) {
        dst[i]     = src[i + 1];
        dst[i + 1] = src[i];
    }
    if (i < len) {
        dst[i]     = 0;
        dst[i + 1] = src[i];
    }
}

/* Bounded MSB-first bit reader for TrueHD Major Sync substream restart header (output_timing) */
struct bit_reader {
    const uint8_t *buf;
    size_t bit_pos;
    size_t bit_end;
};

static inline size_t br_left(const struct bit_reader *br)
{
    return (br->bit_pos < br->bit_end) ? (br->bit_end - br->bit_pos) : 0U;
}

static inline uint32_t br_get(struct bit_reader *br, unsigned int nbits)
{
    uint32_t val = 0;
    for (unsigned int i = 0; i < nbits; i++) {
        size_t p = br->bit_pos++;
        val = (val << 1) | ((br->buf[p >> 3] >> (7U - (p & 7U))) & 1U);
    }
    return val;
}

static inline void br_skip(struct bit_reader *br, size_t nbits)
{
    br->bit_pos += nbits;
}

struct thd_au_info {
    uint16_t input_timing;
    uint16_t output_timing;
    int      has_output_timing;
    int      samples_per_frame;
};

/* Parse TrueHD / MLP Access Unit timing & optional Major Sync restart-header output_timing */
static int truehd_parse_au_info(const uint8_t *au_data, size_t au_size, struct thd_au_info *au)
{
    memset(au, 0, sizeof(*au));
    if (au_size < 4U)
        return -1;

    au->input_timing = (uint16_t)(((uint16_t)au_data[2] << 8) | au_data[3]);

    const uint8_t *data = au_data + 4;
    size_t size = au_size - 4U;
    if (size < 6U || data[0] != 0xf8U || data[1] != 0x72U || data[2] != 0x6fU)
        return 0;

    uint8_t ratebits;
    if (data[3] == 0xbaU)
        ratebits = data[4] >> 4;
    else if (data[3] == 0xbbU)
        ratebits = data[5] >> 4;
    else
        return -1;

    /* 44.1 / 88.2 / 176.4 kHz family cannot be carried on a fixed 192.000 kHz 20ms ring */
    if ((ratebits & 0x08U) != 0U)
        return -2;

    au->samples_per_frame = 40 << (ratebits & 0x03U); /* 40 (48k), 80 (96k), or 160 (192k) */
    if (size < 28U)
        return 0;

    size_t major_sync_size = 28U;
    if (data[3] == 0xbaU && (data[25] & 1U)) {
        size_t ext_size = (size_t)(data[26] >> 4);
        major_sync_size += 2U + ext_size * 2U;
    }
    if (major_sync_size >= size)
        return 0;

    int num_substreams = (int)(data[16] >> 4);
    if (num_substreams <= 0)
        return 0;

    struct bit_reader br = {
        .buf     = data + major_sync_size,
        .bit_pos = 0U,
        .bit_end = (size - major_sync_size) * 8U,
    };

    for (int i = 0; i < num_substreams; i++) {
        if (br_left(&br) < 16U)
            return 0;
        uint32_t extra_word = br_get(&br, 1U);
        br_skip(&br, 15U);
        if (extra_word) {
            if (br_left(&br) < 16U)
                return 0;
            br_skip(&br, 16U);
        }
    }

    if (br_left(&br) < 2U)
        return 0;
    if (!br_get(&br, 1U)) /* !block_header_exists */
        return 0;
    if (!br_get(&br, 1U)) /* !restart_header_exists */
        return 0;
    if (br_left(&br) < (13U + 1U + 16U))
        return 0;

    uint32_t sync_word = br_get(&br, 13U);
    if (sync_word != (0x31eaU >> 1))
        return 0;
    br_skip(&br, 1U); /* noise_type */

    au->output_timing = (uint16_t)br_get(&br, 16U);
    au->has_output_timing = 1;
    return 0;
}

static int mat_emit_current_frame_locked(struct mat_stream_out *m)
{
    memcpy(m->mat_buf + 0, IEC61937_MAT_HDR, sizeof(IEC61937_MAT_HDR));
    memset(m->mat_buf + 8U + MAT_PAYLOAD_BYTES, 0, MAT_FRAME_BYTES - 8U - MAT_PAYLOAD_BYTES);

    int rc = mat_write_all_locked(m, m->mat_buf, MAT_FRAME_BYTES);
    memset(m->mat_buf, 0, MAT_FRAME_BYTES);
    m->mat_filled = 0;
    if (rc != 0)
        return rc;

    m->mat_bursts_emitted++;
    /* One 61,440-byte MAT 2.0 frame = 20.000 ms = sample_rate / 50 frames (960 @ 48k, 1920 @ 96k, 3840 @ 192k) */
    m->written_frames += (uint64_t)(m->sample_rate / 50U);
    return 0;
}

/*
 * Dynamic IEC 61937-9 / Dolby MAT 2.0 AU packer (matches FFmpeg spdif_header_truehd):
 * Places AUs at exact input_timing offsets, seamlessly splitting large UHD Remux AUs (>2560 B)
 * around MAT_MIDDLE_CODE (30708) and across consecutive 61,440-byte MAT frames.
 */
static int mat_pack_truehd_au_locked(struct mat_stream_out *m, const uint8_t *au_buf, size_t au_len)
{
    if (au_len < 10U)
        return 0;

    struct thd_au_info au;
    int prc = truehd_parse_au_info(au_buf, au_len, &au);
    if (prc < 0)
        return 0;

    if (au.samples_per_frame > 0)
        m->thd_samples_per_frame = au.samples_per_frame;
    if (m->thd_samples_per_frame <= 0)
        return 0;

    uint16_t input_timing = au.input_timing;
    int max_padding = (int)(MAT_PAYLOAD_BYTES * 2U);
    int padding_remaining = 0;
    int output_discontinuity = 0;

    m->thd_output_timing = (uint16_t)(m->thd_output_timing + (uint16_t)m->thd_samples_per_frame);
    if (au.has_output_timing) {
        if (m->thd_output_timing_valid && au.output_timing != m->thd_output_timing) {
            int bytes_per_sample = (int)TRUEHD_NOMINAL_AU_SPACING / m->thd_samples_per_frame;
            uint16_t out_minus_spf = (uint16_t)(au.output_timing - (uint16_t)m->thd_samples_per_frame);
            int prev_oi_delta = m->thd_oi_delta;
            int curr_oi_delta = (int)(int16_t)(out_minus_spf - input_timing);
            int prev_padding = m->thd_prev_size ? ((int)TRUEHD_NOMINAL_AU_SPACING - m->thd_prev_size) : 0;
            int disc_padding = (prev_oi_delta - curr_oi_delta) * bytes_per_sample;

            output_discontinuity = 1;
            int raw_padding = prev_padding + disc_padding;
            padding_remaining = raw_padding;
            if (padding_remaining < 0 || padding_remaining > max_padding)
                padding_remaining = 0;
            ALOGI("thd-disc: bursts=%u au_len=%zu exp_out=%u act_out=%u in=%u prev_oi=%d curr_oi=%d prev_pad=%d disc_pad=%d raw_pad=%d pad=%d",
                  m->mat_bursts_emitted, au_len, (unsigned)m->thd_output_timing, (unsigned)au.output_timing,
                  (unsigned)input_timing, prev_oi_delta, curr_oi_delta, prev_padding, disc_padding,
                  raw_padding, padding_remaining);
        }
        m->thd_output_timing = au.output_timing;
        m->thd_output_timing_valid = 1;
    }

    if (m->thd_prev_size > 0 && !output_discontinuity) {
        uint16_t delta_samples = (uint16_t)(input_timing - m->thd_prev_time);
        int delta_bytes = ((int)delta_samples * (int)TRUEHD_NOMINAL_AU_SPACING) / m->thd_samples_per_frame;
        int raw_padding = delta_bytes - m->thd_prev_size;
        padding_remaining = raw_padding;
        if (padding_remaining < 0 || padding_remaining > max_padding)
            padding_remaining = 0;
        if (raw_padding < 0 || raw_padding > max_padding ||
            delta_samples > (uint16_t)(m->thd_samples_per_frame * 8)) {
            ALOGI("thd-jump: bursts=%u au_len=%zu prev_in=%u in=%u delta_smp=%u prev_sz=%d raw_pad=%d pad=%d has_out=%d",
                  m->mat_bursts_emitted, au_len, (unsigned)m->thd_prev_time, (unsigned)input_timing,
                  (unsigned)delta_samples, m->thd_prev_size, raw_padding, padding_remaining, au.has_output_timing);
        }
    }

    if (m->thd_output_timing_valid) {
        uint16_t out_minus_spf = (uint16_t)(m->thd_output_timing - (uint16_t)m->thd_samples_per_frame);
        m->thd_oi_delta = (int)(int16_t)(out_minus_spf - input_timing);
    }

    uint32_t next_code_idx = 0;
    for (; next_code_idx < 3U; next_code_idx++) {
        if (m->mat_filled <= MAT_CODES[next_code_idx].pos)
            break;
    }
    if (next_code_idx >= 3U) {
        mat_reset_packer_locked(m);
        return 0;
    }

    int total_frame_size = (int)au_len;
    const uint8_t *dataptr = au_buf;
    int data_remaining = (int)au_len;
    uint8_t *payload = m->mat_buf + 8U;

    while (padding_remaining > 0 || data_remaining > 0 ||
           MAT_CODES[next_code_idx].pos == m->mat_filled) {

        if (MAT_CODES[next_code_idx].pos == m->mat_filled) {
            int code_len = (int)MAT_CODES[next_code_idx].len;
            int code_len_rem = code_len;
            memcpy(payload + MAT_CODES[next_code_idx].pos, MAT_CODES[next_code_idx].code, (size_t)code_len);
            m->mat_filled += (uint32_t)code_len;

            next_code_idx++;
            if (next_code_idx == 3U) {
                next_code_idx = 0U;
                int rc = mat_emit_current_frame_locked(m);
                if (rc != 0) {
                    mat_reset_packer_locked(m);
                    return rc;
                }
                payload = m->mat_buf + 8U;
                /* Account for the 16-byte IEC inter-frame header/tail gap (61440 - 61424) */
                code_len_rem += (int)(MAT_FRAME_BYTES - MAT_PAYLOAD_BYTES);
            }

            if (padding_remaining > 0) {
                int used_pad = (padding_remaining < code_len_rem) ? padding_remaining : code_len_rem;
                padding_remaining -= used_pad;
                code_len_rem -= used_pad;
            }
            if (code_len_rem > 0)
                total_frame_size += code_len_rem;
        }

        if (padding_remaining > 0) {
            int avail = (int)(MAT_CODES[next_code_idx].pos - m->mat_filled);
            int pad_now = (avail < padding_remaining) ? avail : padding_remaining;
            if (pad_now > 0) {
                memset(payload + m->mat_filled, 0, (size_t)pad_now);
                m->mat_filled += (uint32_t)pad_now;
                padding_remaining -= pad_now;
            }
            if (padding_remaining > 0)
                continue;
        }

        if (data_remaining > 0) {
            int avail = (int)(MAT_CODES[next_code_idx].pos - m->mat_filled);
            int ins_now = (avail < data_remaining) ? avail : data_remaining;
            if (ins_now > 0) {
                copy_swap16(payload + m->mat_filled, dataptr, (size_t)ins_now);
                m->mat_filled += (uint32_t)ins_now;
                dataptr += ins_now;
                data_remaining -= ins_now;
            }
        }
    }

    m->thd_prev_time = input_timing;
    m->thd_prev_size = total_frame_size;
    return 0;
}

/* Pad and flush any trailing partial MAT payload (<20ms tail) at end-of-stream drain() */
static int mat_flush_partial_tail_locked(struct mat_stream_out *m)
{
    if (!m->is_raw_truehd || m->mat_filled <= sizeof(MAT_START_CODE))
        return 0;

    uint8_t *payload = m->mat_buf + 8U;
    if (m->mat_filled <= MAT_CODES[1].pos) {
        memset(payload + m->mat_filled, 0, MAT_CODES[1].pos - m->mat_filled);
        memcpy(payload + MAT_CODES[1].pos, MAT_CODES[1].code, MAT_CODES[1].len);
        m->mat_filled = MAT_CODES[1].pos + MAT_CODES[1].len;
    }
    if (m->mat_filled <= MAT_CODES[2].pos) {
        memset(payload + m->mat_filled, 0, MAT_CODES[2].pos - m->mat_filled);
        memcpy(payload + MAT_CODES[2].pos, MAT_CODES[2].code, MAT_CODES[2].len);
        m->mat_filled = MAT_CODES[2].pos + MAT_CODES[2].len;
    }

    int rc = mat_emit_current_frame_locked(m);
    mat_reset_packer_locked(m);
    return rc;
}

static ssize_t mat_write_truehd_raw_locked(struct mat_stream_out *m, const uint8_t *buf, size_t bytes)
{
    size_t consumed = 0;
    while (consumed < bytes) {
        size_t space = TRUEHD_ACC_MAX - m->thd_acc_len;
        size_t take = bytes - consumed;
        if (take > space) take = space;
        memcpy(m->thd_acc + m->thd_acc_len, buf + consumed, take);
        m->thd_acc_len += take;
        consumed += take;

        size_t pos = 0;
        while (m->thd_acc_len - pos >= 10U) {
            const uint8_t *p = m->thd_acc + pos;
            if (!m->thd_synced) {
                /* Lock cleanly onto a TrueHD Major Sync (F8 72 6F BA at bytes 4..7, 48/96/192 kHz) */
                if (p[4] == 0xF8U && p[5] == 0x72U && p[6] == 0x6FU && p[7] == 0xBAU &&
                    ((p[8] >> 4) & 0x08U) == 0U) {
                    size_t au_len = (size_t)(((p[0] & 0x0fU) << 8) | p[1]) * 2U;
                    if (au_len >= 28U && au_len <= TRUEHD_MAX_AU_BYTES) {
                        m->thd_synced = 1;
                    } else {
                        m->resync_skips++;
                        pos++;
                        continue;
                    }
                } else {
                    m->resync_skips++;
                    pos++;
                    continue;
                }
            }
            /* TrueHD AU length in 16-bit words is in bits [11:0] of first 2 bytes (up to 8,190 B) */
            size_t au_len = (size_t)(((p[0] & 0x0fU) << 8) | p[1]) * 2U;
            if (au_len < 8U || au_len > TRUEHD_MAX_AU_BYTES) {
                /* Lost sync: reset MAT packer alignment and re-lock on next Major Sync */
                m->thd_synced = 0;
                m->thd_prev_size = 0;
                m->thd_output_timing_valid = 0;
                m->mat_filled = 0;
                memset(m->mat_buf, 0, MAT_FRAME_BYTES);
                m->resync_skips++;
                pos++;
                continue;
            }
            if (m->thd_acc_len - pos < au_len)
                break; /* wait for complete AU bytes */

            int rc = mat_pack_truehd_au_locked(m, p, au_len);
            if (rc != 0)
                return rc;
            pos += au_len;
        }

        if (pos > 0) {
            size_t rem = m->thd_acc_len - pos;
            if (rem > 0)
                memmove(m->thd_acc, m->thd_acc + pos, rem);
            m->thd_acc_len = rem;
        } else if (m->thd_acc_len == TRUEHD_ACC_MAX) {
            /* Prevent stall on corrupt input */
            m->thd_acc_len = 0;
            m->thd_synced = 0;
        }
    }
    return (ssize_t)bytes;
}

/* ---- audio_stream callbacks ---- */
static uint32_t mat_get_sample_rate(const struct audio_stream *s)
{
    return ((const struct mat_stream_out *)s)->sample_rate;
}
static int mat_set_sample_rate(struct audio_stream *s, uint32_t r) { (void)s; (void)r; return 0; }
static size_t mat_get_buffer_size(const struct audio_stream *s)
{
    const struct mat_stream_out *m = (const struct mat_stream_out *)s;
    return m->is_raw_truehd ? 4096U : MAT_FRAME_BYTES;
}
static uint32_t mat_get_channels(const struct audio_stream *s)
{
    return ((const struct mat_stream_out *)s)->channel_mask;
}
static uint32_t mat_get_format(const struct audio_stream *s)
{
    return ((const struct mat_stream_out *)s)->format;
}
static int mat_set_format(struct audio_stream *s, uint32_t f) { (void)s; (void)f; return 0; }
static int mat_standby(struct audio_stream *s)
{
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);
    mat_close_fd_locked(m);
    pthread_mutex_unlock(&m->lock);
    return 0;
}
static int mat_dump(const struct audio_stream *s, int fd)
{
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);
    uint64_t pres = mat_calc_presented_locked(m, now_monotonic_ns());
    dprintf(fd, "  TCL eARC MAT Bypass Stream (%s):\n",
            m->is_raw_truehd ? "DOLBY_TRUEHD_RAW -> MAT 2.0" : "IEC61937_MAT");
    dprintf(fd, "    rate=%u ch=0x%x fd=%d started=%d sync_offset_ms=%d auto_av_ms=%d (stock now %u) effective_ms=%d clamp_hits=%u\n",
            m->sample_rate, m->channel_mask, m->fd, m->started, mat_get_pipe_delay_ms(),
            m->auto_av_latched ? m->auto_av_ms : 0, m->last_k_av_delay, mat_effective_delay_ms(m),
            m->clamp_hits);
    dprintf(fd, "    written_frames=%llu presented_frames=%llu mat_bursts=%u skips=%u\n",
            (unsigned long long)m->written_frames, (unsigned long long)pres,
            m->mat_bursts_emitted, m->resync_skips);
    dprintf(fd, "    kernel: pumped=%llu q_depth=%u underruns=%u already_ready=%u relocks=%u hdr_chg=%u\n",
            (unsigned long long)m->hw_consumed_bursts, m->last_k_qdepth,
            m->last_k_underruns, m->last_k_late, m->last_k_relocks, m->last_k_hdr_changes);
    pthread_mutex_unlock(&m->lock);
    return 0;
}
static uint32_t mat_get_device(const struct audio_stream *s)
{
    return ((const struct mat_stream_out *)s)->devices;
}
static int mat_set_device(struct audio_stream *s, uint32_t d)
{
    ((struct mat_stream_out *)s)->devices = d;
    return 0;
}
static int mat_set_parameters(struct audio_stream *s, const char *kv)
{
    (void)s; (void)kv;
    return 0;
}
static char *mat_get_parameters(const struct audio_stream *s, const char *keys)
{
    const struct mat_stream_out *m = (const struct mat_stream_out *)s;
    if (keys && strstr(keys, "sup_formats"))
        return strdup(m->is_raw_truehd ? "sup_formats=AUDIO_FORMAT_DOLBY_TRUEHD"
                                       : "sup_formats=AUDIO_FORMAT_IEC61937");
    if (keys && strstr(keys, "sup_sampling_rates"))
        return strdup(m->is_raw_truehd ? "sup_sampling_rates=48000|96000|192000"
                                       : "sup_sampling_rates=192000");
    if (keys && strstr(keys, "sup_channels"))
        return strdup("sup_channels=AUDIO_CHANNEL_OUT_7POINT1");
    return strdup("");
}
static int mat_add_effect(const struct audio_stream *s, void *e) { (void)s; (void)e; return 0; }
static int mat_rem_effect(const struct audio_stream *s, void *e) { (void)s; (void)e; return 0; }

/* ---- audio_stream_out callbacks ---- */
static uint32_t mat_get_latency(const struct audio_stream_out *s)
{
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);
    int total;
    if (m->started && m->sample_rate > 0) {
        /* Same clock as get_presentation_position: time from write to reported presentation */
        uint64_t pres = mat_calc_presented_locked(m, now_monotonic_ns());
        uint64_t ahead = m->written_frames > pres ? m->written_frames - pres : 0;
        total = (int)((ahead * 1000ULL) / m->sample_rate);
    } else {
        mat_poll_kernel_status_locked(m);
        unsigned depth = m->fd < 0 ? 0 : KERNEL_Q_BURSTS;
        if (m->fd >= 0 && m->can_read_status && m->last_k_qdepth <= KERNEL_Q_BURSTS)
            depth = m->last_k_qdepth;
        total = (int)(depth * 20U) + mat_effective_delay_ms(m);
    }
    pthread_mutex_unlock(&m->lock);
    return total > 0 ? (uint32_t)total : 0;
}
static int mat_set_volume(struct audio_stream_out *s, float l, float r)
{
    (void)s; (void)l; (void)r;
    return 0;
}
static ssize_t mat_write(struct audio_stream_out *s, const void *buffer, size_t bytes)
{
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);

    if (m->paused) {
        pthread_mutex_unlock(&m->lock);
        return -EAGAIN;
    }
    ssize_t ret;
    if (m->is_raw_truehd) {
        ret = mat_write_truehd_raw_locked(m, (const uint8_t *)buffer, bytes);
    } else {
        int rc = mat_write_all_locked(m, (const uint8_t *)buffer, bytes);
        if (rc != 0) {
            ret = rc;
        } else {
            m->written_frames += bytes / MAT_BYTES_PER_FRAME;
            m->mat_bursts_emitted += (uint32_t)(bytes / MAT_FRAME_BYTES);
            ret = (ssize_t)bytes;
        }
    }

    pthread_mutex_unlock(&m->lock);
    return ret;
}
static int mat_get_render_position(const struct audio_stream_out *s, uint32_t *dsp_frames)
{
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);
    if (dsp_frames)
        *dsp_frames = (uint32_t)mat_calc_presented_locked(m, now_monotonic_ns());
    pthread_mutex_unlock(&m->lock);
    return 0;
}
static int mat_get_next_write_timestamp(const struct audio_stream_out *s, int64_t *ts)
{
    (void)s; (void)ts;
    return -ENOSYS;
}
static int mat_pause(struct audio_stream_out *s)
{
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);
    int rc = 0;
    /* Let accepted writes finish while the pump can still make space. */
    while (m->in_write > 0)
        pthread_cond_wait(&m->io_idle, &m->lock);
    if (!m->paused && m->fd >= 0 && ioctl(m->fd, EARC_MAT_PAUSE, 0) < 0)
        rc = -errno;
    if (!rc && !m->paused) {
        mat_calc_presented_locked(m, now_monotonic_ns());
        m->paused = 1;
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}
static int mat_resume(struct audio_stream_out *s)
{
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);
    int rc = 0;
    if (m->paused && m->fd >= 0 && ioctl(m->fd, EARC_MAT_RESUME, 0) < 0)
        rc = -errno;
    if (!rc && m->paused) {
        m->resume_ns = now_monotonic_ns();
        m->auto_av_latched = 0; /* pick up an A/V delay that changed while paused */
        mat_poll_kernel_status_locked(m);
    }
    if (!rc)
        m->paused = 0;
    pthread_mutex_unlock(&m->lock);
    return rc;
}
static int mat_drain(struct audio_stream_out *s, int type)
{
    (void)type;
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);

    if (m->paused) {
        pthread_mutex_unlock(&m->lock);
        return -EAGAIN;
    }
    mat_flush_partial_tail_locked(m);

    int fd = m->fd;
    if (fd >= 0) {
        m->in_write++;
        pthread_mutex_unlock(&m->lock);
        fsync(fd);
        pthread_mutex_lock(&m->lock);
        m->in_write--;
        pthread_cond_broadcast(&m->io_idle);
    }

    pthread_mutex_unlock(&m->lock);
    return 0;
}
static int mat_flush(struct audio_stream_out *s)
{
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);
    mat_close_fd_locked(m);
    mat_reset_packer_locked(m);
    m->written_frames = 0;
    m->base_frames = 0;
    m->last_presented = 0;
    pthread_mutex_unlock(&m->lock);
    return 0;
}
static int mat_get_presentation_position(const struct audio_stream_out *s,
                                         uint64_t *frames, struct timespec *timestamp)
{
    struct mat_stream_out *m = (struct mat_stream_out *)s;
    pthread_mutex_lock(&m->lock);
    clock_gettime(CLOCK_MONOTONIC, timestamp);
    uint64_t now_ns = (uint64_t)timestamp->tv_sec * 1000000000ULL + (uint64_t)timestamp->tv_nsec;
    *frames = mat_calc_presented_locked(m, now_ns);
    pthread_mutex_unlock(&m->lock);
    return 0;
}

/* ---- audio_hw_device hooks ---- */
static char *shim_get_parameters(const struct audio_hw_device *dev, const char *keys)
{
    char *res = g_orig_get_parameters ? g_orig_get_parameters(dev, keys) : strdup("");
    if (res && keys && strstr(keys, "sup_formats")) {
        if ((strstr(res, "AUDIO_FORMAT_DOLBY_TRUEHD") || strstr(res, "AUDIO_FORMAT_E_AC3_JOC")) &&
            !strstr(res, "AUDIO_FORMAT_IEC61937")) {
            size_t len = strlen(res) + 32;
            char *ext = (char *)malloc(len);
            if (ext) {
                snprintf(ext, len, "%s|AUDIO_FORMAT_IEC61937", res);
                ALOGI("sup_formats augmented with AUDIO_FORMAT_IEC61937");
                free(res);
                return ext;
            }
        }
    }
    return res;
}

/* Stateless <5 us hardware/ring probe: never conflicts with active stream (-EBUSY treated as active).
   When targeting HDMI_EARC, retry briefly on -ENODEV to ride out transient DSP/eARC init windows. */
static int is_earc_mat_ring_available(uint32_t devices)
{
    int max_tries = (devices == AUDIO_DEVICE_OUT_HDMI_EARC) ? 5 : 1;
    for (int attempt = 0; attempt < max_tries; attempt++) {
        int pfd = open(BYPASS_DEV, O_RDONLY);
        if (pfd >= 0) {
            close(pfd);
            return 1;
        }
        if (errno == EACCES || errno == EPERM) {
            pfd = open(BYPASS_DEV, O_WRONLY);
            if (pfd >= 0) {
                close(pfd);
                return 1;
            }
        }
        if (errno == EBUSY)
            return 1; /* Another MAT stream is currently open -> eARC MAT ring IS active */
        if (errno != ENODEV || attempt + 1 >= max_tries)
            break;
        usleep(100000); /* 100 ms */
    }
    return 0;
}

static int shim_open_output_stream(struct audio_hw_device *dev, int handle,
                                   uint32_t devices, int flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out,
                                   const char *address)
{
    int is_iec_mat = (config && config->format == AUDIO_FORMAT_IEC61937 &&
                      config->sample_rate == MAT_SAMPLE_RATE);
    int is_truehd  = (config && config->format == AUDIO_FORMAT_DOLBY_TRUEHD);

    if (is_truehd && config->sample_rate != 0U && (config->sample_rate % 48000U) != 0U) {
        ALOGW("TrueHD non-48kHz-family rate=%u -> falling back to stock HAL", config->sample_rate);
        return g_orig_open_output_stream(dev, handle, devices, flags, config, stream_out, address);
    }

    if (is_iec_mat || is_truehd) {
        /* For raw TrueHD, run stateless O_RDONLY probe (<5 us): if eARC MAT ring is inactive
           (e.g. TV on Pass through / TV speakers / soundbar off), fall back cleanly to stock HAL.
           For IEC61937, never fall back since stock vendor HAL does not support IEC61937. */
        if (is_truehd && !is_earc_mat_ring_available(devices)) {
            ALOGW("eARC MAT ring inactive (%s) for DOLBY_TRUEHD_RAW -> falling back to stock HAL",
                  strerror(errno));
            return g_orig_open_output_stream(dev, handle, devices, flags, config, stream_out, address);
        }

        ALOGI("INTERCEPT open_output_stream: %s rate=%u ch=0x%x dev=0x%x flags=0x%x -> %s",
              is_truehd ? "DOLBY_TRUEHD_RAW" : "IEC61937_MAT",
              config->sample_rate, config->channel_mask, devices, flags, BYPASS_DEV);

        struct mat_stream_out *m = (struct mat_stream_out *)calloc(1, sizeof(*m));
        if (!m)
            return -ENOMEM;

        m->magic = MAT_STREAM_MAGIC;
        pthread_mutex_init(&m->lock, NULL);
        pthread_cond_init(&m->io_idle, NULL);
        m->fd = -1;
        m->can_read_status = 1;
        m->is_raw_truehd = is_truehd;
        m->sample_rate   = config->sample_rate ? config->sample_rate : (is_truehd ? 48000U : MAT_SAMPLE_RATE);
        m->channel_mask  = config->channel_mask ? config->channel_mask : AUDIO_CHANNEL_OUT_7POINT1;
        m->format        = config->format;
        m->devices       = devices ? devices : AUDIO_DEVICE_OUT_HDMI_EARC;

        m->stream.common.get_sample_rate = mat_get_sample_rate;
        m->stream.common.set_sample_rate = mat_set_sample_rate;
        m->stream.common.get_buffer_size = mat_get_buffer_size;
        m->stream.common.get_channels    = mat_get_channels;
        m->stream.common.get_format      = mat_get_format;
        m->stream.common.set_format      = mat_set_format;
        m->stream.common.standby         = mat_standby;
        m->stream.common.dump            = mat_dump;
        m->stream.common.get_device      = mat_get_device;
        m->stream.common.set_device      = mat_set_device;
        m->stream.common.set_parameters  = mat_set_parameters;
        m->stream.common.get_parameters  = mat_get_parameters;
        m->stream.common.add_audio_effect    = mat_add_effect;
        m->stream.common.remove_audio_effect = mat_rem_effect;

        m->stream.get_latency              = mat_get_latency;
        m->stream.set_volume               = mat_set_volume;
        m->stream.write                    = mat_write;
        m->stream.get_render_position      = mat_get_render_position;
        m->stream.get_next_write_timestamp = mat_get_next_write_timestamp;
        m->stream.pause                    = mat_pause;
        m->stream.resume                   = mat_resume;
        m->stream.drain                    = mat_drain;
        m->stream.flush                    = mat_flush;
        m->stream.get_presentation_position = mat_get_presentation_position;

        config->sample_rate  = m->sample_rate;
        config->channel_mask = m->channel_mask;
        config->format       = m->format;
        config->frame_count  = is_truehd ? 4096U : (MAT_FRAME_BYTES / MAT_BYTES_PER_FRAME);

        register_shim_stream(m);

        *stream_out = &m->stream;
        return 0;
    }

    return g_orig_open_output_stream(dev, handle, devices, flags, config, stream_out, address);
}

static void shim_close_output_stream(struct audio_hw_device *dev, struct audio_stream_out *out)
{
    struct mat_stream_out *m = unregister_shim_stream(out);
    if (m) {
        pthread_mutex_lock(&m->lock);
        mat_close_fd_locked(m);
        while (m->in_write > 0)
            pthread_cond_wait(&m->io_idle, &m->lock);
        pthread_mutex_unlock(&m->lock);
        m->magic = 0;
        pthread_cond_destroy(&m->io_idle);
        pthread_mutex_destroy(&m->lock);
        free(m);
        ALOGI("closed intercepted MAT/TrueHD stream");
        return;
    }

    g_orig_close_output_stream(dev, out);
}

static int shim_open(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
    (void)module;
    if (!g_orig_handle) {
        g_orig_handle = dlopen(ORIG_HAL_PATH, RTLD_NOW);
        if (!g_orig_handle) {
            ALOGE("dlopen(%s) failed: %s", ORIG_HAL_PATH, dlerror());
            return -ENOENT;
        }
        g_orig_hmi = (struct hw_module_t *)dlsym(g_orig_handle, "HMI");
        if (!g_orig_hmi || !g_orig_hmi->methods || !g_orig_hmi->methods->open) {
            ALOGE("dlsym(HMI) failed in %s", ORIG_HAL_PATH);
            return -EINVAL;
        }
    }

    int rc = g_orig_hmi->methods->open(g_orig_hmi, id, device);
    if (rc != 0 || !*device) {
        ALOGE("orig open(%s) failed: %d", id ? id : "null", rc);
        return rc;
    }

    struct audio_hw_device *adev = (struct audio_hw_device *)(*device);
    g_orig_get_parameters      = adev->get_parameters;
    g_orig_open_output_stream  = adev->open_output_stream;
    g_orig_close_output_stream = adev->close_output_stream;

    adev->get_parameters      = shim_get_parameters;
    adev->open_output_stream  = shim_open_output_stream;
    adev->close_output_stream = shim_close_output_stream;

    ALOGI("READY: wrapped %s (IEC61937 + dynamic MAT 2.0 DOLBY_TRUEHD_RAW -> %s)",
          ORIG_HAL_PATH, BYPASS_DEV);
    return 0;
}

static struct hw_module_methods_t g_shim_methods = {
    .open = shim_open,
};

__attribute__((visibility("default")))
struct hw_module_t HMI = {
    .tag                = HARDWARE_MODULE_TAG,
    .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
    .hal_api_version    = HARDWARE_HAL_API_VERSION,
    .id                 = "audio",
    .name               = "TCL C9K eARC MAT Audio HAL Shim",
    .author             = "TCL eARC Research",
    .methods            = &g_shim_methods,
};
