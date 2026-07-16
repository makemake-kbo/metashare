// Main window for the MetaShare streamer control panel.
//
// The window presents a GNOME-style list of encoder settings (codec, bitrate,
// frame rate) and a primary Start/Stop button. "Start" spawns
// `metashare-streamer` with the chosen CLI args; the streamer's stdout/stderr
// are tee'd into a log view. There is no video decoding here — the Quest (or
// SDL2 test client) is the consumer.

#include "main_window.hpp"

#include <glibmm.h>
#include <gtkmm.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int kMinFps = 1;
constexpr int kMaxFps = 240;
constexpr int kMinBitrate = 200;     // kbps
constexpr int kMaxBitrate = 200000;  // 200 Mbps
constexpr int kMinMonitors = 1;
constexpr int kMaxMonitors = 3;

// Mark the binary we look for in $PATH when no override is given.
constexpr const char* kStreamerBinary = "metashare-streamer";

// A small layer over the active GTK theme. Colours still follow the platform,
// while spacing and shape give the control panel a clearer visual hierarchy.
const char* const kUiCss = R"css(
  .hero-card {
    background: linear-gradient(135deg,
      alpha(@accent_bg_color, 0.17), alpha(@theme_bg_color, 0.65));
    border: 1px solid alpha(@accent_color, 0.22);
    border-radius: 18px;
    padding: 14px;
  }
  .status-dot {
    border-radius: 9999px;
    min-width: 10px;
    min-height: 10px;
  }
  .status-idle    { background-color: alpha(@theme_fg_color, 0.35); }
  .status-running { background-color: @success_color; }
  .status-busy    { background-color: @warning_color; }
  .status-error   { background-color: @error_color; }

  /* Subtle grouping for the primary action so it reads as the main CTA. */
  .primary-action {
    padding: 11px 18px;
    border-radius: 12px;
    font-weight: 700;
  }

  .section-label {
    color: alpha(@theme_fg_color, 0.55);
    font-size: 0.82em;
    font-weight: 700;
    letter-spacing: 0.04em;
  }

  .log-frame {
    background-color: alpha(@theme_fg_color, 0.035);
    border-radius: 14px;
  }
  .log-view {
    background-color: transparent;
    font-size: 0.92em;
  }
)css";

