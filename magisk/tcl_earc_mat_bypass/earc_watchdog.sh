#!/system/bin/sh
# Auto-recovery watchdog for TCL C9K + eARC soundbar (e.g. TCL Q85H Pro).
# If eARC is enabled in TV settings (SupportMode = EARC) but the link drops
# into legacy ARC (ConnectState = ARC_MODE, eARC_TX Status: IDLE2) — which
# TCL's SOUNDBAR_NOT_NEED_TO_RESET_EARC quirk never auto-resets — trigger a
# clean HdmiControlManager eARC off->on re-handshake.

MODDIR="${0%/*}"
INFO="/sys/devices/platform/mtk_earc/RD/Info"
LOG="$MODDIR/earc_watchdog.log"
PIDFILE="$MODDIR/earc_watchdog.pid"

# Single-instance guard (verify cmdline to avoid stale PID reuse after reboot)
if [ -f "$PIDFILE" ]; then
    OLD_PID="$(cat "$PIDFILE" 2>/dev/null)"
    if [ -n "$OLD_PID" ] && [ -r "/proc/$OLD_PID/cmdline" ] && grep -q earc_watchdog "/proc/$OLD_PID/cmdline" 2>/dev/null; then
        exit 0
    fi
fi
echo "$$" > "$PIDFILE"
trap 'rm -f "$PIDFILE"' EXIT

log() {
    # Keep log bounded (~200 lines max)
    if [ -f "$LOG" ] && [ "$(wc -l < "$LOG" 2>/dev/null || echo 0)" -ge 200 ]; then
        tail -n 100 "$LOG" > "${LOG}.tmp" 2>/dev/null && mv "${LOG}.tmp" "$LOG"
    fi
    echo "$(date '+%Y-%m-%d %H:%M:%S') [earc_watchdog] $*" >> "$LOG"
}

is_stuck_arc() {
    [ -r "$INFO" ] || return 1
    DATA="$(cat "$INFO" 2>/dev/null)"
    case "$DATA" in
        *"SupportMode = EARC"*"ConnectState = ARC_MODE"*)
            case "$DATA" in
                *"ArcPin:1"*) return 0 ;;
            esac
            ;;
    esac
    return 1
}

is_earc_ok() {
    [ -r "$INFO" ] || return 1
    case "$(cat "$INFO" 2>/dev/null)" in
        *"ConnectState = EARC_MODE"*"eARC_TX Status: eARC"*) return 0 ;;
        *"eARC_TX Status: eARC"*"ConnectState = EARC_MODE"*) return 0 ;;
    esac
    return 1
}

is_tv_awake() {
    dumpsys power 2>/dev/null | grep -q 'mWakefulness=Awake'
}

log "started (pid=$$)"

RETRIES=0
MAX_RETRIES=3
WAS_AWAKE=1

while true; do
    sleep 5

    if is_earc_ok; then
        if [ "$RETRIES" -ne 0 ]; then
            log "eARC healthy again (EARC_MODE); resetting retry counter"
            RETRIES=0
        fi
        continue
    fi

    if ! is_stuck_arc; then
        # Either eARC is intentionally disabled (SupportMode = ARC), disconnected, or transitioning.
        if [ "$RETRIES" -ne 0 ]; then
            RETRIES=0
        fi
        continue
    fi

    # Only act when TV is awake; if it went to sleep, reset retries for next wakeup.
    if ! is_tv_awake; then
        WAS_AWAKE=0
        RETRIES=0
        continue
    fi

    if [ "$WAS_AWAKE" -eq 0 ]; then
        WAS_AWAKE=1
        RETRIES=0
    fi

    if [ "$RETRIES" -ge "$MAX_RETRIES" ]; then
        continue
    fi

    # Grace period: let normal post-wakeup discovery finish before intervening.
    sleep 4
    if ! is_stuck_arc; then
        continue
    fi

    # Verify global eARC setting is actually enabled (not 0/off).
    EARC_SET="$(cmd hdmi_control cec_setting get earc_enabled 2>/dev/null)"
    case "$EARC_SET" in
        *"= 1"*) ;;
        *)
            continue
            ;;
    esac

    RETRIES=$((RETRIES + 1))
    log "detected stuck ARC_MODE while SupportMode=EARC (attempt $RETRIES/$MAX_RETRIES); toggling earc_enabled 0->1"

    cmd hdmi_control cec_setting set earc_enabled 0 >/dev/null 2>&1
    sleep 2
    cmd hdmi_control cec_setting set earc_enabled 1 >/dev/null 2>&1
    sleep 5

    if is_earc_ok; then
        log "recovered to EARC_MODE on attempt $RETRIES"
        RETRIES=0
    else
        STATE="$(tr '\r\n' '  ' < "$INFO" 2>/dev/null)"
        log "still not in EARC_MODE after attempt $RETRIES ($STATE)"
        sleep $((RETRIES * 10))
    fi
done
