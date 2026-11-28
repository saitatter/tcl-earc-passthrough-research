package ro.saitatter.audioprobe;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.Choreographer;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.util.Arrays;

/**
 * Rhythm-game style A/V Lip-Sync Calibrator for the TCL eARC MAT 2.0 bypass.
 *
 * Plays a 1.000-second rhythmic pulse (50 x 20ms MAT 2.0 bursts = 192,000 frames @ 192kHz)
 * through the exact ENCODING_IEC61937 -> audio.primary.merak.so shim -> /dev/earc_mat_bypass path,
 * synchronized to AudioTrack.getTimestamp() (which reads persist.tcl.earc_sync_ms).
 *
 * Controls (TV remote D-pad):
 *   - OK / CENTER / ENTER : Tap on the audio beat (averages 8 taps -> computes exact offset)
 *   - LEFT / RIGHT        : Live fine-tune offset (-10 ms / +10 ms)
 *   - UP / DOWN           : Coarse offset (-50 ms / +50 ms)
 *   - MENU / Y / PLAY     : Apply averaged tap offset & save permanently
 *   - BACK                : Exit cleanly
 */
public final class SyncCalibratorActivity extends Activity implements Choreographer.FrameCallback {
    private static final String TAG = "SyncCalibrator";
    private static final int MAT_BYTES = 61440;
    private static final int SAMPLE_RATE = 192000;
    private static final int FRAMES_PER_MAT = MAT_BYTES / 16; // 3840 frames = 20.000 ms
    private static final int BURSTS_PER_BEAT = 50;            // 50 * 20ms = 1.000 s
    private static final int ACTIVE_BURSTS = 6;               // 120ms real TrueHD burst per beat
    private static final int MAX_TAPS = 8;

    // Valid IEC61937-9 / Dolby MAT 2.0 silence frame constants (16-bit LE swapped)
    private static final byte[] IEC_HDR = {
        0x72, (byte)0xf8, 0x1f, 0x4e, 0x16, 0x00, (byte)0xf0, (byte)0xef
    };
    private static final byte[] MAT_START = {
        (byte)0x9e, 0x07, 0x03, 0x00, 0x01, (byte)0x84, 0x01, 0x01,
        0x00, (byte)0x80, (byte)0xa5, 0x56, (byte)0xf4, 0x3b, (byte)0x83, (byte)0x81,
        (byte)0x80, 0x49, (byte)0xe0, 0x77
    };
    private static final byte[] MAT_MID = {
        (byte)0xc1, (byte)0xc3, 0x49, 0x42, (byte)0xfa, 0x3b, (byte)0x83, (byte)0x82,
        (byte)0x80, 0x49, (byte)0xe0, 0x77
    };
    private static final byte[] MAT_END = {
        (byte)0xc2, (byte)0xc3, (byte)0xc4, (byte)0xc0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0x11, (byte)0x97
    };

    private volatile boolean running;
    private volatile AudioTrack track;
    private Thread audioThread;

    private SyncCanvasView canvasView;
    private TextView headerView, statusView, tapLogView;
    private Button tapBtn, applyBtn;

    private int currentOffsetMs = 0;
    private final int[] tapErrors = new int[MAX_TAPS];
    private int tapCount = 0;
    private int lastTapErrorMs = 0;
    private long lastTapFlashMs = 0;
    private float currentPhase = 0f; // 0.0 .. 1.0 (0.5 = center target hit!)

