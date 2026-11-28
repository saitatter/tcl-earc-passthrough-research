# TCL MT5896 / Pentonic eARC TrueHD Dolby Atmos MAT Bypass — Architecture & Porting Guide

This guide documents the working internal Android TV → eARC TX lossless **Dolby TrueHD Atmos (MAT 2.0 / IEC61937)** bypass proven on **TCL 65C9K (G08_4K_GB, MT5896, Android 14, firmware V670 AU04)** and explains how to discover the model/firmware-specific addresses and symbol CRCs to port it to other MediaTek Pentonic / MT5896 / MT96xx TCL TVs.

---

## 1. Architecture Overview

On stock TCL V670 firmware, internal Android (`MM_IN`) TrueHD playback is routed through the Dolby MS12 decoder (`DEC-R2`), which strips object metadata (`isAtmos=0`) and outputs Dolby Surround instead of Atmos. Meanwhile, external HDMI TrueHD Atmos works because `SE DSP` streams 61,440-byte (`0xF000`) IEC61937/MAT 2.0 bursts directly from a DDR ring buffer into `eARC TX` (`0x951115 = 0x25`, `SE DSP NPCM2`).

Key discovery: **even while idle on Android from a cold boot (zero HDMI input)**, with eARC enabled (`0x951115 = 0x25`, `0x95038F = 0x02`, `0x95030C = 0x00`), `DEC-R2` continuously writes silent 61,440-byte MAT 2.0 bursts into the SE DSP Non-PCM DDR ring at an exact **20.000 ms** cadence, and `SE DSP` continuously streams that ring over eARC TX to the soundbar.

```text
Android App (Kodi AudioTrack IEC / AudioProbe / ExoPlayer)
  │  ENCODING_IEC61937 (0x0D000000), 192 kHz, 7.1 (8-ch), 61,440-byte MAT 2.0 bursts
  ▼
/system/lib/libaudioclient.so  (5 TCL "/system/bin/mediaserver" IEC61937 blocks removed)
  ▼
AudioFlinger / AudioPolicyManager  (primary_audio_policy_configuration.xml: direct_iec61937 -> HDMI EARC)
  ▼
/vendor/lib/hw/audio.primary.merak.so  (shim wrapping audio.primary.merak.orig.so)
  │  Intercepts IEC61937 192 kHz streams only; provides CLOCK_MONOTONIC A/V sync timestamps
  ▼
/dev/earc_mat_bypass  (tcl_earc_mat.ko: 32-frame / 640 ms lock-free SPSC ring + kthread)
  │  0.25 ms hrtimer polling on +0xEFFC sentinel; overwrites each silent MAT slot <0.3 ms after DEC-R2 writes it
  ▼
DDR MAT 2.0 Ring: 0x2249C000..0x22640000  (28 × 61,440 bytes = 0x1A4000 = 560 ms)
  ▼
SE DSP (NPCM2) ──▶ eARC TX (0x951115 = 0x25) ──▶ Soundbar displays "Dolby Atmos" (lossless)
```

No boot/DTBO changes and no DSP firmware patches are required. Everything is deployed reversibly via a single Magisk module (`/data/adb/modules/tcl_earc_mat_bypass/`).

---

## 2. Repository Files

### Production driver & Magisk module
- [`native/tcl_earc_mat.c`](../native/tcl_earc_mat.c) — Production AArch64 kernel module exposing `/dev/earc_mat_bypass` (32-frame / 640 ms SPSC jitter buffer with binary completion wakeup, stateless `O_RDONLY` probe & hardware-clocked telemetry via `_copy_to_iter`, `fsync` drain, Vivaldi RIU + multi-slot MAT ring validation, PAN-safe `_copy_from_iter`, and a `SCHED_FIFO` `earc_mat_pump` kthread phase-locked via `ktime_get()` + `TASK_IDLE`).
- [`native/audio_primary_merak_shim.c`](../native/audio_primary_merak_shim.c) — 32-bit ARM Android Audio HAL wrapper (`audio.primary.merak.so`) that wraps `audio.primary.merak.orig.so` in-place, intercepts both `AUDIO_FORMAT_IEC61937` (192 kHz 7.1 MAT) and `AUDIO_FORMAT_DOLBY_TRUEHD` (raw 48/96/192 kHz TrueHD, dynamically packed via IEC 61937-9 / MAT 2.0 `input_timing` & `output_timing` into bit-exact 61,440-byte MAT 2.0 bursts, supporting UHD peak AUs > 2,560 B), routes them to `/dev/earc_mat_bypass`, and provides hardware-clocked `CLOCK_MONOTONIC` presentation timestamps.
- [`magisk/tcl_earc_mat_bypass/module.prop`](../magisk/tcl_earc_mat_bypass/module.prop), [`service.sh`](../magisk/tcl_earc_mat_bypass/service.sh), [`sepolicy.rule`](../magisk/tcl_earc_mat_bypass/sepolicy.rule) — Magisk auto-load module with kernel version safety guard (`uname -r`), `ueventd` wait loop, and `audio_device` SELinux labeling.
- [`tools/patch_tcl_audio_stack.py`](../tools/patch_tcl_audio_stack.py) — Extracts kernel module CRCs from pulled `/vendor_dlkm/lib/modules/*.ko`, patches `primary_audio_policy_configuration.xml`, and patches the 5 TCL IEC61937 package-name blocks in `libaudioclient.so`.

