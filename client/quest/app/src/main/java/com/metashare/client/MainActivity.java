package com.metashare.client;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.HashSet;
import java.util.Set;

public final class MainActivity extends Activity
        implements SurfaceHolder.Callback, StreamSession.Listener {

    private static final String TAG = "MetaShare2D";
    static final int MAX_MONITORS = 3;

    private SurfaceView surfaceView;
    private FrameLayout root;
    private TextView statusView;
    private int monitorCount = 1;
    private int availableMonitorCount = MAX_MONITORS;
    private StreamSession session;
    private QuestToolbar toolbar;
    private RemoteInputController inputController;
    private volatile int streamWidth;
    private volatile int streamHeight;

    private final Set<Integer> launchedSecondaries = new HashSet<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);

        statusView = new TextView(this);
        QuestToolbar.styleStatus(statusView);

        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        root.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        FrameLayout.LayoutParams statusParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        statusParams.setMargins(18, 18, 18, 18);
        root.addView(statusView, statusParams);

        inputController = new RemoteInputController(this, root, surfaceView,
                new RemoteInputController.Listener() {
                    @Override
                    public void onPointerInput(RemoteInputController.PointerEvent event) {
                        handlePointerInput(event);
                    }

                    @Override
                    public void onTextInput(String text) {
                        // Deliberately do not log content; this callback is the
                        // future input transport boundary.
                        Log.d(TAG, "captured " + text.length() + " keyboard character(s)");
                    }

                    @Override
                    public void onKeyInput(int keyCode, boolean pressed) {
                        Log.v(TAG, "key " + keyCode + (pressed ? " down" : " up"));
                    }
                });

        toolbar = new QuestToolbar(this, root,
                new QuestToolbar.Callbacks() {
                    @Override
                    public void onScreenCountRequested(int count) {
                        setMonitorCount(count);
                    }

                    @Override
                    public void onKeyboardRequested() {
                        inputController.showKeyboard();
                    }

                    @Override
                    public void onPointerEnabledChanged(boolean enabled) {
                        inputController.setPointerEnabled(enabled);
                    }

                });
        toolbar.setScreenCount(monitorCount, availableMonitorCount);
        setContentView(root);
        root.addOnLayoutChangeListener(
                (v, l, t, r, b, ol, ot, or, ob) -> applyAspectRatio());

        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        setStatus("Searching for MetaShare streamer...");
    }

    private void setMonitorCount(int requestedCount) {
        final int count = Math.max(1, Math.min(availableMonitorCount, requestedCount));
        monitorCount = count;
        Log.i(TAG, "setting monitor count to " + count);

        for (int i = 1; i < count; i++) {
            if (!launchedSecondaries.contains(i)) {
                launchedSecondaries.add(i);
                Intent intent = new Intent(this, MonitorActivity.class);
                intent.putExtra("monitor_index", i);
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                        | Intent.FLAG_ACTIVITY_MULTIPLE_TASK);
                startActivity(intent);
            }
        }

        for (MonitorActivity m : MonitorActivity.active) {
            if (m.getMonitorIndex() >= count) m.finish();
        }
        launchedSecondaries.removeIf(i -> i >= count);
        toolbar.setScreenCount(count, availableMonitorCount);
    }

    // ------------------------------------------------------- SurfaceHolder.Callback

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // The Surface is now valid — hand it to the session so MediaCodec can
        // render into it. Created here (not onCreate) so StreamSession sees a
        // ready Surface on first negotiate.
        if (session == null) {
            session = new StreamSession(this, surfaceView, 0, this);
            session.start();
        } else {
            // Surface recreated while the session kept running: rebind the live
            // decoder to it, otherwise it stays on the destroyed surface and wedges.
            session.setRenderer(surfaceView);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Don't stop — Quest may recreate the surface; StreamSession retries
        // and re-binds to whatever Surface is current on next negotiate.
    }

    @Override
    protected void onDestroy() {
        if (session != null) { session.stop(); session = null; }
        if (inputController != null) inputController.release();
        launchedSecondaries.clear();
        super.onDestroy();
    }

    // ------------------------------------------------------- StreamSession.Listener

    @Override
    public void onStatus(String text) { runOnUiThread(() -> setStatus(text)); }

    @Override
    public void onStreamSize(int width, int height) {
        streamWidth = width;
        streamHeight = height;
        runOnUiThread(this::applyAspectRatio);
    }

    @Override
    public void onAvailableMonitors(int count) {
        runOnUiThread(() -> {
            availableMonitorCount = Math.max(1, Math.min(MAX_MONITORS, count));
            if (monitorCount > availableMonitorCount) {
                setMonitorCount(availableMonitorCount);
            } else {
                toolbar.setScreenCount(monitorCount, availableMonitorCount);
            }
        });
    }

    private void applyAspectRatio() {
        int sw = streamWidth, sh = streamHeight;
        int cw = root.getWidth(), ch = root.getHeight();
        if (sw <= 0 || sh <= 0 || cw <= 0 || ch <= 0) return;
        float streamRatio = (float) sw / sh;
        float containerRatio = (float) cw / ch;
        int targetW, targetH;
        if (streamRatio > containerRatio) {
            targetW = cw; targetH = Math.round(cw / streamRatio);
        } else {
            targetH = ch; targetW = Math.round(ch * streamRatio);
        }
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        if (lp == null || lp.width != targetW || lp.height != targetH || lp.gravity != Gravity.CENTER) {
            surfaceView.setLayoutParams(new FrameLayout.LayoutParams(targetW, targetH, Gravity.CENTER));
        }
    }

    private void setStatus(String text) {
        if (text == null || text.isEmpty()) statusView.setVisibility(View.GONE);
        else { statusView.setVisibility(View.VISIBLE); statusView.setText(text); }
    }

    private void handlePointerInput(RemoteInputController.PointerEvent event) {
        // Keep motion quiet in logcat; down/up are useful while the input
        // transport is being connected later.
        if (event.action == android.view.MotionEvent.ACTION_DOWN
                || event.action == android.view.MotionEvent.ACTION_UP) {
            Log.d(TAG, "pointer " + event.action + " at " + event.x + "," + event.y);
        }
    }
}
