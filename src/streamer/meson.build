cc = meson.get_compiler('cpp')

# Core encode/network deps — always required.
threads_dep = dependency('threads')
avcodec_dep = dependency('libavcodec')
avutil_dep = dependency('libavutil')
swscale_dep = dependency('libswscale')

streamer_srcs = files(
  'main.cpp',
  'test_pattern_source.cpp',
  'encoder.cpp',
  'net_server.cpp',
  'discovery.cpp',
)

streamer_deps = [
  threads_dep, avcodec_dep, avutil_dep, swscale_dep,
]

streamer_args = []

# Optional real Wayland capture: xdg-desktop-portal (sdbus-c++) + PipeWire.
portal_opt = get_option('portal')
sdbus_dep = dependency('sdbus-c++', required : portal_opt)
pipewire_dep = dependency('libpipewire-0.3', required : portal_opt)

if not portal_opt.disabled() and sdbus_dep.found() and pipewire_dep.found()
  streamer_srcs += files('portal_pipewire_source.cpp')
  streamer_deps += [sdbus_dep, pipewire_dep]
  streamer_args += '-DMETASHARE_HAVE_PORTAL'
  message('Wayland portal capture: ENABLED')
else
  message('Wayland portal capture: disabled (test-pattern source only)')
endif

executable('metashare-streamer',
  streamer_srcs,
  include_directories : common_inc,
  dependencies : streamer_deps,
  cpp_args : streamer_args,
  install : true)