void inject_css_provider() {
    auto provider = Gtk::CssProvider::create();
    provider->load_from_data(kUiCss);
    Gtk::StyleProvider::add_provider_for_display(
        Gdk::Display::get_default(), provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

}  // namespace

MainWindow::MainWindow() { build_ui(); }

MainWindow::~MainWindow() {
    // Make sure we don't leave a streamer process orphaned if the user closes
    // the window mid-stream.
    if (running_) stop_streamer();
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

Gtk::ListBoxRow* MainWindow::make_row(const Glib::ustring& title,
                                      Gtk::Widget& control) {
    auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
    row->set_activatable(false);
    row->set_selectable(false);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    box->set_hexpand(true);
    box->set_margin_start(12);
    box->set_margin_end(12);
    box->set_margin_top(10);
    box->set_margin_bottom(10);

    auto* t = Gtk::make_managed<Gtk::Label>(title);
    t->set_halign(Gtk::Align::START);
    t->set_xalign(0.0f);
    t->set_hexpand(true);
    box->append(*t);

    control.set_halign(Gtk::Align::END);
    control.set_valign(Gtk::Align::CENTER);
    box->append(control);
    row->set_child(*box);
    return row;
}

Gtk::Label* MainWindow::make_section_label(const Glib::ustring& text) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->set_halign(Gtk::Align::START);
    l->set_xalign(0.0f);
    l->set_margin_start(6);
    l->set_margin_bottom(6);
    l->add_css_class("section-label");
    return l;
}

void MainWindow::build_ui() {
    set_title("MetaShare Streamer");
    set_default_size(500, 600);
    set_resizable(true);

    inject_css_provider();

    // ---- Header bar ---------------------------------------------------------
    // Keep the decoration clean: just the app name centred in the title slot.
    // Modern GNOME apps put the primary action either in the headerbar or as a
    // prominent button in the content area — we do the latter so the user can
    // always see status at a glance.
    auto* title_widget = Gtk::make_managed<Gtk::Label>();
    title_widget->set_text("MetaShare Streamer");
    title_widget->add_css_class("title");
    header_.set_title_widget(*title_widget);
    set_titlebar(header_);
    header_.set_show_title_buttons(true);

    // ---- Main column --------------------------------------------------------
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 20);
    root->set_margin_start(22);
    root->set_margin_end(22);
    root->set_margin_top(22);
    root->set_margin_bottom(22);

    // --- Status + primary action --------------------------------------------
    auto* hero = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    hero->add_css_class("hero-card");
    hero->set_valign(Gtk::Align::CENTER);

    status_dot_.set_size_request(10, 10);
    status_dot_.add_css_class("status-dot");
    status_dot_.add_css_class("status-idle");
    hero->append(status_dot_);

    status_label_.set_text("Idle");
    status_label_.set_xalign(0.0f);
    status_label_.set_halign(Gtk::Align::START);
    status_label_.set_hexpand(true);
    status_label_.add_css_class("dim-label");
    hero->append(status_label_);

    btn_primary_.set_label("Start Streaming");
    btn_primary_.add_css_class("suggested-action");
    btn_primary_.add_css_class("primary-action");
    // A play-symbol up front makes the call-to-action obvious; toggled to a
    // stop symbol when the stream is live.
    btn_primary_.set_icon_name("media-playback-start-symbolic");
    btn_primary_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_primary_clicked));
    hero->append(btn_primary_);
    root->append(*hero);

    // --- Settings list -------------------------------------------------------
    settings_box_.set_orientation(Gtk::Orientation::VERTICAL);
    settings_box_.set_spacing(0);

    root->append(*make_section_label("SETTINGS"));

    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->set_selection_mode(Gtk::SelectionMode::NONE);
    list->add_css_class("boxed-list");
    list->set_header_func([](Gtk::ListBoxRow* row, Gtk::ListBoxRow* before) {
        if (before) {
            auto* sep =
                Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
            row->set_header(*sep);
        }
    });

    // --- Codec ---
    {
        drop_codec_.set_model(
            Gtk::StringList::create({"HEVC (H.265)", "H.264"}));
        drop_codec_.set_tooltip_text("HEVC recommended; H.264 fallback.");
        list->append(*make_row("Codec", drop_codec_));
    }

    // --- Virtual displays ---
    {
        spin_monitors_.set_range(kMinMonitors, kMaxMonitors);
        spin_monitors_.set_increments(1, 1);
        spin_monitors_.set_numeric(true);
        spin_monitors_.set_value(kMaxMonitors);
        spin_monitors_.set_width_chars(3);
        list->append(*make_row("Virtual screens", spin_monitors_));
    }

    // --- Bitrate ---
    {
        spin_bitrate_.set_range(kMinBitrate, kMaxBitrate);
        spin_bitrate_.set_increments(500, 5000);
        spin_bitrate_.set_numeric(true);
        spin_bitrate_.set_value(8000);
        spin_bitrate_.set_width_chars(7);
        auto* wrap =
            Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        wrap->append(spin_bitrate_);
        auto* unit = Gtk::make_managed<Gtk::Label>("kbps");
        unit->add_css_class("dim-label");
        wrap->append(*unit);
        list->append(*make_row("Bitrate", *wrap));
    }

    // --- Frame rate ---
    {
        spin_fps_.set_range(kMinFps, kMaxFps);
        spin_fps_.set_increments(1, 5);
        spin_fps_.set_numeric(true);
        spin_fps_.set_value(60);
        spin_fps_.set_width_chars(5);
        auto* wrap =
            Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        wrap->append(spin_fps_);
        auto* unit = Gtk::make_managed<Gtk::Label>("fps");
        unit->add_css_class("dim-label");
        wrap->append(*unit);
        list->append(*make_row("Frame rate", *wrap));
    }

    settings_box_.append(*list);
    root->append(settings_box_);

    // ---- Log area ----
    auto* log_header =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* log_label = make_section_label("LOG");
    log_label->set_hexpand(true);
    log_label->set_margin_bottom(0);
    log_header->append(*log_label);
    auto* clear_log = Gtk::make_managed<Gtk::Button>("Clear");
    clear_log->add_css_class("flat");
    clear_log->set_tooltip_text("Clear streamer output");
    clear_log->signal_clicked().connect([this] { log_buf_->set_text(""); });
    log_header->append(*clear_log);
    root->append(*log_header);

    auto* log_frame = Gtk::make_managed<Gtk::Frame>();
    log_frame->add_css_class("log-frame");
    log_frame->set_child(log_scroll_);
    log_buf_ = Gtk::TextBuffer::create();
    log_view_.set_buffer(log_buf_);
    log_view_.set_editable(false);
    log_view_.set_cursor_visible(false);
    log_view_.set_monospace(true);
    log_view_.add_css_class("log-view");
    log_view_.set_wrap_mode(Gtk::WrapMode::WORD);
    log_view_.set_top_margin(8);
    log_view_.set_bottom_margin(8);
    log_view_.set_left_margin(8);
    log_view_.set_right_margin(8);
    log_scroll_.set_child(log_view_);
    log_scroll_.set_size_request(-1, 180);
    log_scroll_.set_vexpand(true);
    log_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC,
                           Gtk::PolicyType::AUTOMATIC);
    root->append(*log_frame);

    set_child(*root);

    // Initial state.
    set_status("Idle", "status-idle");
}

