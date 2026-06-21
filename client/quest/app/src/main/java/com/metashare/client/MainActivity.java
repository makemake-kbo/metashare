package com.metashare.client;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.FrameLayout;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.util.HashSet;
import java.util.Set;

public final class MainActivity extends Activity
        implements SurfaceHolder.Callback, StreamSession.Listener {

    private static final String TAG = "MetaShare2D";
    static final int MAX_MONITORS = 3;

    private SurfaceView surfaceView;
    private FrameLayout root;
    private TextView statusView;
    private Spinner monitorSpinner;
    private StreamSession session;
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
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(14);
        statusView.setGravity(Gravity.START);
        statusView.setBackgroundColor(0xAA000000);
        statusView.setPadding(18, 10, 18, 10);

        monitorSpinner = createMonitorSpinner();

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
        FrameLayout.LayoutParams spinnerParams = new FrameLayout.LayoutParams(
                dp(160), dp(44),
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        spinnerParams.setMargins(0, dp(12), 0, 0);
        root.addView(monitorSpinner, spinnerParams);
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

    private Spinner createMonitorSpinner() {
        String[] options = new String[MAX_MONITORS];
        for (int i = 0; i < MAX_MONITORS; i++) {
            options[i] = (i + 1) + (i == 0 ? " Monitor" : " Monitors");
        }

        ArrayAdapter<String> adapter = new ArrayAdapter<String>(
                this, android.R.layout.simple_spinner_item, options) {
            @Override
            public View getView(int position, View convertView, android.view.ViewGroup parent) {
                View v = super.getView(position, convertView, parent);
                if (v instanceof TextView) {
                    TextView tv = (TextView) v;
                    tv.setTextColor(Color.WHITE);
                    tv.setTextSize(14);
                }
                return v;
            }

            @Override
            public View getDropDownView(int position, View convertView, android.view.ViewGroup parent) {
                View v = super.getDropDownView(position, convertView, parent);
                if (v instanceof TextView) {
                    TextView tv = (TextView) v;
                    tv.setTextColor(Color.WHITE);
                    tv.setBackgroundColor(0xFF222222);
                    tv.setPadding(dp(16), dp(10), dp(16), dp(10));
                }
                return v;
            }
        };
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);

        Spinner spinner = new Spinner(this);
        spinner.setAdapter(adapter);
        spinner.setBackgroundColor(0xCC000000);
        spinner.setPopupBackgroundDrawable(new ColorDrawable(0xFF222222));
        spinner.setOnItemSelectedListener(new Spinner.OnItemSelectedListener() {
            private boolean first = true;

            @Override
            public void onItemSelected(android.widget.AdapterView<?> parent, View view,
                                        int position, long id) {
                if (first) { first = false; return; }
                setMonitorCount(position + 1);
            }

            @Override
            public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        return spinner;
    }

    private void setMonitorCount(int requestedCount) {
        final int count = Math.max(1, Math.min(MAX_MONITORS, requestedCount));
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

        Toast.makeText(this, count + " monitor" + (count > 1 ? "s" : ""), Toast.LENGTH_SHORT).show();
    }

    // ------------------------------------------------------- SurfaceHolder.Callback

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (session == null) {
            session = new StreamSession(this, holder.getSurface(), 0, this);
            session.start();
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Don't stop — Quest may recreate the surface; StreamSession retries.
    }

    @Override
    protected void onDestroy() {
        if (session != null) { session.stop(); session = null; }
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

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }
}