### Research & diagnostic kernel modules / scripts
- [`native/mad_full_scan.c`](../native/mad_full_scan.c) — Scans the entire 52 MiB `MI_MAD_ADV_BUF` DDR region for IEC61937/MAT sync words (`0x4e1ff872`).
- [`native/mad_mat_scan.c`](../native/mad_mat_scan.c) — Inspects ring boundaries and wraparound around `0x2249C000..0x22640000`.
- [`native/earc_mat_mute_test.c`](../native/earc_mat_mute_test.c) — 2.5 s zero-mute causality test proving `0x2249C000` drives live eARC TX output.
- [`native/earc_ring_probe.c`](../native/earc_ring_probe.c) — 700 ms sentinel overwrite probe proving `DEC-R2` continuously writes MAT frames in idle.
- [`native/earc_slot_timing.c`](../native/earc_slot_timing.c) — Measures exact 20.000 ms slot write order and cadence across all 28 slots.
- [`native/earc_subms_diag.c`](../native/earc_subms_diag.c) — Sub-millisecond (250 µs) write-burst profile proving `DEC-R2` writes each 61,440-byte slot atomically in `< 0.25 ms`.
- [`native/earc_doorbell_diff.c`](../native/earc_doorbell_diff.c), [`native/earc_sram_pace_test.c`](../native/earc_sram_pace_test.c), [`native/earc_mat_poc.c`](../native/earc_mat_poc.c) — Intermediate experiments (Vivaldi RIU bank diff, `SRAM 0x1113` pacing test, snapshot/inject PoC).
- [`tools/read_earc_dma_topology.sh`](../tools/read_earc_dma_topology.sh), [`tools/read_earc_registers.sh`](../tools/read_earc_registers.sh), [`tools/trace_ser2_npcm.sh`](../tools/trace_ser2_npcm.sh) — Live read-only register and kprobe inspection helpers.

---

## 3. Porting to Another TCL / MediaTek Model or Firmware

Because DDR memory maps, kernel `MODVERSIONS` CRCs, and `libaudioclient.so` offsets are **model- and firmware-specific**, follow these 6 steps on any new rooted MediaTek TCL TV.

### Step 1: Verify kernel config and extract `vermagic` + symbol CRCs

Run on the TV as root:
```sh
uname -r
zcat /proc/config.gz | grep -E "CONFIG_MODULES|CONFIG_MODVERSIONS|CONFIG_CFI_CLANG|CONFIG_ARM64_MODULE_PLTS|CONFIG_ARM64_SW_TTBR0_PAN"
```

Pull `/vendor_dlkm/lib/modules/mtk-tvsnd-soc.ko`, `rfkill.ko`, and `bluetooth.ko` (or any `.ko` importing the needed symbols) to `private/` and extract the exact `vermagic` and `__versions` CRCs:
```powershell
python tools/patch_tcl_audio_stack.py extract-crcs private/mtk-tvsnd-soc.ko private/rfkill.ko private/bluetooth.ko
```

Update the `____versions[]` table and `vermagic=` string at the bottom of `native/tcl_earc_mat.c` with your firmware's CRCs and `uname -r` output.

> **Why `_copy_from_iter`?** Android 14 GKI kernels enable `CONFIG_ARM64_SW_TTBR0_PAN=y`. Calling raw `__arch_copy_from_user` from an out-of-tree module without `uaccess_ttbr0_enable()` causes an infinite minor page-fault loop on 32-bit compat callers. Using `f_op->write_iter` + core-kernel `_copy_from_iter` avoids this completely.

### Step 2: Find the live `MI_MAD_ADV_BUF`, `SE_MAD_BASE`, and eARC MAT DDR ring address (`RING_PHYS`)

