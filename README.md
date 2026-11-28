# TCL 65C9K eARC / Atmos research

Lossless Dolby TrueHD with Dolby Atmos from **internal** Android TV apps on a **TCL 65C9K** (Google TV 14, MediaTek
MT5896, chassis T653T01, firmware V8-T653T01-LF1V670 / GB AU04) to a TCL Q85H Pro soundbar over eARC — plus a
reversible debloat + custom-launcher setup on the rooted TV.

## Status: SOLVED (2026-11-25)

**Internal Android TV → eARC TX lossless Dolby TrueHD Atmos passthrough is working cleanly end-to-end** on stock C9K
V670 with **no HDMI input, no DSP firmware patch, and no boot/DTBO changes**, deployed reversibly via a single Magisk
module (`/data/adb/modules/tcl_earc_mat_bypass/`):

- **How it works:** Even while idle on Android right from a cold boot (with *Digital Audio Out = Auto*, zero
  HDMI ever touched), `DEC-R2` continuously writes silent 61,440-byte IEC61937/MAT 2.0 bursts into the SE DSP Non-PCM
  DDR ring (`0x2249C000..0x22640000`, `28 × 61,440` bytes = `560 ms`) every `20.000 ms`, and `SE DSP` continuously
  streams that ring to `eARC TX` (`0x951115 = 0x25`). Our bypass skips `DEC-R2` completely and feeds untouched TrueHD
  Atmos MAT 2.0 bursts directly into that ring.
- **Kernel driver ([`native/tcl_earc_mat.c`](native/tcl_earc_mat.c) → `/dev/earc_mat_bypass`):** 32-frame (`640 ms`)
  lock-free SPSC jitter buffer with binary completion wakeup, `fsync` end-of-stream drain, non-exclusive `O_RDONLY`
  probe/telemetry (`frames_out` + `ktime_get()` monotonic timestamps), Vivaldi RIU + multi-slot MAT ring guards, and a
  dedicated `SCHED_FIFO` `kthread` (`earc_mat_pump`) phase-locked to `DEC-R2`'s live 20.000 ms slot cadence.
- **Audio HAL shim ([`native/audio_primary_merak_shim.c`](native/audio_primary_merak_shim.c) →
  `/vendor/lib/hw/audio.primary.merak.so`):** Wraps the stock 32-bit vendor HAL in-place, intercepts **both**
  `AUDIO_FORMAT_IEC61937` (192 kHz 7.1 MAT — Kodi `AudioTrack (IEC)`, AudioProbe) **and** `AUDIO_FORMAT_DOLBY_TRUEHD`
  (raw 48/96/192 kHz TrueHD — Moonfin, Jellyfin, Plex, Stremio, ExoPlayer, dynamically packed via IEC 61937-9 / MAT 2.0
  `input_timing` & `output_timing` into bit-exact 61,440-byte MAT 2.0 bursts, including UHD Blu-ray peak AUs > 2,560 B)
  into `/dev/earc_mat_bypass` with hardware-clocked `CLOCK_MONOTONIC` presentation timestamps and multi-stream tracking.
  All other streams (PCM, AC3, E-AC-3, DTS, UI, mic) pass straight through to the stock HAL untouched.
- **Audio Policy + `libaudioclient.so` ([`tools/patch_tcl_audio_stack.py`](tools/patch_tcl_audio_stack.py)):** Exposes
  `direct_iec61937 output` on `HDMI EARC` (preserving all dynamic EDID profiles via `AUDIO_FORMAT_DEFAULT`) and removes
  TCL's 5 hardcoded `/system/bin/mediaserver` package-name blocks in `/system/lib/libaudioclient.so`.
- **Magisk auto-load module ([`magisk/tcl_earc_mat_bypass/`](magisk/tcl_earc_mat_bypass/)):** Loads `tcl_earc_mat.ko`
  on boot (with `uname -r` guard and `audio_device` SELinux label) and overlays the HAL shim, policy XML, and patched
  `libaudioclient.so`.

### Verified live results (cold boot, zero HDMI)

| Source | Result |
|---|---|
| Internal app, **TrueHD Atmos (Moonfin / Jellyfin / ExoPlayer)** | **Lossless Dolby Atmos** (`AUDIO_FORMAT_DOLBY_TRUEHD` → live MAT 2.0), `1,829` frames across play/seek/pause/resume with **`underruns=0, already_ready=0, relocks=0`** |
| Internal app, **IEC61937 MAT 192 kHz 7.1 (AudioProbe / Kodi IEC)** | **Lossless Dolby Atmos** (`direct=true`), `505` frames with **`underruns=0, already_ready=0, relocks=0`** |
| Standalone 30 s `truehd.iec` test (`/dev/earc_mat_bypass`) | **Lossless Dolby Atmos**, `1,499` frames in `29.979 s`, `already_ready=0, timeouts=0` |
| Internal app, **E-AC3 / DD+ JOC** | **Atmos** — stock passthrough untouched |
| **External HDMI source**, TrueHD | **Atmos** — stock HW bypass untouched |

