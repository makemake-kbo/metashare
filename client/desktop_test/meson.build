# The desktop test client still drives the legacy WebRTC/libdatachannel stack
# (SDP offer/answer + ICE). The v4 migration switched the streamer to raw RTP,
# so this client is dormant pending a raw-RTP rewrite. It is intentionally kept
# out of the build to avoid pulling libdatachannel back in.
message('Desktop test client: disabled (pending raw-RTP rewrite)')
subdir_done()

sdl2_dep = dependency('sdl2', required : false)
if not sdl2_dep.found()
  sdl2_dep = dependency('SDL2', required : false)
endif

if not sdl2_dep.found()
  message('Desktop test client: disabled (SDL2 not found)')
  subdir_done()
endif

rtc_dep = dependency('libdatachannel', required : false)
if not rtc_dep.found()
  rtc_dep = dependency('LibDataChannel', method : 'cmake', required : false)
endif
if not rtc_dep.found()
  error('libdatachannel not found for desktop_test client')
endif

executable('metashare-testclient',
  files('main.cpp',
        '../../src/common/signaling.cpp',
        '../../src/common/signaling_client.cpp'),
  include_directories : common_inc,
  dependencies : [
    sdl2_dep,
    rtc_dep,
    dependency('libavcodec'),
    dependency('libavutil'),
    dependency('libswresample'),
  ],
  install : true)
