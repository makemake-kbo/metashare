// Main window for the MetaShare streamer control panel.
//
// The window presents a GNOME-style list of settings (source, resolution with a
// locked aspect ratio, fps, bitrate, codec, hardware accel, port, discovery)
// and a Start/Stop button. "Start" spawns `metashare-streamer` with the chosen
// CLI args; the streamer's stdout/stderr are tee'd into a log view. There is no
// video decoding here — the Quest (or SDL2 test client) is the consumer.

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

constexpr int kMinDim = 16;
constexpr int kMaxDim = 7680;  // 8K cap
constexpr int kMinFps = 1;
constexpr int kMaxFps = 240;
constexpr int kMinBitrate = 200;     // kbps
constexpr int kMaxBitrate = 200000;  // 200 Mbps
constexpr int kMinPort = 1024;
constexpr int kMaxPort = 65535;

// Mark the binary we look for in $PATH when no override is given.
constexpr const char* kStreamerBinary = "metashare-streamer";

}  // namespace

// Aspect presets offered by the dropdown. 16:9 first (default selection).
const MainWindow::AspectPreset MainWindow::kPresets[] = {
    {"16:9", 16, 9}, {"16:10", 16, 10}, {"4:3", 4, 3}, {"21:9", 21, 9},
    {"32:9", 32, 9}, {"1:1", 1, 1},     {"3:2", 3, 2},
};

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
                                      const Glib::ustring& subtitle,
                                      Gtk::Widget& control) {
    auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
    row->set_activatable(false);
    row->set_selectable(false);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    box->set_margin_start(12);
    box->set_margin_end(12);
    box->set_margin_top(8);
    box->set_margin_bottom(8);

    auto* title_box =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    title_box->set_hexpand(true);

    auto* labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    labels->set_hexpand(true);
    labels->set_halign(Gtk::Align::START);
    labels->set_valign(Gtk::Align::CENTER);

    auto* t = Gtk::make_managed<Gtk::Label>(title);
    t->set_halign(Gtk::Align::START);
    t->set_xalign(0.0f);
    labels->append(*t);

    if (!subtitle.empty()) {
        auto* s = Gtk::make_managed<Gtk::Label>(subtitle);
        s->set_halign(Gtk::Align::START);
        s->set_xalign(0.0f);
        s->set_wrap(true);
        s->add_css_class("dim-label");
        labels->append(*s);
    }

    title_box->append(*labels);

    control.set_halign(Gtk::Align::END);
    control.set_valign(Gtk::Align::CENTER);
    title_box->append(control);

    box->append(*title_box);
    row->set_child(*box);
    return row;
}