👉 **Full architecture, build steps, and how to find the model/firmware-specific DDR addresses & CRCs on other TCL /
MediaTek TVs:** **[`docs/earc-mat-bypass-porting-guide.md`](docs/earc-mat-bypass-porting-guide.md)**.

### Earlier findings (why stock firmware failed)
- The **`MatMlpOnly` hypothesis was disproven** — forcing it `1→0` live via Frida changed nothing (inert HAL field).
- Stock `DEC-R2` (MS12) strips Atmos metadata on internal `MM_IN` TrueHD, with no exposed DSP SHM toggle.
- Stock `snd-pcm-platform.ko` DPlayback/DMA7 (`0x95030c`) collides with active Utopia eARC mute mailboxes, and
  `earc_tx` has no direct host-DMA silicon input bypassing SE DSP ([nonpcm-dplayback-driver-audit](docs/nonpcm-dplayback-driver-audit-2026-11-24.md)).

### Future work / TODO
- **DTS-HD Master Audio / DTS:X lossless passthrough over eARC:** Investigate whether the same `192 kHz 8-ch` SE DSP Non-PCM HBR ring (`0x2249C000`) can also carry IEC61937 DTS-HD HBR (`Pc = 0x0011`) bursts from internal apps instead of falling back to lossy DTS 5.1 core.

## Rooting (UART → U-Boot, this exact set)

Root was obtained over the **HDMI service connector's UART** into the live U-Boot 2019.04 console (no exploit needed).
Details and the full command/output log: [c9k-hdmi-service-uart](docs/c9k-hdmi-service-uart.md),
[c9k-v670-uart-root-attempt](docs/c9k-v670-uart-root-attempt-2026-11-23.md).

**Hardware — successful 3.3 V USB–UART adapter test on this C9K; TV-side logic voltage/tolerance not conclusively measured:**
- HDMI service connector **pin 14 = TV TX** (console out) → adapter RX
- **pin 2 = TV RX** (commands in) ← adapter TX. The owner reports successful two-way U-Boot communication using an adapter configured for **3.3 V**; the exact TV-side TX voltage and whether the final wiring included level translation were not independently documented.
- **pin 11 = signal ground / reference**
- **Observed:** PuTTY on COM5 at 115200 8N1 accepted commands and received replies with the owner's **3.3 V USB–UART adapter**. The earlier blanket **“1.8 V only / 3.3 V does not work”** wording was not supported by that working test.
- **Electrical safety:** functional communication does **not** establish that TV pin 2 is rated for direct 3.3 V drive. Measure the voltage at the TV-side RX/TX pins and verify electrical tolerance or use a suitable level shifter before reproducing; never assume a 3.3 V adapter is universally safe. **Never connect HDMI pin 18 (+5 V)** to UART gear. Make/break connections only with the TV powered off.
- Serial: **115200 8N1** (e.g. PuTTY). You should reach the `=>` U-Boot prompt and see the boot log.

**USB stick:** FAT32, containing the Magisk-patched `boot_b` image (patch the stock `boot_b` with Magisk, verify
kernel byte-identical + ramdisk changed before flashing). Keep the untouched stock `boot_b` for recovery.

**At the U-Boot `=>` prompt** (active slot was `_b`; adjust for `_a`):
```text
avb init mmc
env set devicestate unlock
avb set-devicestate 0            # 0 = unlock, 1 = lock
avb set-verity disable
saveenv
usb_partial_upgrade_to_emmc c9k_v670_boot_b_magisk.img boot_b
reset
```
Then complete Android setup, install Magisk Manager, and grant Superuser for ADB/shell (`su -c id` → `uid=0`).

> ⚠️ With the patched boot installed, **do not relock** the bootloader. A bad `mboot`/bootloader write is a hard brick
> (BROM/SP Flash Tool recovery only). The orange "device is unlocked" boot message lives in `mboot` and was left alone
> on purpose — it is cosmetic and not worth the brick risk.

## Device changes applied (all reversible)

See **[device-changes-20261123](docs/device-changes-20261123.md)** for the exact packages, commands and restore steps:
- **TrueHD Atmos eARC bypass:** Magisk module `/data/adb/modules/tcl_earc_mat_bypass/` (`tcl_earc_mat.ko` + HAL shim +
  policy XML + patched `libaudioclient.so`).
- **Debloat:** 14 packages disabled via `pm disable-user --user 0` (TCL browser/analytics/Guard/Alexa/ChannelPlus/
  Dashboard/Exhibition/Remote-Diagnosis/Feedback, Netflix, Disney+, Prime Video, HBO Max).