On C9K V670:
- `MI_MAD_ADV_BUF` = physical `0x20200000`, size `0x3400000` (52 MiB)
- `SE_MAD_BASE` = physical `0x21B10000`
- `RING_PHYS` = `SE_MAD_BASE + 0x0098C000` = **`0x2249C000`**, `RING_SIZE` = **`0x1A4000`** (`28 × 61,440` bytes)

To find these on a different model/firmware:
1. **Check the eARC TX mux and Non-PCM flags** via `rw_reg`:
   ```sh
   N=/sys/devices/platform/mtk-tv-offload-platform/mtk_dbg/rw_reg
   for a in 95030c 95038e 95038f 951115; do printf '%s\n' "$a" > "$N"; cat "$N"; done
   printf '0\n' > "$N"
   ```
   Confirm `0x951115` has bits `[6:4] = 2` (`0x2x` = `SE DSP NPCM2`), `0x95038F` has bit 1 set (`0x02` = `eARCConfig` Non-PCM), and `0x95030C = 0x00` (unmuted).
2. **Find `MI_MAD_ADV_BUF` and `SE_MAD_BASE`:**
   - Check `/proc/iomem` or `/sys/firmware/devicetree/base/reserved-memory/` for the audio MAD buffer (`mi_mad_adv_buf` / `audio_mad`), or disassemble `MDrv_AUDIO_Dump_HdmiNpcm_Monitor` in `mtk-tvsnd-soc.ko` to read the offset added to `SE_MAD_BASE` (`0x0098C000` on V670).
   - You can also kprobe `HAL_AUDIO_DumpDspInfo` (which reads `g_AudioVars2` containing `SE_MAD_BASE` and `MI_MAD_ADV_BUF`).
3. **Scan for MAT 2.0 sync words (`0x4e1ff872 0xeff00016`):**
   - Update `SCAN_PHYS` / `SCAN_SIZE` in `native/mad_full_scan.c`, compile and `insmod` it (it returns `-EAGAIN` after logging hits so it auto-unloads).
   - Look for a contiguous run of hits spaced by exactly `0xF000` (`61,440` bytes) with `w0=4e1ff872 w1=eff00016`.
   - Find the exact start and end of the ring with `native/mad_mat_scan.c` (the ring starts at a page-aligned base where the last frame wraps cleanly back to the base; size = `N × 0xF000`, e.g. `28 × 0xF000 = 0x1A4000`).
4. **Prove causality & idle carrier:**
   - Run `native/earc_mat_mute_test.c` during live HDMI Dolby Atmos playback: soundbar audio must drop cleanly for 2.5 s and resume.
   - Run `native/earc_ring_probe.c` and `native/earc_slot_timing.c` while idle on Android (no HDMI): confirm `DEC-R2` overwrites one slot every `20.000 ms`.
   - Set `RING_PHYS`, `RING_SIZE`, and `NUM_SLOTS` in `native/tcl_earc_mat.c`.

### Step 3: Build `tcl_earc_mat.ko`

Using Android NDK r27+ `clang`:
```powershell
$clang = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.3.13750724\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
& $clang --target=aarch64-linux-gnu -O2 -ffixed-x18 -fno-stack-protector -fno-PIC -fno-asynchronous-unwind-tables -c native\tcl_earc_mat.c -o private\tcl_earc_mat.ko
```

Verify standalone streaming before touching the Audio HAL:
```sh
insmod /data/local/tmp/tcl_earc_mat.ko
dd if=/sdcard/Android/data/ro.saitatter.audioprobe/files/truehd.iec of=/dev/earc_mat_bypass bs=4096 count=3000
dmesg | grep tcl_earc_mat
# Expected: closed: frames=200 underruns=0 already_ready=0 relocks=0
```

### Step 4: Adapt and build the Audio HAL shim

1. Find the loaded primary Audio HAL library and architecture on your TV:
   ```sh
   PID=$(ps -ef | grep "hardware.audio.service" | grep -v grep | awk '{print $2}' | head -n 1)
   grep "audio.primary" /proc/$PID/maps | awk '{print $6}' | sort -u
   ls -laZ /vendor/lib/hw/audio.primary* /vendor/lib64/hw/audio.primary* 2>/dev/null
   ```
   On C9K V670 this is 32-bit `/vendor/lib/hw/audio.primary.merak.so` (`u:object_r:same_process_hal_file:s0`).
