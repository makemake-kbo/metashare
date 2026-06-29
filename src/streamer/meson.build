cc = meson.get_compiler('cpp')

# Core encode/network deps — always required.
threads_dep = dependency('threads')
avcodec_dep = dependency('libavcodec')
avutil_dep = dependency('libavutil')
swscale_dep = dependency('libswscale')
swresample_dep = dependency('libswresample')

streamer_srcs = files(
  'main.cpp',
  'test_pattern_source.cpp',
  'test_tone_source.cpp',
  'audio_encoder.cpp',
  'encoder.cpp',
  'signaling_server.cpp',
  'rtp_packetizer.cpp',
  'rtp_server.cpp',
  'discovery.cpp',
  '../common/signaling.cpp',
)

streamer_deps = [
  threads_dep, avcodec_dep, avutil_dep, swscale_dep, swresample_dep,
]

streamer_args = []

# Both the xdg-desktop-portal source and the Mutter ScreenCast source need
# sdbus-c++ + libpipewire. Either being enabled makes them hard requirements.
portal_opt = get_option('portal')
mutter_opt = get_option('mutter')
need_dbus_pw = portal_opt.enabled() or mutter_opt.enabled()

sdbus_dep   = dependency('sdbus-c++',        required : need_dbus_pw)
pipewire_dep = dependency('libpipewire-0.3', required : need_dbus_pw)
have_dbus_pw = sdbus_dep.found() and pipewire_dep.found()
# Set to true by whichever block (portal/mutter) actually added sources +
# defines; the audio TU depends on one of those defines being live.
have_pw_audio = false

# --- xdg-desktop-portal capture (any compliant Wayland compositor) ----------
if not portal_opt.disabled() and have_dbus_pw
  streamer_srcs += files('portal_pipewire_source.cpp')
  streamer_deps += [sdbus_dep, pipewire_dep]
  streamer_args += '-DMETASHARE_HAVE_PORTAL'
  have_pw_audio = true
  message('Wayland portal capture: ENABLED')
else
  message('Wayland portal capture: disabled (test-pattern source only)')
endif

# --- Mutter ScreenCast virtual-monitor source (GNOME 44+) -------------------
if not mutter_opt.disabled() and have_dbus_pw
  streamer_srcs += files(
    'mutter_screencast.cpp',
    'mutter_virtual_source.cpp',
  )
  streamer_deps += [sdbus_dep, pipewire_dep]
  streamer_args += '-DMETASHARE_HAVE_MUTTER'
  have_pw_audio = true
  message('Mutter ScreenCast virtual-monitor source: ENABLED')

  # Phase 0 standalone PoC tool: creates one virtual monitor (or captures an
  # existing one) and dumps a single frame as a PPM. Default mode is
  # portal-virtual, which works on any GNOME 44+ user session; mutter-direct
  # requires system/headless Mutter where session-creation isn't inhibited.
  executable('mutter_virtual_dump',
    ['mutter_virtual_dump.cpp',
     'mutter_screencast.cpp',
     'mutter_virtual_source.cpp',
     'portal_pipewire_source.cpp'],
    include_directories : common_inc,
    dependencies : [threads_dep, avutil_dep, sdbus_dep, pipewire_dep],
    cpp_args : ['-DMETASHARE_HAVE_MUTTER', '-DMETASHARE_HAVE_PORTAL'])
else
  message('Mutter ScreenCast virtual-monitor source: disabled')
endif

# --- PipeWire audio capture (system loopback + microphone) ------------------
# Either portal or mutter already pulled in libpipewire, so adding this TU is
# free. Built into the streamer so a `--audio` request can always find a real
# capture backend when the host has PipeWire running. Gated on the same
# METASHARE_HAVE_PORTAL / METASHARE_HAVE_MUTTER defines in main.cpp, so we
# only link it when one of those got set.
if have_pw_audio
  streamer_srcs += files('pipewire_audio_source.cpp')
  message('Audio capture (PipeWire loopback + mic): ENABLED')
else
  message('Audio capture: disabled (needs PipeWire via portal or mutter)')
endif

executable('metashare-streamer',
  streamer_srcs,
  include_directories : common_inc,
  dependencies : streamer_deps,
  cpp_args : streamer_args,
  install : true)