- **Launcher:** [cleanyourtv](https://github.com/gathin23/cleanyourtv) `com.gathin.tvhome`, built with
  `stock_home_redirect=launcherx` and set up in safe redirect mode (stock `launcherx` kept as fallback). A Magisk
  `service.d` boot script re-asserts the Home-key accessibility redirect on every boot (Android clears it otherwise).
- **Updates off:** `settings put global ota_disable_automatic_update 1`.
- Partition backups (18 critical partitions + SHA-256 manifest) are on USB; host copy of the manifest in
  git-ignored `private/backups_20261123/`.

## Docs

| Topic | Doc |
|---|---|
| **Working eARC MAT 2.0 bypass & porting guide** | **[earc-mat-bypass-porting-guide](docs/earc-mat-bypass-porting-guide.md)**, [nonpcm-dplayback-driver-audit](docs/nonpcm-dplayback-driver-audit-2026-11-24.md) |
| Live rooted Frida / ftrace investigation | [frida-live-trace-session](docs/frida-live-trace-session.md) |
| Audio evidence trail (static → live) | [investigation](docs/investigation.md), [hdmi-comparison](docs/hdmi-comparison.md), [joc-positive-control](docs/joc-positive-control.md), [minirama-retest](docs/minirama-retest.md), [mat-tunnel-results](docs/mat-tunnel-results.md) |
| MAT/DSP decode analysis | [mat-mlp-only-analysis](docs/mat-mlp-only-analysis.md), [mat-mlp-consumer-trace](docs/mat-mlp-consumer-trace.md), [ms12-mat-transcode-trace](docs/ms12-mat-transcode-trace.md), [dsp-dolby-param-map](docs/dsp-dolby-param-map.md), [mat-bypass-source-predicates](docs/mat-bypass-source-predicates.md) |
| Decoder / kernel / DSP internals | [decoder-dispatch-and-parser](docs/decoder-dispatch-and-parser.md), [decoder-config-contract](docs/decoder-config-contract.md), [mm-dsp-input-boundary](docs/mm-dsp-input-boundary.md), [kernel-audio-driver-audit](docs/kernel-audio-driver-audit.md) |
| IEC61937 / eARC design | [custom-firmware-iec61937](docs/custom-firmware-iec61937.md), [iec-client-experiment](docs/iec-client-experiment.md), [earc-endpoint-and-policy](docs/earc-endpoint-and-policy.md), [iec-mat-carrier-reference](docs/iec-mat-carrier-reference.md) |
| Rooting (UART / U-Boot) | [c9k-hdmi-service-uart](docs/c9k-hdmi-service-uart.md), [c9k-v670-uart-root-attempt](docs/c9k-v670-uart-root-attempt-2026-11-23.md) |
| Device changes (debloat / launcher / updates) | [device-changes-20261123](docs/device-changes-20261123.md) |
| Debloat / privacy analysis | [privacy-manifest-results](docs/privacy-manifest-results.md), [privacy-inventory-results](docs/privacy-inventory-results.md) |
| Build, deploy, test | [reproduction](docs/reproduction.md) |
| Evidence hashes | [evidence/README](evidence/README.md) |

## Reproduce (diagnostic app)

Needs JDK 17+, Android SDK 34 build-tools, adb, Python 3.9+, FFmpeg. Builds the `ro.saitatter.audioprobe` app
(AudioTrack / tunnel / MAT-IEC probes, no network permission):

```powershell
adb connect <tv-ip>:<port>
powershell -ExecutionPolicy Bypass -File .\build.ps1
python .\tools\prepare_samples.py 'C:\path\truehd-atmos.mkv' --eac3 'C:\path\eac3-joc.mkv'
powershell -ExecutionPolicy Bypass -File .\install_samples.ps1
```

Keep soundbar volume very low when testing encoded formats.

## Layout

```text
native/     Kernel bypass driver (tcl_earc_mat.c), Audio HAL shim (audio_primary_merak_shim.c), and DDR/ring diagnostic LKMs
magisk/     Magisk auto-load module template (tcl_earc_mat_bypass: module.prop, service.sh, sepolicy.rule)
app/        Java diagnostic app (AudioTrack / tunnel / MAT probes, no network permission)
tools/      CRC extractor & XML/libaudioclient patcher (patch_tcl_audio_stack.py), sample prep, host tests, register & topology scripts
scripts/    ADB collection and preflight scripts
docs/       Research notes and porting guide (earc-mat-bypass-porting-guide.md)
evidence/   Derived observations and hashes (no raw dumps)
private/    Git-ignored: device dumps, stock/boot images, pulled binaries, compiled .ko/.so, backups, media
```

No TCL/MediaTek/Dolby binaries, firmware, commercial media or keys are committed. Not affiliated with TCL, Dolby,
MediaTek or Google.
