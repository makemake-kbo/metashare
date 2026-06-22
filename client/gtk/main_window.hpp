// Main window for the MetaShare streamer control panel.

#pragma once

#include <gtkmm.h>

#include <sigc++/connection.h>
#include <sys/types.h>

#include <vector>
#include <string>

// A GNOME-styled control panel that launches `metashare-streamer` with the
// chosen settings and surfaces its log output. All controls are disabled while
// a stream is running; the user must stop the current stream to change them.
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

    // ---- Signal handlers ----
    void on_source_changed();
    void on_aspect_preset_changed();
    void on_width_changed();
    void on_height_changed();
    void on_lock_toggled();
    void on_start_clicked();
    void on_stop_clicked();
    bool on_pipe_io(Glib::IOCondition cond, int fd);
    void on_child_exited(GPid pid, int status);

    // ---- Streamer control ----
    void start_streamer();
    void stop_streamer();
    void apply_running_state(bool running);
    void append_log(const Glib::ustring& text);
    std::vector<std::string> build_argv() const;
    Glib::ustring resolve_streamer_path() const;

    // Resolution helpers (aspect-ratio locking + even-dim rounding).
    int current_aspect_num() const;
    int current_aspect_den() const;
    static int round_even(int v);

    // ---- Widgets ----
    Gtk::HeaderBar header_;
    Gtk::ToggleButton btn_start_;  // "Start" / doubles as status pill
    Gtk::Button btn_stop_;
    Gtk::Label status_label_;

    Gtk::DropDown drop_source_;
    Gtk::DropDown drop_aspect_;
    Gtk::SpinButton spin_width_;
    Gtk::SpinButton spin_height_;
    Gtk::Switch sw_lock_;
    Gtk::SpinButton spin_fps_;
    Gtk::SpinButton spin_bitrate_;
    Gtk::DropDown drop_codec_;
    Gtk::Switch sw_hw_;
    Gtk::SpinButton spin_port_;
    Gtk::Switch sw_discovery_;
    Gtk::Entry entry_streamer_path_;

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

    // Guards against recursive signal handlers when we set dimensions
    // programmatically (e.g. when re-applying the aspect ratio).
    bool updating_dims_ = false;

    // Aspect presets (label, num, den). 16:9 first as the default.
    struct AspectPreset {
        const char* label;
        int num;
        int den;
    };
    static const AspectPreset kPresets[];
};