void MainWindow::build_ui() {
    set_title("MetaShare Streamer");
    set_default_size(520, 720);
    set_resizable(true);

    // ---- Header bar ---------------------------------------------------------
    btn_start_.set_label("Start Stream");
    btn_start_.add_css_class("suggested-action");
    btn_start_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_start_clicked));

    btn_stop_.set_label("Stop");
    btn_stop_.add_css_class("destructive-action");
    btn_stop_.set_visible(false);
    btn_stop_.signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindow::on_stop_clicked));

    header_.pack_start(btn_start_);
    header_.pack_start(btn_stop_);

    auto* title_box =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    title_box->set_halign(Gtk::Align::CENTER);
    auto* title = Gtk::make_managed<Gtk::Label>();
    title->set_markup("<b>MetaShare Streamer</b>");
    title->set_single_line_mode(true);
    status_label_.set_text("Idle");
    status_label_.set_single_line_mode(true);
    status_label_.add_css_class("dim-label");
    title_box->append(*title);
    title_box->append(status_label_);
    header_.set_title_widget(*title_box);

    set_titlebar(header_);

    // ---- Main column --------------------------------------------------------
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    root->set_margin_start(12);
    root->set_margin_end(12);
    root->set_margin_top(12);
    root->set_margin_bottom(12);

    settings_box_.set_orientation(Gtk::Orientation::VERTICAL);
    settings_box_.set_spacing(0);

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

    // --- Source ---
    {
        drop_source_.set_model(
            Gtk::StringList::create({"Test pattern", "Wayland portal"}));
        drop_source_.set_tooltip_text(
            "Test pattern draws a synthetic moving image — use it when "
            "developing without a Wayland session. Portal captures a real "
            "Wayland monitor via xdg-desktop-portal.");
        drop_source_.property_selected().signal_changed().connect(
            sigc::mem_fun(*this, &MainWindow::on_source_changed));
        list->append(*make_row(
            "Capture source",
            "Test pattern works anywhere; portal needs a Wayland session.",
            drop_source_));
    }

    // --- Aspect ratio preset ---
    {
        std::vector<Glib::ustring> labels;
        for (const auto& p : kPresets) labels.emplace_back(p.label);
        drop_aspect_.set_model(Gtk::StringList::create(labels));
        drop_aspect_.property_selected().signal_changed().connect(
            sigc::mem_fun(*this, &MainWindow::on_aspect_preset_changed));
        list->append(
            *make_row("Aspect ratio",
                      "Used to keep width/height in proportion when locked.",
                      drop_aspect_));
    }

    // --- Width / Height ---
    {
        spin_width_.set_range(kMinDim, kMaxDim);
        spin_width_.set_increments(2, 16);
        spin_width_.set_numeric(true);
        spin_width_.set_value(1920);
        spin_width_.signal_value_changed().connect(
            sigc::mem_fun(*this, &MainWindow::on_width_changed));

        spin_height_.set_range(kMinDim, kMaxDim);
        spin_height_.set_increments(2, 16);
        spin_height_.set_numeric(true);
        spin_height_.set_value(1080);
        spin_height_.signal_value_changed().connect(
            sigc::mem_fun(*this, &MainWindow::on_height_changed));

        auto* dims =
            Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        spin_width_.set_width_chars(6);
        spin_height_.set_width_chars(6);
        dims->append(spin_width_);
        auto* x = Gtk::make_managed<Gtk::Label>("×");
        dims->append(*x);
        dims->append(spin_height_);
        list->append(*make_row(
            "Resolution",
            "Only applies to the test source. Rounded to even numbers.",
            *dims));
    }

    // --- Lock aspect switch ---
    {
        sw_lock_.set_active(true);
        sw_lock_.property_active().signal_changed().connect(
            sigc::mem_fun(*this, &MainWindow::on_lock_toggled));
        list->append(*make_row(
            "Lock aspect ratio",
            "When on, editing one dimension updates the other.", sw_lock_));
    }

    // --- FPS ---
    {
        spin_fps_.set_range(kMinFps, kMaxFps);
        spin_fps_.set_increments(1, 5);
        spin_fps_.set_numeric(true);
        spin_fps_.set_value(60);
        list->append(*make_row(
            "Frame rate", "Frames per second sent to the encoder.", spin_fps_));
    }

    // --- Bitrate ---
    {
        spin_bitrate_.set_range(kMinBitrate, kMaxBitrate);
        spin_bitrate_.set_increments(500, 5000);
        spin_bitrate_.set_numeric(true);
        spin_bitrate_.set_value(15000);
        list->append(*make_row("Bitrate",
                               "Encoder target, in kbps. 15 000 ≈ 1080p60.",
                               spin_bitrate_));
    }

    // --- Codec ---
    {
        drop_codec_.set_model(
            Gtk::StringList::create({"HEVC (H.265)", "H.264"}));
        list->append(*make_row("Codec",
                               "HEVC falls back to software H.264 if no HW "
                               "HEVC encoder is available.",
                               drop_codec_));
    }

    // --- Hardware accel ---
    {
        sw_hw_.set_active(true);
        list->append(*make_row("Hardware encoder",
                               "Use VAAPI/NVENC when available. Disable to "
                               "force software encoding.",
                               sw_hw_));
    }

    // --- Port ---
    {
        spin_port_.set_range(kMinPort, kMaxPort);
        spin_port_.set_increments(1, 10);
        spin_port_.set_numeric(true);
        spin_port_.set_value(7778);
        list->append(*make_row("TCP stream port",
                               "Where clients connect for the video stream.",
                               spin_port_));
    }

    // --- Discovery ---
    {
        sw_discovery_.set_active(true);
        list->append(*make_row(
            "UDP discovery",
            "Broadcast the streamer on UDP 7777 so clients need no address.",
            sw_discovery_));
    }

    // --- Streamer binary override ---
    {
        entry_streamer_path_.set_placeholder_text(
            "auto (" + Glib::ustring(kStreamerBinary) + ")");
        entry_streamer_path_.set_width_chars(20);
        list->append(
            *make_row("Streamer binary",
                      "Optional override. Leave empty to search $PATH.",
                      entry_streamer_path_));
    }

    settings_box_.append(*list);
    root->append(settings_box_);

    // ---- Log area ----
    auto* log_label = Gtk::make_managed<Gtk::Label>();
    log_label->set_markup("<b>Streamer output</b>");
    log_label->set_halign(Gtk::Align::START);
    log_label->set_margin_top(12);
    log_label->set_margin_bottom(4);
    root->append(*log_label);

    log_buf_ = Gtk::TextBuffer::create();
    log_view_.set_buffer(log_buf_);
    log_view_.set_editable(false);
    log_view_.set_cursor_visible(false);
    log_view_.set_monospace(true);
    log_view_.set_wrap_mode(Gtk::WrapMode::WORD);
    log_scroll_.set_child(log_view_);
    log_scroll_.set_size_request(-1, 160);
    log_scroll_.set_vexpand(true);
    log_scroll_.set_policy(Gtk::PolicyType::AUTOMATIC,
                           Gtk::PolicyType::AUTOMATIC);
    root->append(log_scroll_);

    set_child(*root);

    // Initial state.
    on_source_changed();
}

