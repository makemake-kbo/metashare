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
