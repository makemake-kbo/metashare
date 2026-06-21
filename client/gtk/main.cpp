// MetaShare streamer — GTK4 control panel entry point.

#include "main_window.hpp"

#include <gtkmm.h>

int main(int argc, char* argv[]) {
  // GApplication ID; registered so the app is single-instance on the desktop.
  auto app = Gtk::Application::create("dev.metashare.StreamerUI",
                                      Gio::Application::Flags::DEFAULT_FLAGS);
  return app->make_window_and_run<MainWindow>(argc, argv);
}