// ---------------------------------------------------------------------------
// Resolution helpers
// ---------------------------------------------------------------------------

int MainWindow::round_even(int v) {
    if (v < kMinDim) v = kMinDim;
    if (v > kMaxDim) v = kMaxDim;
    return v & ~1;  // clear low bit (must be even for H.264/HEVC)
}

int MainWindow::current_aspect_num() const {
    const auto idx = drop_aspect_.get_selected();
    if (idx >= std::size(kPresets)) return 16;
    return kPresets[idx].num;
}

int MainWindow::current_aspect_den() const {
    const auto idx = drop_aspect_.get_selected();
    if (idx >= std::size(kPresets)) return 9;
    return kPresets[idx].den;
}

void MainWindow::on_lock_toggled() {
    if (sw_lock_.get_active()) {
        // Snap current dimensions to the chosen ratio.
        updating_dims_ = true;
        int h = round_even(spin_width_.get_value_as_int() *
                           current_aspect_den() / current_aspect_num());
        spin_height_.set_value(h);
        updating_dims_ = false;
    }
}

void MainWindow::on_aspect_preset_changed() {
    if (!sw_lock_.get_active()) return;
    updating_dims_ = true;
    int h = round_even(spin_width_.get_value_as_int() * current_aspect_den() /
                       current_aspect_num());
    spin_height_.set_value(h);
    updating_dims_ = false;
}

void MainWindow::on_width_changed() {
    if (updating_dims_ || !sw_lock_.get_active()) return;
    updating_dims_ = true;
    int h = round_even(spin_width_.get_value_as_int() * current_aspect_den() /
                       current_aspect_num());
    spin_height_.set_value(h);
    updating_dims_ = false;
}

void MainWindow::on_height_changed() {
    if (updating_dims_ || !sw_lock_.get_active()) return;
    updating_dims_ = true;
    int w = round_even(spin_height_.get_value_as_int() * current_aspect_num() /
                       current_aspect_den());
    spin_width_.set_value(w);
    updating_dims_ = false;
}

