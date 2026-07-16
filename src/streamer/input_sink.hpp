// Injection side of remote input: capture backends that can synthesize input
// into the captured desktop implement this. The portal source injects via
// org.freedesktop.portal.RemoteDesktop on the same session it captures with,
// so pointer coordinates always target the surface the client is looking at.
// Backends that can't inject leave FrameSource::input_sink() at nullptr and
// the streamer silently drops client input.
//
// All methods may be called from the signaling thread and must be cheap /
// non-blocking; implementations should fire-and-forget.

#pragma once

#include <cstdint>

namespace metashare {

class InputSink {
  public:
    virtual ~InputSink() = default;

    // Absolute pointer position, normalized 0..1 across the streamed surface.
    virtual void pointer_motion(double nx, double ny) = 0;

    // Linux evdev button code (BTN_LEFT=272, BTN_RIGHT=273, BTN_MIDDLE=274).
    virtual void pointer_button(int evdev_button, bool pressed) = 0;

    // Discrete wheel steps; positive = down / right.
    virtual void pointer_scroll(int v_steps, int h_steps) = 0;

    // X11 keysym (Unicode chars are 0x01000000+codepoint).
    virtual void key(std::uint32_t keysym, bool pressed) = 0;
};

}  // namespace metashare
