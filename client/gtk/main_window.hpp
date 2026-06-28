// Main window for the MetaShare streamer control panel.

#pragma once

#include <gtkmm.h>

#include <sigc++/connection.h>
#include <sys/types.h>

#include <vector>
#include <string>

// A GNOME-styled control panel that launches `metashare-streamer` with the
// chosen settings and surfaces its log output. The only user-facing knobs are
// codec / bitrate / frame rate — everything else (capture source, resolution,
// hardware encode, ports, discovery) is left to the streamer's sane defaults.
// All controls are disabled while a stream is running.
class MainWindow : public Gtk::Window {
  public:
    MainWindow();
    ~MainWindow() override;

  private:
    // ---- UI construction ----
    void build_ui();
    Gtk::ListBoxRow* make_row(const Glib::ustring& title,
                              const Glib::ustring& subtitle,
                              Gtk::Widget& control);
    Gtk::Label* make_section_label(const Glib::ustring& text);

    // ---- Signal handlers ----
    void on_primary_clicked();
    bool on_pipe_io(Glib::IOCondition cond, int fd);
    void on_child_exited(GPid pid, int status);

    // ---- Streamer control ----
    void start_streamer();
    void stop_streamer();
    void apply_running_state(bool running);
    void set_status(const Glib::ustring& text, const Glib::ustring& dot_class);
    void append_log(const Glib::ustring& text);
    std::vector<std::string> build_argv() const;
    Glib::ustring resolve_streamer_path() const;

    // ---- Widgets ----
    Gtk::HeaderBar header_;
    Gtk::Button btn_primary_;  // full-width Start/Stop in content area
    Gtk::Label status_label_;
    Gtk::Image status_dot_;  // coloured dot next to the status text

    Gtk::SpinButton spin_fps_;
    Gtk::SpinButton spin_bitrate_;
    Gtk::DropDown drop_codec_;

    Gtk::ScrolledWindow log_scroll_;
    Gtk::TextView log_view_;
    Glib::RefPtr<Gtk::TextBuffer> log_buf_;

    // Container holding all setting rows — disabled as a group while running.
    Gtk::Box settings_box_;

    // ---- Subprocess state ----
    GPid pid_ = 0;
    bool running_ = false;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;
    sigc::connection io_stdout_conn_;
    sigc::connection io_stderr_conn_;
    sigc::connection child_watch_conn_;
};
