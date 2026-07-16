package com.metashare.client;

import android.app.Activity;
import android.graphics.Color;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

/**
 * Captures Android/Quest input in a transport-neutral form. Networking is
 * intentionally left to a future input channel; activities can forward these
 * callbacks without having to revisit view or IME handling.
 */
final class RemoteInputController {

    static final class PointerEvent {
        final int action;
        final float x;
        final float y;
        final float verticalScroll;
        final int buttons;
        final int source;
        final int toolType;

        PointerEvent(int action, float x, float y, float verticalScroll,
                     int buttons, int source, int toolType) {
            this.action = action;
            this.x = x;
            this.y = y;
            this.verticalScroll = verticalScroll;
            this.buttons = buttons;
            this.source = source;
            this.toolType = toolType;
        }
    }

    interface Listener {
        void onPointerInput(PointerEvent event);
        void onTextInput(String text);
        void onKeyInput(int keyCode, boolean pressed);
    }

    private final Activity activity;
    private final SurfaceView surface;
    private final EditText keyboardTarget;
    private final Listener listener;
    private boolean pointerEnabled = true;

    RemoteInputController(Activity activity, FrameLayout root,
                          SurfaceView surface, Listener listener) {
        this.activity = activity;
        this.surface = surface;
        this.listener = listener;

        surface.setOnTouchListener((view, event) -> capturePointer(event));
        surface.setOnGenericMotionListener((view, event) -> capturePointer(event));

        // A tiny, nearly transparent editor gives the Android IME a legitimate
        // input target while the decoded Surface remains visually dominant.
        keyboardTarget = new EditText(activity);
        keyboardTarget.setSingleLine(false);
        keyboardTarget.setTextColor(Color.TRANSPARENT);
        keyboardTarget.setBackgroundColor(Color.TRANSPARENT);
        keyboardTarget.setCursorVisible(false);
        keyboardTarget.setAlpha(0.02f);
        keyboardTarget.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        keyboardTarget.setContentDescription("Remote keyboard input");
        keyboardTarget.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
                if (count > 0) {
                    listener.onTextInput(s.subSequence(start, start + count).toString());
                }
                for (int i = count; i < before; i++) {
                    listener.onKeyInput(KeyEvent.KEYCODE_DEL, true);
                    listener.onKeyInput(KeyEvent.KEYCODE_DEL, false);
                }
            }

            @Override public void afterTextChanged(Editable editable) {}
        });
        keyboardTarget.setOnKeyListener((view, keyCode, event) -> {
            listener.onKeyInput(keyCode, event.getAction() == KeyEvent.ACTION_DOWN);
            return false;
        });
        FrameLayout.LayoutParams keyboardParams = new FrameLayout.LayoutParams(2, 2);
        keyboardParams.leftMargin = 1;
        keyboardParams.topMargin = 1;
        root.addView(keyboardTarget, keyboardParams);

        // Keep the IME closed on launch. Only the primary toolbar's explicit
        // keyboard action transfers focus to the editor.
        root.setFocusableInTouchMode(true);
        root.requestFocus();
    }

    void setPointerEnabled(boolean enabled) {
        pointerEnabled = enabled;
    }

    void showKeyboard() {
        keyboardTarget.requestFocus();
        keyboardTarget.post(() -> {
            InputMethodManager ime = (InputMethodManager)
                    activity.getSystemService(Activity.INPUT_METHOD_SERVICE);
            if (ime != null) ime.showSoftInput(keyboardTarget, InputMethodManager.SHOW_IMPLICIT);
        });
    }

    void release() {
        surface.setOnTouchListener(null);
        surface.setOnGenericMotionListener(null);
        InputMethodManager ime = (InputMethodManager)
                activity.getSystemService(Activity.INPUT_METHOD_SERVICE);
        if (ime != null) ime.hideSoftInputFromWindow(keyboardTarget.getWindowToken(), 0);
    }

    private boolean capturePointer(MotionEvent event) {
        if (!pointerEnabled || surface.getWidth() <= 0 || surface.getHeight() <= 0) {
            return false;
        }
        if ((event.getSource() & InputDevice.SOURCE_CLASS_POINTER) == 0
                && event.getToolType(0) == MotionEvent.TOOL_TYPE_UNKNOWN) {
            return false;
        }
        float normalizedX = clamp(event.getX() / surface.getWidth());
        float normalizedY = clamp(event.getY() / surface.getHeight());
        listener.onPointerInput(new PointerEvent(
                event.getActionMasked(), normalizedX, normalizedY,
                event.getAxisValue(MotionEvent.AXIS_VSCROLL),
                event.getButtonState(), event.getSource(), event.getToolType(0)));
        return true;
    }

    private static float clamp(float value) {
        return Math.max(0f, Math.min(1f, value));
    }
}
