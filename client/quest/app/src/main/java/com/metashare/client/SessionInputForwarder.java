package com.metashare.client;

import android.view.MotionEvent;

/**
 * Bridges {@link RemoteInputController} callbacks onto a {@link StreamSession}:
 * normalizes Android pointer actions into evdev button transitions and relays
 * keys/text (already keysym-converted) to the host. One per streamed screen;
 * the session is bound once the surface's StreamSession exists.
 */
final class SessionInputForwarder implements RemoteInputController.Listener {

    private static final int BTN_LEFT = 272;    // Linux evdev codes
    private static final int BTN_RIGHT = 273;
    private static final int BTN_MIDDLE = 274;

    private volatile StreamSession session;

    // Pointer-button state as MotionEvent.BUTTON_* bits. Touch/trigger contact
    // carries no button bits, so a synthetic primary is held from ACTION_DOWN
    // until ACTION_UP/CANCEL; real mice report bits and diff cleanly.
    private int heldButtons;
    private boolean syntheticPrimary;
    private float scrollAccum;

    void setSession(StreamSession session) {
        this.session = session;
    }

    @Override
    public void onPointerInput(RemoteInputController.PointerEvent e) {
        StreamSession s = session;
        if (s == null) return;

        switch (e.action) {
            case MotionEvent.ACTION_DOWN:
                if (e.buttons == 0) syntheticPrimary = true;
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                syntheticPrimary = false;  // released via the diff below
                break;
            case MotionEvent.ACTION_SCROLL:
                scrollAccum += e.verticalScroll;
                int steps = (int) scrollAccum;
                if (steps != 0) {
                    scrollAccum -= steps;
                    // Android: positive = wheel away from the user (scroll
                    // up); the wire (libinput convention): positive = down.
                    s.sendScroll(-steps, 0);
                }
                break;
            default:
                break;
        }

        // Position before buttons, so a click lands where the cursor is.
        switch (e.action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_HOVER_ENTER:
            case MotionEvent.ACTION_HOVER_MOVE:
            case MotionEvent.ACTION_BUTTON_PRESS:
            case MotionEvent.ACTION_BUTTON_RELEASE:
                s.sendPointerMove(e.x, e.y);
                break;
            default:
                break;
        }

        int now = e.buttons
                | (syntheticPrimary ? MotionEvent.BUTTON_PRIMARY : 0);
        int changed = now ^ heldButtons;
        if (changed != 0) {
            emit(s, changed, now, MotionEvent.BUTTON_PRIMARY, BTN_LEFT);
            emit(s, changed, now, MotionEvent.BUTTON_SECONDARY, BTN_RIGHT);
            emit(s, changed, now, MotionEvent.BUTTON_TERTIARY, BTN_MIDDLE);
            heldButtons = now;
        }
    }

    private static void emit(StreamSession s, int changed, int now,
                             int androidBit, int evdevButton) {
        if ((changed & androidBit) != 0) {
            s.sendPointerButton(evdevButton, (now & androidBit) != 0);
        }
    }

    @Override
    public void onTextInput(String text) {
        StreamSession s = session;
        if (s == null) return;
        for (int i = 0; i < text.length(); ) {
            int cp = text.codePointAt(i);
            int keysym = Keysyms.fromCodePoint(cp);
            s.sendKey(keysym, true);
            s.sendKey(keysym, false);
            i += Character.charCount(cp);
        }
    }

    @Override
    public void onKeyInput(int keysym, boolean pressed) {
        StreamSession s = session;
        if (s != null) s.sendKey(keysym, pressed);
    }
}
