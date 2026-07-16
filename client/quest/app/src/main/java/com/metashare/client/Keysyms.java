package com.metashare.client;

import android.view.KeyCharacterMap;
import android.view.KeyEvent;

/**
 * Android-to-X11 keysym conversion for the remote-input wire protocol. The
 * host injects X11 keysyms (via the RemoteDesktop portal), which sidesteps
 * keyboard-layout mismatches: committed IME text maps codepoint-for-codepoint,
 * and only navigation/modifier keys need an explicit table.
 */
final class Keysyms {

    private Keysyms() {}

    /**
     * Keysym for one Unicode codepoint of committed text. Latin-1 printables
     * are their own keysym; everything else uses the reserved Unicode range.
     */
    static int fromCodePoint(int cp) {
        if (cp == '\n') return 0xff0d;  // XK_Return
        if (cp == '\t') return 0xff09;  // XK_Tab
        if (cp >= 0x20 && cp <= 0x7e) return cp;
        if (cp >= 0xa0 && cp <= 0xff) return cp;
        return 0x01000000 | cp;
    }

    /**
     * Keysym for a navigation/editing/modifier key that never produces text,
     * or 0 when the key isn't one (and should flow through the IME instead).
     */
    static int special(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_DEL: return 0xff08;             // BackSpace
            case KeyEvent.KEYCODE_FORWARD_DEL: return 0xffff;     // Delete
            case KeyEvent.KEYCODE_ENTER: return 0xff0d;           // Return
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return 0xff8d;    // KP_Enter
            case KeyEvent.KEYCODE_TAB: return 0xff09;             // Tab
            case KeyEvent.KEYCODE_ESCAPE: return 0xff1b;          // Escape
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0xff51;       // Left
            case KeyEvent.KEYCODE_DPAD_UP: return 0xff52;         // Up
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0xff53;      // Right
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0xff54;       // Down
            case KeyEvent.KEYCODE_MOVE_HOME: return 0xff50;       // Home
            case KeyEvent.KEYCODE_MOVE_END: return 0xff57;        // End
            case KeyEvent.KEYCODE_PAGE_UP: return 0xff55;         // Prior
            case KeyEvent.KEYCODE_PAGE_DOWN: return 0xff56;       // Next
            case KeyEvent.KEYCODE_INSERT: return 0xff63;          // Insert
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 0xffe1;      // Shift_L
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0xffe2;     // Shift_R
            case KeyEvent.KEYCODE_CTRL_LEFT: return 0xffe3;       // Control_L
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 0xffe4;      // Control_R
            case KeyEvent.KEYCODE_ALT_LEFT: return 0xffe9;        // Alt_L
            case KeyEvent.KEYCODE_ALT_RIGHT: return 0xffea;       // Alt_R
            case KeyEvent.KEYCODE_META_LEFT: return 0xffeb;       // Super_L
            case KeyEvent.KEYCODE_META_RIGHT: return 0xffec;      // Super_R
            case KeyEvent.KEYCODE_CAPS_LOCK: return 0xffe5;       // Caps_Lock
            default:
                if (keyCode >= KeyEvent.KEYCODE_F1
                        && keyCode <= KeyEvent.KEYCODE_F12) {
                    return 0xffbe + (keyCode - KeyEvent.KEYCODE_F1);  // F1..F12
                }
                return 0;
        }
    }

    /**
     * Keysym for a key pressed together with Ctrl/Alt/Meta (which the IME
     * won't turn into text, so shortcuts like Ctrl+C would otherwise vanish).
     * Uses the key's unmodified character so "Ctrl+C" sends keysym 'c'.
     * Returns 0 for keys with no base character.
     */
    static int chorded(KeyEvent event) {
        int ch = event.getUnicodeChar(0);
        if ((ch & KeyCharacterMap.COMBINING_ACCENT) != 0 || ch <= 0) return 0;
        return fromCodePoint(ch);
    }
}
