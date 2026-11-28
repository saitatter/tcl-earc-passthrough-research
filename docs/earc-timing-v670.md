# V670 eARC timing measurements, 2026-11-25

The Q85H Pro returned latency byte 0 from video9 before playback and during confirmed 48 kHz Moonfin TrueHD playback. The V670 getter uses cached receiver state. Its direct HWREG getter is selected in stub mode. Keep the manual offset for this combination.

Three independent opens with the carrier running and no user payload produced the following observations. The driver tagged the completed DEC-R2 slot during ring lock. The stock diagnostic returned the SE DSP pointer in 16-byte units. The first return of the read loop corresponds to the requested address, 0x1113.

| Completed slot | Slot byte offset | DSP pointer (16-byte units) | DSP byte offset minus slot offset |
|---:|---:|---:|---:|
| 1 | 0x16800 | 0x2580 | 0xf000 |
| 11 | 0xac800 | 0xbb80 | 0xf000 |
| 21 | 0x142800 | 0x15180 | 0xf000 |

The observed phase was one MAT slot on all three opens within one carrier session. This measures a write-pointer relationship. Physical eARC output latency, acoustic delay and stability across receiver mode changes or cold boots require separate measurements.

For V670 diagnostics, build the module with EARC_MAT_TRACE_SYNC and run tools/measure_earc_phase.sh as root with an active carrier and the stream idle. Match trace returns by timestamp against the kernel sync probe messages. The release module omits the diagnostic call.

After a TV restart, three more independent opens produced (completed slot, slot byte offset, DSP pointer in 16-byte units) of (24, 0x16f800, 0x17e80), (6, 0x61800, 0x7080), and (15, 0xe8800, 0xf780). Each difference remained 0xf000 bytes. The release module was reloaded after tracing.

Two on-device runs against the installed ARM32 HAL and release module exercised the IEC61937 MAT callbacks with a real burst. At zero offset, get_latency returned 140 ms for seven queued bursts and 160 ms for eight. The eighth burst was queued through the device descriptor while paused to hold depth constant. Flush reset the counters and returned zero latency. After twelve more bursts, standby reported all 46080 accepted carrier frames and the next write reopened with base 46080. These callback checks complement the separate Moonfin raw TrueHD pause and resume test. Fast-forward in Moonfin closed and recreated the output stream.

## Automatic video A/V delay, 2026-11-28

The stock HAL compensates the TV's video pipeline by delaying audio. When a video decoder starts, the kernel log shows `[HAL_SND_SetParam] ch1 delay: 286` and `ch6 delay: 286`. During this session that was 4K Dolby Vision at 23.976 fps, output 119.88 Hz, game mode off. `HAL_SND_SetParam` stores ids `0xC9` (ch1) and `0xCB` (ch6) through `HAL_SND_R2_Set_SHM_PARAM` at SND-R2 shared memory `+0x198` and `+0x1A0`. That memory is `g_AudioVars2[+0xA0] + 0x01913000`. A one-shot scan of `MI_MAD_ADV_BUF` (`native/earc_shm_delay_probe.c`) found it at physical `0x21B13000`, with both fields reading 286 and ch8 (`+0x1A8`) reading 0. The values stayed at 286 after video stopped.

The MAT bypass replaces DEC-R2 output and never received this delay. `tcl_earc_mat.ko` now maps that page read-only and returns `max(ch1, ch6)` as `av_delay_ms` (status ABI v2, 48 bytes; 40-byte readers still work). The HAL latches it when a session opens and on resume. It reports presentation `av_delay_ms - persist.tcl.earc_sync_ms` ahead of the pumped DDR slot. `persist.tcl.earc_sync_ms` is now a trim and defaults to 0. Set `persist.tcl.earc_auto_av=0` to disable the automatic part.

The previous 8-slot queue capped any lead at roughly 140 ms, because presentation is clamped to frames written. The queue is now 32 slots (640 ms). The dump and `sync-diag` print `clamp_hits`, which counts positions that were still clamped. On resume the interpolation now starts from the resume time instead of the pre-pause slot stamp, which removes a one-burst forward jump.
