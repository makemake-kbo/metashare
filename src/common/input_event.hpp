// Remote-input events — decoded from the signaling channel's INPUT lines.
//
// The wire body is a compact space-separated form (trivially parseable, like
// READY's port). One event per line:
//
//   m <x> <y>       pointer move; normalized floats 0..1 in the streamed
//                   surface (the client doesn't know the host resolution)
//   b <btn> <0|1>   pointer button; Linux evdev code (BTN_LEFT=272,
//                   BTN_RIGHT=273, BTN_MIDDLE=274), 1=pressed
//   s <v> <h>       discrete scroll steps; positive = down / right
//   k <sym> <0|1>   keyboard key; X11 keysym (Unicode maps to
//                   0x01000000+codepoint), 1=pressed
//
// Keysyms (not evdev keycodes) are used for the keyboard so the client can
// send committed IME text without either side needing an XKB layout: the
// compositor's virtual keyboard resolves keysyms directly.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace metashare::input {

struct Event {
    enum class Kind { kMove, kButton, kScroll, kKey };
    Kind kind{};
    double x = 0, y = 0;       // kMove, normalized 0..1
    int button = 0;            // kButton, evdev code
    bool pressed = false;      // kButton / kKey
    std::uint32_t keysym = 0;  // kKey
    int scroll_v = 0;          // kScroll, positive = down
    int scroll_h = 0;          // kScroll, positive = right
};

// Parse one INPUT body. Returns false on malformed input; out is left in an
// unspecified state on failure.
inline bool parse(const std::string& body, Event& out) {
    if (body.size() < 3 || body[1] != ' ') return false;
    const char* rest = body.c_str() + 2;
    switch (body[0]) {
    case 'm': {
        double x = 0, y = 0;
        if (std::sscanf(rest, "%lf %lf", &x, &y) != 2) return false;
        if (!(x >= 0.0 && x <= 1.0) || !(y >= 0.0 && y <= 1.0)) return false;
        out.kind = Event::Kind::kMove;
        out.x = x;
        out.y = y;
        return true;
    }
    case 'b': {
        int b = 0, s = 0;
        if (std::sscanf(rest, "%d %d", &b, &s) != 2) return false;
        out.kind = Event::Kind::kButton;
        out.button = b;
        out.pressed = (s != 0);
        return true;
    }
    case 's': {
        int v = 0, h = 0;
        if (std::sscanf(rest, "%d %d", &v, &h) != 2) return false;
        out.kind = Event::Kind::kScroll;
        out.scroll_v = v;
        out.scroll_h = h;
        return true;
    }
    case 'k': {
        unsigned sym = 0;
        int s = 0;
        if (std::sscanf(rest, "%u %d", &sym, &s) != 2) return false;
        out.kind = Event::Kind::kKey;
        out.keysym = sym;
        out.pressed = (s != 0);
        return true;
    }
    default: return false;
    }
}

}  // namespace metashare::input
