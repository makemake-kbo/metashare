project('metashare', 'cpp',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=2',
    'b_ndebug=if-release',
  ])

add_project_arguments('-Wno-unused-parameter', language : 'cpp')

# Shared protocol/common headers.
common_inc = include_directories('src/common')

subdir('src/streamer')
subdir('client/desktop_test')
