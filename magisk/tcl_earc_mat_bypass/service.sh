#!/system/bin/sh
MODDIR="${0%/*}"
LOG="$MODDIR/boot.log"

{
    echo "=== tcl_earc_mat_bypass service.sh $(date) ==="
    # Wait for Android boot completion (max 60s)
    i=0
    while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$i" -lt 60 ]; do
        sleep 1
        i=$((i + 1))
    done

    KVER="$(uname -r)"
    echo "kernel=$KVER"
    if [ "$KVER" != "5.15.192-android14-11" ]; then
        echo "ABORT: kernel version mismatch (expected 5.15.192-android14-11)"
        exit 0
    fi

    # Wait briefly for eARC platform node
    j=0
    while [ ! -d /sys/devices/platform/mtk_earc ] && [ "$j" -lt 20 ]; do
        sleep 1
        j=$((j + 1))
    done

    if ! lsmod | grep -q '^tcl_earc_mat '; then
        insmod "$MODDIR/tcl_earc_mat.ko" 2>&1
        echo "insmod_rc=$?"
    else
        echo "tcl_earc_mat already loaded"
    fi

    # Wait for ueventd to create /dev/earc_mat_bypass (up to 5s)
    k=0
    while [ ! -c /dev/earc_mat_bypass ] && [ "$k" -lt 25 ]; do
        sleep 0.2
        k=$((k + 1))
    done

    if [ -c /dev/earc_mat_bypass ]; then
        chmod 0666 /dev/earc_mat_bypass
        chcon u:object_r:audio_device:s0 /dev/earc_mat_bypass 2>/dev/null || true
        ls -laZ /dev/earc_mat_bypass
    else
        echo "ERROR: /dev/earc_mat_bypass missing"
    fi

    # Start eARC auto-recovery watchdog (recovers from stuck ARC_MODE after standby)
    if [ -x "$MODDIR/earc_watchdog.sh" ]; then
        setsid "$MODDIR/earc_watchdog.sh" </dev/null >/dev/null 2>&1 &
        echo "earc_watchdog started"
    fi
} > "$LOG" 2>&1