// ---------------------------------------------------------------------------
// Streamer control
// ---------------------------------------------------------------------------

Glib::ustring MainWindow::resolve_streamer_path() const {
    std::string found = Glib::find_program_in_path(kStreamerBinary);
    if (!found.empty()) return Glib::ustring(found);
    // Dev convenience: ./build/src/streamer/metashare-streamer
    if (::access("./build/src/streamer/metashare-streamer", X_OK) == 0)
        return "./build/src/streamer/metashare-streamer";
    return {};
}

std::vector<std::string> MainWindow::build_argv() const {
    std::vector<std::string> argv;
    argv.push_back(resolve_streamer_path().raw());
    if (argv.front().empty()) return {};  // caller checks and reports

    // The streamer's own defaults handle most things. We only override:
    //   --source portal  — always virtual monitors (not the physical screen),
    //                       so the Quest gets dedicated virtual displays.
    //   --monitors N     — stream the requested number of virtual displays.
    //   --fps/--bitrate/--codec — the three knobs exposed in the UI.
    argv.push_back("--source");
    argv.push_back("portal");

    argv.push_back("--monitors");
    argv.push_back(std::to_string(spin_monitors_.get_value_as_int()));

    argv.push_back("--fps");
    argv.push_back(std::to_string(spin_fps_.get_value_as_int()));

    argv.push_back("--bitrate");
    argv.push_back(std::to_string(spin_bitrate_.get_value_as_int()));

    argv.push_back("--codec");
    argv.push_back(drop_codec_.get_selected() == 1 ? "h264" : "hevc");

    return argv;
}

void MainWindow::on_primary_clicked() {
    if (running_) {
        stop_streamer();
    } else {
        start_streamer();
    }
}

void MainWindow::start_streamer() {
    auto argv = build_argv();
    if (argv.empty() || argv.front().empty()) {
        append_log("Could not find the streamer binary.\n"
                   "Install metashare-streamer somewhere on $PATH.\n");
        set_status("Streamer not found", "status-error");
        return;
    }

    // Show the resolved command so the user sees exactly what was launched.
    Glib::ustring cmd;
    for (const auto& a : argv) cmd += Glib::ustring(a) + " ";
    append_log("$ " + cmd + "\n");

    GPid pid = 0;
    int out_fd = -1, err_fd = -1;
    try {
        Glib::spawn_async_with_pipes(
            /* working_directory */ "", argv,
            // NOTE: no envp argument → child inherits the parent's environment.
            // Passing {} would give the child an EMPTY environment (no
            // DBUS_SESSION_BUS_ADDRESS, no WAYLAND_DISPLAY, etc.), which breaks
            // the portal source.
            Glib::SpawnFlags::DO_NOT_REAP_CHILD | Glib::SpawnFlags::SEARCH_PATH,
            /* child_setup */ sigc::slot<void()>(), &pid, /* stdin */ nullptr,
            &out_fd, &err_fd);
    } catch (const Glib::Error& e) {
        append_log(Glib::ustring("Failed to start streamer: ") + e.what() +
                   "\n");
        set_status("Failed to start", "status-error");
        return;
    }

    pid_ = pid;
    stdout_fd_ = out_fd;
    stderr_fd_ = err_fd;
    running_ = true;
    apply_running_state(true);

    // Watch the pipes asynchronously on the GLib main loop.
    if (stdout_fd_ >= 0) {
        io_stdout_conn_ = Glib::signal_io().connect(
            sigc::bind(sigc::mem_fun(*this, &MainWindow::on_pipe_io),
                       stdout_fd_),
            stdout_fd_,
            Glib::IOCondition::IO_IN | Glib::IOCondition::IO_HUP |
                Glib::IOCondition::IO_ERR);
    }
    if (stderr_fd_ >= 0) {
        io_stderr_conn_ = Glib::signal_io().connect(
            sigc::bind(sigc::mem_fun(*this, &MainWindow::on_pipe_io),
                       stderr_fd_),
            stderr_fd_,
            Glib::IOCondition::IO_IN | Glib::IOCondition::IO_HUP |
                Glib::IOCondition::IO_ERR);
    }
    child_watch_conn_ = Glib::signal_child_watch().connect(
        sigc::mem_fun(*this, &MainWindow::on_child_exited), pid_);

    set_status("Streaming", "status-running");
}

