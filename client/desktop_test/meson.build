sdl2_dep = dependency('sdl2', required : false)
if not sdl2_dep.found()
  sdl2_dep = dependency('SDL2', required : false)
endif

if not sdl2_dep.found()
  message('Desktop test client: disabled (SDL2 not found)')
  subdir_done()
endif

executable('metashare-testclient',
  files('main.cpp'),
  include_directories : common_inc,
  dependencies : [
    sdl2_dep,
    dependency('libavcodec'),
    dependency('libavutil'),
  ],
  install : true)
