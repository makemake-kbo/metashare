package com.metashare.client;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

/** Compact, pointer-revealed controls owned by the primary Quest screen. */
final class QuestToolbar {

    interface Callbacks {
        void onScreenCountRequested(int count);
        void onKeyboardRequested();
        void onPointerEnabledChanged(boolean enabled);
    }

    private static final int PANEL = 0xF02A2E36;
    private static final int CONTROL = 0xFF3B414C;
    private static final int CONTROL_ACTIVE = 0xFF4678C8;
    private static final float RESTING_ALPHA = 0.34f;

    private final Activity activity;
    private final Callbacks callbacks;
    private final HoverRevealArea revealArea;
    private final LinearLayout controls;
    private final TextView countView;
    private final Button minusButton;
    private final Button plusButton;
    private final Runnable concealRunnable = this::conceal;

    private int screenCount = 1;
    private int maxScreens = 1;

    QuestToolbar(Activity activity, FrameLayout root, Callbacks callbacks) {
        this.activity = activity;
        this.callbacks = callbacks;

        revealArea = new HoverRevealArea(activity);
        FrameLayout.LayoutParams revealParams = new FrameLayout.LayoutParams(
                dp(380), dp(92), Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        root.addView(revealArea, revealParams);

        controls = new LinearLayout(activity);
        controls.setOrientation(LinearLayout.HORIZONTAL);
        controls.setGravity(Gravity.CENTER_VERTICAL);
        controls.setPadding(dp(7), dp(5), dp(7), dp(5));
        controls.setBackground(rounded(PANEL, 16, 0x557C8492, 1));
        controls.setElevation(dp(12));

        Button keyboard = makeButton("⌨", 22);
        keyboard.setContentDescription("Open virtual keyboard");
        keyboard.setBackground(rounded(CONTROL, 11, 0, 0));
        keyboard.setOnClickListener(v -> {
            conceal();
            callbacks.onKeyboardRequested();
        });
        controls.addView(keyboard, fixed(dp(50), dp(46)));

        controls.addView(divider(), dividerParams());

        TextView pointerIcon = label("🖱", 20);
        pointerIcon.setGravity(Gravity.CENTER);
        pointerIcon.setContentDescription("Pointer input");
        controls.addView(pointerIcon, fixed(dp(36), dp(46)));

        Switch pointerSwitch = new Switch(activity);
        pointerSwitch.setChecked(true);
        pointerSwitch.setShowText(false);
        pointerSwitch.setContentDescription("Toggle pointer input");
        pointerSwitch.setOnCheckedChangeListener((button, checked) ->
                callbacks.onPointerEnabledChanged(checked));
        controls.addView(pointerSwitch, fixed(dp(54), dp(46)));

        controls.addView(divider(), dividerParams());

        minusButton = makeButton("−", 24);
        minusButton.setContentDescription("Remove a monitor");
        minusButton.setBackground(rounded(CONTROL, 11, 0, 0));
        minusButton.setOnClickListener(v ->
                callbacks.onScreenCountRequested(screenCount - 1));
        controls.addView(minusButton, fixed(dp(46), dp(46)));

        countView = label("1", 16);
        countView.setGravity(Gravity.CENTER);
        countView.setContentDescription("One monitor open");
        controls.addView(countView, fixed(dp(34), dp(46)));

        plusButton = makeButton("+", 22);
        plusButton.setContentDescription("Add a monitor");
        plusButton.setBackground(rounded(CONTROL_ACTIVE, 11, 0, 0));
        plusButton.setOnClickListener(v ->
                callbacks.onScreenCountRequested(screenCount + 1));
        controls.addView(plusButton, fixed(dp(46), dp(46)));

        FrameLayout.LayoutParams controlsParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, dp(56),
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        revealArea.addView(controls, controlsParams);
        conceal();
    }

    void setScreenCount(int count, int maximum) {
        screenCount = Math.max(1, count);
        maxScreens = Math.max(1, maximum);
        countView.setText(Integer.toString(screenCount));
        countView.setContentDescription(screenCount + (screenCount == 1
                ? " monitor open" : " monitors open"));
        minusButton.setEnabled(screenCount > 1);
        minusButton.setAlpha(screenCount > 1 ? 1f : 0.28f);
        plusButton.setEnabled(screenCount < maxScreens);
        plusButton.setAlpha(screenCount < maxScreens ? 1f : 0.28f);
    }

    static void styleStatus(TextView view) {
        view.setTextColor(Color.WHITE);
        view.setTextSize(12);
        view.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        view.setBackground(rounded(0xB82A2E36, 10, 0, 0));
        view.setPadding(dp(view, 11), dp(view, 7), dp(view, 11), dp(view, 7));
        view.setElevation(dp(view, 4));
    }

    private void reveal() {
        controls.animate().cancel();
        controls.animate()
                .translationY(dp(8))
                .alpha(1f)
                .setDuration(140)
                .start();
    }

    private void conceal() {
        controls.animate().cancel();
        controls.animate()
                .translationY(-dp(40))
                .alpha(RESTING_ALPHA)
                .setDuration(180)
                .start();
    }

    private View divider() {
        View divider = new View(activity);
        divider.setBackgroundColor(0x557C8492);
        return divider;
    }

    private LinearLayout.LayoutParams dividerParams() {
        LinearLayout.LayoutParams params = fixed(dp(1), dp(30));
        params.leftMargin = dp(7);
        params.rightMargin = dp(7);
        return params;
    }

    private TextView label(String text, int sp) {
        TextView view = new TextView(activity);
        view.setText(text);
        view.setTextColor(Color.WHITE);
        view.setTextSize(sp);
        return view;
    }

    private Button makeButton(String text, int sp) {
        Button button = new Button(activity);
        button.setText(text);
        button.setTextColor(Color.WHITE);
        button.setTextSize(sp);
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER);
        button.setPadding(0, 0, 0, 0);
        button.setMinHeight(0);
        button.setMinWidth(0);
        return button;
    }

    private static GradientDrawable rounded(int color, int radiusDp,
                                            int strokeColor, int strokeDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(radiusDp * 3f);
        if (strokeDp > 0) drawable.setStroke(strokeDp * 3, strokeColor);
        return drawable;
    }

    private static LinearLayout.LayoutParams fixed(int width, int height) {
        return new LinearLayout.LayoutParams(width, height);
    }

    private int dp(int value) {
        return (int) (value * activity.getResources().getDisplayMetrics().density + 0.5f);
    }

    private static int dp(View view, int value) {
        return (int) (value * view.getResources().getDisplayMetrics().density + 0.5f);
    }

    /** A generous invisible proximity target around the narrow resting tab. */
    private final class HoverRevealArea extends FrameLayout {
        HoverRevealArea(Activity context) {
            super(context);
            setClipChildren(false);
        }

        @Override
        public boolean dispatchHoverEvent(MotionEvent event) {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_HOVER_ENTER:
                case MotionEvent.ACTION_HOVER_MOVE:
                    removeCallbacks(concealRunnable);
                    reveal();
                    break;
                case MotionEvent.ACTION_HOVER_EXIT:
                    postDelayed(concealRunnable, 300);
                    break;
                default:
                    break;
            }
            return super.dispatchHoverEvent(event);
        }

        @Override
        public boolean dispatchTouchEvent(MotionEvent event) {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) reveal();
            return super.dispatchTouchEvent(event);
        }
    }
}