void MainWindow::stop_streamer() {
    if (!running_) return;
    set_status("Stopping…", "status-busy");
    if (pid_ > 0) {
        // The streamer installs SIGINT/SIGTERM handlers and shuts down cleanly.
        ::kill(pid_, SIGTERM);
    }
    // The actual cleanup happens in on_child_exited(). If the child doesn't
    // exit within a couple of seconds, escalate to SIGKILL.
    Glib::signal_timeout().connect_once(
        [this] {
            if (running_ && pid_ > 0) { ::kill(pid_, SIGKILL); }
        },
        2000);
}

void MainWindow::on_child_exited(GPid pid, int status) {
    (void)pid;
    Glib::spawn_close_pid(pid_);

    io_stdout_conn_.disconnect();
    io_stderr_conn_.disconnect();
    child_watch_conn_.disconnect();
    if (stdout_fd_ >= 0) {
        ::close(stdout_fd_);
        stdout_fd_ = -1;
    }
    if (stderr_fd_ >= 0) {
        ::close(stderr_fd_);
        stderr_fd_ = -1;
    }

    pid_ = 0;
    const bool was_running = running_;
    running_ = false;
    apply_running_state(false);

    Glib::ustring tail;
    Glib::ustring dot_class = "status-idle";
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        tail = "streamer exited (code " + std::to_string(code) + ")\n";
        if (was_running && code != 0) dot_class = "status-error";
    } else if (WIFSIGNALED(status)) {
        tail = "streamer killed (signal " + std::to_string(WTERMSIG(status)) +
               ")\n";
    } else {
        tail = "streamer exited\n";
    }
    append_log("\n[" + tail + "]\n");

    if (was_running && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        set_status("Crashed", dot_class);
    } else {
        set_status("Idle", dot_class);
    }
}

bool MainWindow::on_pipe_io(Glib::IOCondition cond, int fd) {
    const Glib::IOCondition interesting =
        Glib::IOCondition::IO_HUP | Glib::IOCondition::IO_ERR;
    if (static_cast<int>(cond & interesting) != 0) {
        // Drain any pending bytes; EOF will surface as a 0-byte read.
    }
    char buf[4096];
    ssize_t n = 0;
    do {
        n = ::read(fd, buf, sizeof(buf));
        if (n > 0) { append_log(Glib::ustring(buf, buf + n)); }
    } while (n == static_cast<ssize_t>(sizeof(buf)));  // drain fully

    if (n <= 0) {
        // EOF or error: drop this source. The fd itself is closed in
        // on_child_exited.
        return false;  // disconnect this IO source
    }
    return true;
}

void MainWindow::apply_running_state(bool running) {
    // Lock all settings while the streamer is running — changing them would
    // need a restart anyway.
    settings_box_.set_sensitive(!running);

    if (running) {
        btn_primary_.set_label("Stop Streaming");
        btn_primary_.remove_css_class("suggested-action");
        btn_primary_.add_css_class("destructive-action");
        btn_primary_.set_icon_name("media-playback-stop-symbolic");
    } else {
        btn_primary_.set_label("Start Streaming");
        btn_primary_.remove_css_class("destructive-action");
        btn_primary_.add_css_class("suggested-action");
        btn_primary_.set_icon_name("media-playback-start-symbolic");
    }
}

void MainWindow::set_status(const Glib::ustring& text,
                            const Glib::ustring& dot_class) {
    status_label_.set_text(text);
    // Swap the dot's colour class.
    status_dot_.remove_css_class("status-idle");
    status_dot_.remove_css_class("status-running");
    status_dot_.remove_css_class("status-busy");
    status_dot_.remove_css_class("status-error");
    status_dot_.add_css_class(dot_class);
}

void MainWindow::append_log(const Glib::ustring& text) {
    log_buf_->insert(log_buf_->end(), text);
    // Keep the latest output visible.
    auto insert_mark = log_buf_->get_insert();
    log_view_.scroll_to(insert_mark);
}