2. Update `ORIG_HAL_PATH` in `native/audio_primary_merak_shim.c` if your SoC uses a different name (e.g., `audio.primary.mt5896.orig.so`).
3. Compile (32-bit ARMv7a for 32-bit audio HAL):
   ```powershell
   $clang32 = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.3.13750724\toolchains\llvm\prebuilt\windows-x86_64\bin\armv7a-linux-androideabi34-clang.cmd"
   & $clang32 -O2 -Wall -Wextra -shared -fPIC native\audio_primary_merak_shim.c -o private\audio.primary.merak.shim.so -llog -ldl "-Wl,-soname,audio.primary.merak.so"
   ```

### Step 5: Patch `primary_audio_policy_configuration.xml` and `libaudioclient.so`

Pull your TV's own stock files to `private/`:
```powershell
adb shell "su -c 'cp /vendor/etc/primary_audio_policy_configuration.xml /system/lib/libaudioclient.so /data/local/tmp/ && chmod 644 /data/local/tmp/primary_audio_policy_configuration.xml /data/local/tmp/libaudioclient.so'"
adb pull /data/local/tmp/primary_audio_policy_configuration.xml private\
adb pull /data/local/tmp/libaudioclient.so private\
```

Run the patcher script:
```powershell
python tools/patch_tcl_audio_stack.py patch-xml private/primary_audio_policy_configuration.xml private/primary_audio_policy_configuration.patched.xml
python tools/patch_tcl_audio_stack.py patch-audioclient private/libaudioclient.so private/libaudioclient.patched.so
```

What these two patches do:
- **`primary_audio_policy_configuration.xml`:**
  - Adds `<mixPort name="direct_iec61937 output" flags="AUDIO_OUTPUT_FLAG_DIRECT" ...>` with `AUDIO_FORMAT_IEC61937`, `192000 Hz`, `AUDIO_CHANNEL_OUT_7POINT1`.
  - Adds `direct_iec61937 output` to the `HDMI EARC` `<route>`.
  - Adds `<profile name="" format="AUDIO_FORMAT_DEFAULT"/>` (preserving all dynamic EDID profiles) + `<profile name="" format="AUDIO_FORMAT_IEC61937" samplingRates="192000" channelMasks="AUDIO_CHANNEL_OUT_7POINT1"/>` inside `<devicePort tagName="HDMI EARC">`.
- **`libaudioclient.so`:**
  - TCL hardcodes 5 checks in `libaudioclient.so` (`isDirectOutputSupported`, `getDirectPlaybackSupport`, `getDirectProfilesForAttributes`, `AudioTrack::set`) that reject `AUDIO_FORMAT_IEC61937` (`0x0D000000`) for any package other than `/system/bin/mediaserver`.
  - The patcher locates the 5 Thumb-2 `cmp.w rX, #0xd000000` guards followed by the `/proc/%d/cmdline` check and turns each guard branch into an unconditional skip (`b.w` / `b` / `nop`).

### Step 6: Install the Magisk module and verify

Assemble `/data/adb/modules/tcl_earc_mat_bypass/` on the TV:
- `module.prop`, `service.sh`, `sepolicy.rule` (from `magisk/tcl_earc_mat_bypass/`)
- `tcl_earc_mat.ko` (from Step 3)
- `system/vendor/lib/hw/audio.primary.merak.orig.so` (copy of stock `/vendor/lib/hw/audio.primary.merak.so`, context `same_process_hal_file`)
- `system/vendor/lib/hw/audio.primary.merak.so` (our shim from Step 4, context `same_process_hal_file`)
- `system/vendor/etc/primary_audio_policy_configuration.xml` (patched XML from Step 5, context `vendor_configs_file`)
- `system/lib/libaudioclient.so` (patched library from Step 5, context `system_lib_file`)

Reboot cleanly and verify:
1. `/data/adb/modules/tcl_earc_mat_bypass/boot.log` shows `insmod_rc=0` and `/dev/earc_mat_bypass` with `u:object_r:audio_device:s0`.
2. `dumpsys media.audio_policy | grep -i IEC61937` shows `direct_iec61937 output` and `AUDIO_FORMAT_IEC61937` under `HDMI EARC`.
3. In **AudioProbe** (`IEC61937 TrueHD HBR 192k 7.1`) or **Kodi** (*Settings → System → Audio → Audio output device: `AudioTrack (IEC)`*, *Allow passthrough*, *Dolby TrueHD capable receiver*), play a TrueHD Atmos track:
   - Soundbar displays **Dolby Atmos** with clean lossless audio.
   - `dmesg | grep tcl_earc_mat` reports `underruns=0 already_ready=0 relocks=0`.