    private final AudioTimestamp ts = new AudioTimestamp();
    private long anchorFrame = 0;
    private long anchorNano = 0;
    private boolean hasTs = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        currentOffsetMs = readCurrentPropMs();

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(16, 18, 24));
        root.setPadding(48, 32, 48, 32);

        headerView = new TextView(this);
        headerView.setTextColor(Color.WHITE);
        headerView.setTextSize(26);
        headerView.setText("eARC TrueHD Atmos A/V Sync Calibrator");
        root.addView(headerView);

        statusView = new TextView(this);
        statusView.setTextColor(Color.rgb(130, 220, 255));
        statusView.setTextSize(18);
        statusView.setPadding(0, 8, 0, 12);
        root.addView(statusView);

        canvasView = new SyncCanvasView(this);
        root.addView(canvasView, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        tapLogView = new TextView(this);
        tapLogView.setTextColor(Color.rgb(210, 215, 225));
        tapLogView.setTextSize(16);
        tapLogView.setPadding(0, 12, 0, 10);
        root.addView(tapLogView);

        LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        addBtn(btnRow, "-50 ms", v -> applyOffsetMs(currentOffsetMs - 50));
        addBtn(btnRow, "-10 ms", v -> applyOffsetMs(currentOffsetMs - 10));
        tapBtn = addBtn(btnRow, "TAP ON BEAT (OK)", v -> recordTap());
        addBtn(btnRow, "+10 ms", v -> applyOffsetMs(currentOffsetMs + 10));
        addBtn(btnRow, "+50 ms", v -> applyOffsetMs(currentOffsetMs + 50));
        applyBtn = addBtn(btnRow, "Apply Suggested", v -> applyTapAverage());
        addBtn(btnRow, "Clear", v -> {
            tapCount = 0;
            updateLabels("Taps cleared — tap OK on the beep (" + MAX_TAPS + " taps)");
        });
        root.addView(btnRow, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        setContentView(root);
        tapBtn.requestFocus();
        updateLabels("Starting TrueHD MAT 2.0 1 Hz beep...");
        startAudio();
        Choreographer.getInstance().postFrameCallback(this);
    }

    private Button addBtn(LinearLayout row, String label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setFocusable(true);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(4, 0, 4, 0);
        row.addView(b, lp);
        return b;
    }

    @Override
    protected void onDestroy() {
        running = false;
        Choreographer.getInstance().removeFrameCallback(this);
        if (audioThread != null) {
            audioThread.interrupt();
            try { audioThread.join(1000); } catch (InterruptedException ignored) {}
        }
        AudioTrack t = track;
        track = null;
        if (t != null) {
            try { t.pause(); t.flush(); t.release(); } catch (Exception ignored) {}
        }
        super.onDestroy();
    }

    private int readCurrentPropMs() {
        String v = runCmd("getprop persist.tcl.earc_sync_ms");
        if (v == null || v.isEmpty()) v = runCmd("getprop debug.earc_sync_ms");
        try {
            if (v != null && !v.isEmpty()) return Integer.parseInt(v.trim());
        } catch (NumberFormatException ignored) {}
        return 0;
    }

    private void applyOffsetMs(int newMs) {
        if (newMs < -500) newMs = -500;
        if (newMs > 500) newMs = 500;
        currentOffsetMs = newMs;
        final int val = newMs;
        new Thread(() -> {
            runRootCmd("setprop persist.tcl.earc_sync_ms " + val + " && setprop debug.earc_sync_ms " + val);
            // Brief pause/play restarts presentation clock cleanly with the new offset
            AudioTrack t = track;
            if (t != null) {
                try { t.pause(); Thread.sleep(40); t.play(); } catch (Exception ignored) {}
            }
        }).start();
        updateLabels("Saved persist.tcl.earc_sync_ms = " + newMs + " ms (applied live)");
    }

    private String runCmd(String cmd) {
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"sh", "-c", cmd});
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line = r.readLine();
            p.waitFor();
            return line;
        } catch (Exception e) {
            return null;
        }
    }

    private void runRootCmd(String cmd) {
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
            p.waitFor();
        } catch (Exception e) {
            Log.e(TAG, "su failed: " + e.getMessage());
        }
    }

    private byte[] buildSilenceMatFrame() {
        byte[] f = new byte[MAT_BYTES];
        System.arraycopy(IEC_HDR, 0, f, 0, IEC_HDR.length);
        System.arraycopy(MAT_START, 0, f, 8, MAT_START.length);
        System.arraycopy(MAT_MID, 0, f, 30716, MAT_MID.length);
        System.arraycopy(MAT_END, 0, f, 61416, MAT_END.length);
        return f;
    }

    private void startAudio() {
        running = true;
        audioThread = new Thread(() -> {
            File dir = getExternalFilesDir(null);
            if (dir == null) dir = getFilesDir();
            File beatFile = new File(dir, "sync_beat.iec");
            if (!beatFile.exists()) {
                runOnUiThread(() -> updateLabels("ERROR: missing " + beatFile.getAbsolutePath()));
                return;
            }

            byte[] loopBuf;
            int totalBursts;
            try (FileInputStream fis = new FileInputStream(beatFile)) {
                int len = (int) beatFile.length();
                totalBursts = len / MAT_BYTES;
                if (totalBursts <= 0) {
                    runOnUiThread(() -> updateLabels("ERROR: sync_beat.iec is empty"));
                    return;
                }
                loopBuf = new byte[totalBursts * MAT_BYTES];
                int off = 0;
                while (off < loopBuf.length) {
                    int n = fis.read(loopBuf, off, loopBuf.length - off);
                    if (n <= 0) break;
                    off += n;
                }
            } catch (Exception e) {
                runOnUiThread(() -> updateLabels("ERROR reading sync_beat.iec: " + e.getMessage()));
                return;
            }

            try {
                AudioFormat fmt = new AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_IEC61937)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_7POINT1_SURROUND)
                    .build();
                AudioAttributes attrs = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                    .build();
                AudioTrack t = new AudioTrack.Builder()
                    .setAudioFormat(fmt)
                    .setAudioAttributes(attrs)
                    .setBufferSizeInBytes(MAT_BYTES * 4)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build();
                track = t;
                t.play();
                runOnUiThread(() -> updateLabels("Playing continuous TrueHD 1 Hz beep — hit OK on beep, or LEFT/RIGHT ±10 ms"));

                int burstIdx = 0;
                while (running) {
                    int written = t.write(loopBuf, burstIdx * MAT_BYTES, MAT_BYTES, AudioTrack.WRITE_BLOCKING);
                    if (written < 0) {
                        Log.w(TAG, "AudioTrack write error: " + written);
                        Thread.sleep(50);
                    }
                    burstIdx = (burstIdx + 1) % totalBursts;
                }
            } catch (Exception e) {
                Log.e(TAG, "Audio error", e);
                runOnUiThread(() -> updateLabels("Audio error: " + e.getMessage()));
            }
        }, "SyncCalibratorAudio");
        audioThread.start();
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        if (!running) return;
        AudioTrack t = track;
        if (t != null && t.getPlayState() == AudioTrack.PLAYSTATE_PLAYING) {
            if (t.getTimestamp(ts) && ts.framePosition > 0) {
                anchorFrame = ts.framePosition;
                anchorNano = ts.nanoTime;
                hasTs = true;
            }
            if (hasTs) {
                long dtNs = frameTimeNanos - anchorNano;
                long estFrames = anchorFrame + (dtNs * SAMPLE_RATE) / 1_000_000_000L;
                long beatFrames = (long) BURSTS_PER_BEAT * FRAMES_PER_MAT; // 192,000 frames = 1.000s
                long mod = ((estFrames % beatFrames) + beatFrames) % beatFrames;
                currentPhase = (float) mod / (float) beatFrames;
            }
        }
        canvasView.invalidate();
        Choreographer.getInstance().postFrameCallback(this);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_MENU:
            case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:
                applyTapAverage();
                return true;
            case KeyEvent.KEYCODE_SPACE:
                recordTap();
                return true;
            default:
                return super.onKeyDown(keyCode, event);
        }
    }

    private void recordTap() {
        // Center target is at phase = 0.5 -> error in ms in [-500, +500]
        // Positive error means user heard the beat AFTER the marker crossed center -> audio is late -> increase offset
        int errMs = Math.round((currentPhase - 0.5f) * 1000f);
        lastTapErrorMs = errMs;
        lastTapFlashMs = SystemClock.uptimeMillis();
        if (tapCount < MAX_TAPS) {
            tapErrors[tapCount++] = errMs;
        } else {
            System.arraycopy(tapErrors, 1, tapErrors, 0, MAX_TAPS - 1);
            tapErrors[MAX_TAPS - 1] = errMs;
        }
        if (tapCount == MAX_TAPS) {
            int med = medianTapError();
            updateLabels("8/8 taps! Median error: " + fmtMs(med) + " → Suggested offset: "
                + (currentOffsetMs + med) + " ms (press PLAY/MENU to apply, or LEFT/RIGHT)");
        } else {
            updateLabels("Tap " + tapCount + "/" + MAX_TAPS + ": " + fmtMs(errMs)
                + "  (keep tapping OK on the audio beat)");
        }
    }

    private int medianTapError() {
        if (tapCount == 0) return 0;
        int[] copy = Arrays.copyOf(tapErrors, tapCount);
        Arrays.sort(copy);
        return copy[tapCount / 2];
    }

    private void applyTapAverage() {
        if (tapCount == 0) return;
        int med = medianTapError();
        int target = currentOffsetMs + med;
        tapCount = 0;
        applyOffsetMs(target);
    }

    private static String fmtMs(int ms) {
        return (ms >= 0 ? "+" : "") + ms + " ms";
    }

    private void updateLabels(String msg) {
        statusView.setText("Current persist.tcl.earc_sync_ms = " + currentOffsetMs + " ms   |   " + msg);
        StringBuilder sb = new StringBuilder();
        sb.append("Use D-pad + OK on the buttons below (starts focused on TAP ON BEAT). BACK to exit.\n");
        sb.append("Taps (").append(tapCount).append("/").append(MAX_TAPS).append("): ");
        for (int i = 0; i < tapCount; i++) {
            if (i > 0) sb.append(", ");
            sb.append(fmtMs(tapErrors[i]));
        }
        if (tapCount > 0) {
            int med = medianTapError();
            int sug = currentOffsetMs + med;
            sb.append("   |   Median: ").append(fmtMs(med)).append("   |   Suggested: ").append(sug).append(" ms");
            if (applyBtn != null) applyBtn.setText("Apply " + sug + " ms");
        } else {
            if (applyBtn != null) applyBtn.setText("Apply Suggested");
        }
        tapLogView.setText(sb.toString());
    }

    private final class SyncCanvasView extends View {
        private final Paint trackPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint zonePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint centerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint markerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        SyncCanvasView(Context ctx) {
            super(ctx);
            trackPaint.setColor(Color.rgb(42, 48, 62));
            zonePaint.setColor(Color.argb(90, 40, 210, 120));
            centerPaint.setColor(Color.WHITE);
            centerPaint.setStrokeWidth(6f);
            markerPaint.setColor(Color.rgb(60, 190, 255));
            textPaint.setColor(Color.WHITE);
            textPaint.setTextSize(34f);
            textPaint.setTextAlign(Paint.Align.CENTER);
        }

        @Override
        protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            float cy = h * 0.5f;
            float left = w * 0.08f, right = w * 0.92f;
            float span = right - left;
            float cx = left + span * 0.5f;

            // Flash background briefly when phase crosses 0.5 (visual beat)
            float distFromCenter = Math.abs(currentPhase - 0.5f);
            if (distFromCenter < 0.045f) {
                int alpha = (int) ((1f - distFromCenter / 0.045f) * 140f);
                c.drawColor(Color.argb(alpha, 50, 240, 140));
            }

            // Track bar
            c.drawRoundRect(left, cy - 36f, right, cy + 36f, 18f, 18f, trackPaint);

            // ±25ms sweet-spot zone around center
            float zoneHalf = span * 0.025f;
            c.drawRect(cx - zoneHalf, cy - 90f, cx + zoneHalf, cy + 90f, zonePaint);

            // Center target line
            c.drawLine(cx, cy - 120f, cx, cy + 120f, centerPaint);

            // Moving rhythm marker (crosses center at phase = 0.5)
            float mx = left + currentPhase * span;
            markerPaint.setColor(distFromCenter < 0.03f ? Color.rgb(80, 255, 140) : Color.rgb(60, 190, 255));
            c.drawRoundRect(mx - 14f, cy - 80f, mx + 14f, cy + 80f, 8f, 8f, markerPaint);

            // Show last tap feedback
            if (SystemClock.uptimeMillis() - lastTapFlashMs < 900) {
                textPaint.setColor(Math.abs(lastTapErrorMs) <= 25 ? Color.rgb(80, 255, 140) : Color.rgb(255, 210, 70));
                c.drawText("TAP: " + fmtMs(lastTapErrorMs), cx, cy - 150f, textPaint);
            } else {
                textPaint.setColor(Color.rgb(180, 190, 210));
                c.drawText("HIT [OK] WHEN YOU HEAR THE BEAT", cx, cy - 150f, textPaint);
            }
        }
    }
}
