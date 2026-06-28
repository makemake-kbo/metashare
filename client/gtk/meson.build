# Modern GNOME/GTK4 frontend (control panel) for the MetaShare streamer.
#
# This is a launcher only: it spawns `metashare-streamer` as a subprocess with
# the chosen settings and surfaces its stdout/stderr. It does not decode or
# display the video — that stays with the Quest / SDL2 clients.

gtkmm_dep = dependency('gtkmm-4.0', required : false)
if not gtkmm_dep.found()
  message('GTK4 frontend: disabled (gtkmm-4.0 not found)')
  subdir_done()
endif

executable('metashare-streamer-ui',
  files(
    'main.cpp',
    'main_window.cpp',
  ),
  dependencies : [gtkmm_dep],
  install : true)

# GNOME desktop entry so the launcher shows up in the app grid.
install_data(
  'dev.metashare.StreamerUI.desktop',
  install_dir : get_option('datadir') / 'applications')

# App icon + wordmark logo + mark, shipped as scalable named icons under the
# GNOME icon theme. The app-id name (`dev.metashare.StreamerUI`) is what the
# desktop entry's Icon= and Gtk::Image lookups resolve against.
icondir = get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps'
install_data(
  files('../../data/brand/metashare-icon.svg',
        '../../data/brand/metashare-logo.svg',
        '../../data/brand/metashare-mark.svg'),
  install_dir : icondir,
  rename : [
    'dev.metashare.StreamerUI.svg',
    'dev.metashare.StreamerUI-logo.svg',
    'dev.metashare.StreamerUI-mark.svg',
  ])
