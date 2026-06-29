package com.metashare.client;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.PopupMenu;
import android.widget.TextView;

import org.webrtc.EglBase;
import org.webrtc.RendererCommon;
import org.webrtc.SurfaceViewRenderer;

import java.util.concurrent.CopyOnWriteArrayList;

public final class MonitorActivity extends Activity
        implements SurfaceHolder.Callback, StreamSession.Listener {

    private static final String TAG = "MonitorActivity";

    static final CopyOnWriteArrayList<MonitorActivity> active = new CopyOnWriteArrayList<>();

    private SurfaceViewRenderer surfaceView;
    private EglBase eglBase;
    private FrameLayout root;
    private TextView statusView;
    private StreamSession session;
    private int streamWidth;
    private int streamHeight;
    private int monitorIndex;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        monitorIndex = getIntent().getIntExtra("monitor_index", 1);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // One shared EGL context per Activity — SurfaceViewRenderer needs it
        // for both its own GL rendering and the underlying MediaCodec decoder.
        eglBase = EglBase.create();

        surfaceView = new SurfaceViewRenderer(this);
        surfaceView.getHolder().addCallback(this);

        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(14);
        statusView.setGravity(Gravity.START);
        statusView.setBackgroundColor(0xAA000000);
        statusView.setPadding(18, 10, 18, 10);

        View menuBar = new View(this);
        menuBar.setBackgroundColor(0xCC444444);
        menuBar.setClickable(true);
        menuBar.setOnClickListener(this::showMenu);

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
        FrameLayout.LayoutParams menuBarParams = new FrameLayout.LayoutParams(
                dp(80), dp(8),
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        menuBarParams.setMargins(0, dp(8), 0, 0);
        root.addView(menuBar, menuBarParams);
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

        active.add(this);
        Log.i(TAG, "Monitor " + (monitorIndex + 1) + " created");
    }

    int getMonitorIndex() { return monitorIndex; }

    private void showMenu(View anchor) {
        PopupMenu menu = new PopupMenu(this, anchor);
        menu.getMenu().add("Monitor " + (monitorIndex + 1)).setEnabled(false);
        menu.getMenu().add("Close monitor");
        menu.setOnMenuItemClickListener(item -> {
            if ("Close monitor".contentEquals(item.getTitle())) {
                finish();
                return true;
            }
            return false;
        });
        menu.show();
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // SurfaceViewRenderer.init() must be called once before frames can
        // be added. Use SCALE_ASPECT_FIT so the stream letterboxes inside
        // the View rather than stretching.
        try {
            surfaceView.init(eglBase.getEglBaseContext(), rendererEvents);
            surfaceView.setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT);
            surfaceView.setMirror(false);
        } catch (Exception e) {
            Log.e(TAG, "SurfaceViewRenderer.init failed", e);
        }
        if (session == null) {
            session = new StreamSession(this, surfaceView, eglBase, monitorIndex, this);
            session.start();
        }
    }

    private final RendererCommon.RendererEvents rendererEvents =
            new RendererCommon.RendererEvents() {
                @Override public void onFirstFrameRendered() {
                    Log.i(TAG, "first frame rendered");
                }
                @Override
                public void onFrameResolutionChanged(int w, int h, int rotation) {
                    streamWidth = w;
                    streamHeight = h;
                    runOnUiThread(MonitorActivity.this::applyAspectRatio);
                }
            };

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Don't stop — StreamSession retries with new surface.
        // SurfaceViewRenderer.release() is deferred to onDestroy().
    }

    @Override
    protected void onDestroy() {
        active.remove(this);
        if (session != null) { session.stop(); session = null; }
        try { surfaceView.release(); } catch (Exception ignored) {}
        if (eglBase != null) {
            eglBase.release();
            eglBase = null;
        }
        Log.i(TAG, "Monitor " + (monitorIndex + 1) + " destroyed");
        super.onDestroy();
    }

    @Override
    public void onStatus(String text) {
        runOnUiThread(() -> {
            if (text == null || text.isEmpty()) statusView.setVisibility(View.GONE);
            else { statusView.setVisibility(View.VISIBLE); statusView.setText(text); }
        });
    }

    @Override
    public void onStreamSize(int width, int height) {
        // RendererCommon.RendererEvents delivers real frame dimensions now;
        // this legacy callback is a no-op kept for source compat.
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
        FrameLayout.LayoutParams lp =
                (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        if (lp == null || lp.width != targetW || lp.height != targetH
                || lp.gravity != Gravity.CENTER) {
            surfaceView.setLayoutParams(
                    new FrameLayout.LayoutParams(targetW, targetH, Gravity.CENTER));
        }
    }
}