void MainWindow::on_source_changed() {
    // Width/height only affect the test source; the portal source uses the
    // monitor's own resolution.
    const bool portal = drop_source_.get_selected() == 1;
    spin_width_.set_sensitive(!portal);
    spin_height_.set_sensitive(!portal);
    drop_aspect_.set_sensitive(!portal);
    sw_lock_.set_sensitive(!portal);
}

// ---------------------------------------------------------------------------
// Streamer control
// ---------------------------------------------------------------------------

Glib::ustring MainWindow::resolve_streamer_path() const {
    Glib::ustring ov = entry_streamer_path_.get_text();
    if (!ov.empty()) {
        if (::access(ov.c_str(), X_OK) == 0) return ov;
        // Could be a bare name to be looked up in PATH.
        std::string found = Glib::find_program_in_path(ov.raw());
        if (!found.empty()) return Glib::ustring(found);
        // Fall through to default lookup so we still produce a helpful message.
    }
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

    const bool portal = drop_source_.get_selected() == 1;
    argv.push_back("--source");
    argv.push_back(portal ? "portal" : "test");

    if (!portal) {
        argv.push_back("--width");
        argv.push_back(
            std::to_string(round_even(spin_width_.get_value_as_int())));
        argv.push_back("--height");
        argv.push_back(
            std::to_string(round_even(spin_height_.get_value_as_int())));
    }

    argv.push_back("--fps");
    argv.push_back(std::to_string(spin_fps_.get_value_as_int()));

    argv.push_back("--bitrate");
    argv.push_back(std::to_string(spin_bitrate_.get_value_as_int()));

    argv.push_back("--codec");
    argv.push_back(drop_codec_.get_selected() == 1 ? "h264" : "hevc");

    if (!sw_hw_.get_active()) argv.push_back("--no-hw");

    argv.push_back("--port");
    argv.push_back(std::to_string(spin_port_.get_value_as_int()));

    if (!sw_discovery_.get_active()) argv.push_back("--no-discovery");

    return argv;
}

void MainWindow::on_start_clicked() {
    if (running_) return;
    start_streamer();
}

void MainWindow::on_stop_clicked() {
    if (!running_) return;
    stop_streamer();
}

void MainWindow::start_streamer() {
    auto argv = build_argv();
    if (argv.empty() || argv.front().empty()) {
        append_log("Could not find the streamer binary.\n"
                   "Install metashare-streamer or set a path under 'Streamer "
                   "binary'.\n");
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
            /* envp */ {},
            Glib::SpawnFlags::DO_NOT_REAP_CHILD | Glib::SpawnFlags::SEARCH_PATH,
            /* child_setup */ sigc::slot<void()>(), &pid, /* stdin */ nullptr,
            &out_fd, &err_fd);
    } catch (const Glib::Error& e) {
        append_log(Glib::ustring("Failed to start streamer: ") + e.what() +
                   "\n");
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

    status_label_.set_text("Running");
}

void MainWindow::stop_streamer() {
    if (!running_) return;
    status_label_.set_text("Stopping…");
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
    running_ = false;
    apply_running_state(false);

    Glib::ustring tail;
    if (WIFEXITED(status)) {
        tail = "streamer exited (code " + std::to_string(WEXITSTATUS(status)) +
               ")\n";
    } else if (WIFSIGNALED(status)) {
        tail = "streamer killed (signal " + std::to_string(WTERMSIG(status)) +
               ")\n";
    } else {
        tail = "streamer exited\n";
    }
    append_log("\n[" + tail + "]\n");
    status_label_.set_text("Idle");
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
    btn_start_.set_visible(!running);
    btn_stop_.set_visible(running);
    // Lock all settings while the streamer is running — changing them would
    // need a restart anyway.
    settings_box_.set_sensitive(!running);
}

void MainWindow::append_log(const Glib::ustring& text) {
    log_buf_->insert(log_buf_->end(), text);
    // Keep the latest output visible.
    auto insert_mark = log_buf_->get_insert();
    log_view_.scroll_to(insert_mark);
}
