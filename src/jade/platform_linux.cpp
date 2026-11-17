// Jade's native layer on Linux: see platform.h, and docs/linux-gui-plan.html.
//
// Wayland, talked to directly, as platform_win.cpp talks Win32: a surface with xdg-shell's
// toplevel role for each window, its OpenGL 3.3 context through EGL, the keyboard read with
// xkbcommon. One native layer for KDE Plasma and GNOME alike (both run Wayland and speak its
// common protocols); what one offers and the other does not (a title bar drawn by the
// compositor) is asked for, and done without where missing.
//
// ww is linked with none of the system's libraries for it: each is opened as the first window
// is made (dlopen), so that ww runs where they are missing (a server, a container), and a
// program that opens a window there is told there is no display. The functions the protocol
// headers call are redirected below to pointers filled from the libraries; the protocols'
// descriptions (wl_interface) are made from their XML by wayland-scanner as ww is built
// (CMakeLists.txt) and included at the end of this file.

#if defined(__linux__) && defined(WW_WAYLAND)

#include <dlfcn.h>
#include <fcntl.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// ---- The libraries' functions, reached through pointers ----
// Each name below, as the system's headers declare it, becomes a pointer of the function's own
// type (the header's declaration of it, so redirected, defines the pointer), filled from the
// library as it is opened. Every call, ours and the headers' own inline ones, goes through it.

#define wl_display_connect (*ww_wl_display_connect)
#define wl_display_disconnect (*ww_wl_display_disconnect)
#define wl_display_get_fd (*ww_wl_display_get_fd)
#define wl_display_dispatch_pending (*ww_wl_display_dispatch_pending)
#define wl_display_dispatch_queue_pending (*ww_wl_display_dispatch_queue_pending)
#define wl_display_flush (*ww_wl_display_flush)
#define wl_display_roundtrip (*ww_wl_display_roundtrip)
#define wl_display_prepare_read (*ww_wl_display_prepare_read)
#define wl_display_prepare_read_queue (*ww_wl_display_prepare_read_queue)
#define wl_display_read_events (*ww_wl_display_read_events)
#define wl_display_cancel_read (*ww_wl_display_cancel_read)
#define wl_display_create_queue (*ww_wl_display_create_queue)
#define wl_display_get_error (*ww_wl_display_get_error)
#define wl_event_queue_destroy (*ww_wl_event_queue_destroy)
#define wl_proxy_set_queue (*ww_wl_proxy_set_queue)
#define wl_proxy_marshal_flags (*ww_wl_proxy_marshal_flags)
#define wl_proxy_add_listener (*ww_wl_proxy_add_listener)
#define wl_proxy_destroy (*ww_wl_proxy_destroy)
#define wl_proxy_get_version (*ww_wl_proxy_get_version)
#define wl_proxy_set_user_data (*ww_wl_proxy_set_user_data)
#define wl_proxy_get_user_data (*ww_wl_proxy_get_user_data)
#include <wayland-client-core.h>

#define wl_egl_window_create (*ww_wl_egl_window_create)
#define wl_egl_window_destroy (*ww_wl_egl_window_destroy)
#define wl_egl_window_resize (*ww_wl_egl_window_resize)
#include <wayland-egl-core.h>

#define wl_cursor_theme_load (*ww_wl_cursor_theme_load)
#define wl_cursor_theme_destroy (*ww_wl_cursor_theme_destroy)
#define wl_cursor_theme_get_cursor (*ww_wl_cursor_theme_get_cursor)
#define wl_cursor_image_get_buffer (*ww_wl_cursor_image_get_buffer)
#include <wayland-cursor.h>

#include <wayland-client-protocol.h>

#include "fractional-scale-client.h"
#include "viewporter-client.h"
#include "xdg-decoration-client.h"
#include "pointer-constraints-client.h"
#include "relative-pointer-client.h"
#include "xdg-activation-client.h"
#include "xdg-foreign-client.h"
#include "xdg-shell-client.h"

#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include "gl.h"
#include "platform.h"
#include "portal.h"

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace ww::jade {

namespace {

// ---- Opening the libraries ----

// EGL's and xkbcommon's functions, by the types their headers give them.
struct Egl {
    decltype(&::eglGetProcAddress) GetProcAddress = nullptr;
    decltype(&::eglGetPlatformDisplay) GetPlatformDisplay = nullptr;
    decltype(&::eglInitialize) Initialize = nullptr;
    decltype(&::eglBindAPI) BindAPI = nullptr;
    decltype(&::eglChooseConfig) ChooseConfig = nullptr;
    decltype(&::eglCreateContext) CreateContext = nullptr;
    decltype(&::eglCreateWindowSurface) CreateWindowSurface = nullptr;
    decltype(&::eglDestroySurface) DestroySurface = nullptr;
    decltype(&::eglMakeCurrent) MakeCurrent = nullptr;
    decltype(&::eglSwapBuffers) SwapBuffers = nullptr;
    decltype(&::eglSwapInterval) SwapInterval = nullptr;
    decltype(&::eglQueryString) QueryString = nullptr;
    decltype(&::eglGetError) GetError = nullptr;
    decltype(&::eglQuerySurface) QuerySurface = nullptr;
} egl;

struct Xkb {
    decltype(&::xkb_context_new) context_new = nullptr;
    decltype(&::xkb_keymap_new_from_string) keymap_new_from_string = nullptr;
    decltype(&::xkb_keymap_unref) keymap_unref = nullptr;
    decltype(&::xkb_keymap_key_repeats) keymap_key_repeats = nullptr;
    decltype(&::xkb_state_new) state_new = nullptr;
    decltype(&::xkb_state_unref) state_unref = nullptr;
    decltype(&::xkb_state_update_mask) state_update_mask = nullptr;
    decltype(&::xkb_state_key_get_one_sym) state_key_get_one_sym = nullptr;
    decltype(&::xkb_state_key_get_utf8) state_key_get_utf8 = nullptr;
    decltype(&::xkb_state_mod_name_is_active) state_mod_name_is_active = nullptr;
    decltype(&::xkb_keysym_to_utf32) keysym_to_utf32 = nullptr;
    decltype(&::xkb_keysym_to_upper) keysym_to_upper = nullptr;  // xkbcommon 1.8 and later: may be missing
    decltype(&::xkb_compose_table_new_from_locale) compose_table_new_from_locale = nullptr;
    decltype(&::xkb_compose_table_unref) compose_table_unref = nullptr;
    decltype(&::xkb_compose_state_new) compose_state_new = nullptr;
    decltype(&::xkb_compose_state_unref) compose_state_unref = nullptr;
    decltype(&::xkb_compose_state_feed) compose_state_feed = nullptr;
    decltype(&::xkb_compose_state_get_status) compose_state_get_status = nullptr;
    decltype(&::xkb_compose_state_get_utf8) compose_state_get_utf8 = nullptr;
    decltype(&::xkb_compose_state_reset) compose_state_reset = nullptr;
} xkb;

// A library by the name it is installed under; null, with `error` saying which, if it is not there.
void* openLibrary(const char* name, std::string& error) {
    void* handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr && error.empty()) error = std::string("the library ") + name + " is not installed";
    return handle;
}
// A function of a library into `slot`; false, with `error` naming it, where it is missing.
template <typename T>
bool find(void* library, T& slot, const char* name, std::string& error) {
    slot = reinterpret_cast<T>(dlsym(library, name));
    if (slot == nullptr && error.empty()) error = std::string("the system's libraries lack ") + name;
    return slot != nullptr;
}

bool loadLibraries(std::string& error) {
    void* client = openLibrary("libwayland-client.so.0", error);
    void* eglWindow = openLibrary("libwayland-egl.so.1", error);
    void* cursor = openLibrary("libwayland-cursor.so.0", error);
    void* eglLibrary = openLibrary("libEGL.so.1", error);
    void* keys = openLibrary("libxkbcommon.so.0", error);
    if (!error.empty()) return false;
    bool ok = true;
#define WW_FIND(library, name) ok = find(library, ww_##name, #name, error) && ok
    WW_FIND(client, wl_display_connect);
    WW_FIND(client, wl_display_disconnect);
    WW_FIND(client, wl_display_get_fd);
    WW_FIND(client, wl_display_dispatch_pending);
    WW_FIND(client, wl_display_dispatch_queue_pending);
    WW_FIND(client, wl_display_flush);
    WW_FIND(client, wl_display_roundtrip);
    WW_FIND(client, wl_display_prepare_read);
    WW_FIND(client, wl_display_prepare_read_queue);
    WW_FIND(client, wl_display_read_events);
    WW_FIND(client, wl_display_cancel_read);
    WW_FIND(client, wl_display_create_queue);
    WW_FIND(client, wl_display_get_error);
    WW_FIND(client, wl_event_queue_destroy);
    WW_FIND(client, wl_proxy_set_queue);
    WW_FIND(client, wl_proxy_marshal_flags);
    WW_FIND(client, wl_proxy_add_listener);
    WW_FIND(client, wl_proxy_destroy);
    WW_FIND(client, wl_proxy_get_version);
    WW_FIND(client, wl_proxy_set_user_data);
    WW_FIND(client, wl_proxy_get_user_data);
    WW_FIND(eglWindow, wl_egl_window_create);
    WW_FIND(eglWindow, wl_egl_window_destroy);
    WW_FIND(eglWindow, wl_egl_window_resize);
    WW_FIND(cursor, wl_cursor_theme_load);
    WW_FIND(cursor, wl_cursor_theme_destroy);
    WW_FIND(cursor, wl_cursor_theme_get_cursor);
    WW_FIND(cursor, wl_cursor_image_get_buffer);
#undef WW_FIND
#define WW_EGL(name) ok = find(eglLibrary, egl.name, "egl" #name, error) && ok
    WW_EGL(GetProcAddress);
    WW_EGL(GetPlatformDisplay);
    WW_EGL(Initialize);
    WW_EGL(BindAPI);
    WW_EGL(ChooseConfig);
    WW_EGL(CreateContext);
    WW_EGL(CreateWindowSurface);
    WW_EGL(DestroySurface);
    WW_EGL(MakeCurrent);
    WW_EGL(SwapBuffers);
    WW_EGL(SwapInterval);
    WW_EGL(QueryString);
    WW_EGL(GetError);
    WW_EGL(QuerySurface);
#undef WW_EGL
#define WW_XKB(name) ok = find(keys, xkb.name, "xkb_" #name, error) && ok
    WW_XKB(context_new);
    WW_XKB(keymap_new_from_string);
    WW_XKB(keymap_unref);
    WW_XKB(keymap_key_repeats);
    WW_XKB(state_new);
    WW_XKB(state_unref);
    WW_XKB(state_update_mask);
    WW_XKB(state_key_get_one_sym);
    WW_XKB(state_key_get_utf8);
    WW_XKB(state_mod_name_is_active);
    WW_XKB(keysym_to_utf32);
    WW_XKB(compose_table_new_from_locale);
    WW_XKB(compose_table_unref);
    WW_XKB(compose_state_new);
    WW_XKB(compose_state_unref);
    WW_XKB(compose_state_feed);
    WW_XKB(compose_state_get_status);
    WW_XKB(compose_state_get_utf8);
    WW_XKB(compose_state_reset);
#undef WW_XKB
    xkb.keysym_to_upper = reinterpret_cast<decltype(xkb.keysym_to_upper)>(dlsym(keys, "xkb_keysym_to_upper"));
    return ok;
}

// ---- Keys ----

// Which key a Linux key code is (evdev's, which Wayland gives), by where the key is on the
// keyboard, whatever the layout: from 1 to 88 they are the PC's scan codes, which platform_win's
// table reads too.
Key keyOf(uint32_t code) {
    static const struct {
        uint32_t code;
        Key key;
    } table[] = {
        {30, Key::A}, {48, Key::B}, {46, Key::C}, {32, Key::D}, {18, Key::E}, {33, Key::F}, {34, Key::G}, {35, Key::H},
        {23, Key::I}, {36, Key::J}, {37, Key::K}, {38, Key::L}, {50, Key::M}, {49, Key::N}, {24, Key::O}, {25, Key::P},
        {16, Key::Q}, {19, Key::R}, {31, Key::S}, {20, Key::T}, {22, Key::U}, {47, Key::V}, {17, Key::W}, {45, Key::X},
        {21, Key::Y}, {44, Key::Z},
        {11, Key::Digit0}, {2, Key::Digit1}, {3, Key::Digit2}, {4, Key::Digit3}, {5, Key::Digit4}, {6, Key::Digit5},
        {7, Key::Digit6}, {8, Key::Digit7}, {9, Key::Digit8}, {10, Key::Digit9},
        {57, Key::Space}, {40, Key::Apostrophe}, {51, Key::Comma}, {12, Key::Minus}, {52, Key::Period}, {53, Key::Slash},
        {39, Key::Semicolon}, {13, Key::Equal}, {26, Key::LeftBracket}, {43, Key::Backslash}, {27, Key::RightBracket},
        {41, Key::GraveAccent}, {1, Key::Escape}, {28, Key::Enter}, {15, Key::Tab}, {14, Key::Backspace},
        {58, Key::CapsLock}, {70, Key::ScrollLock}, {69, Key::NumLock},
        {82, Key::Numpad0}, {79, Key::Numpad1}, {80, Key::Numpad2}, {81, Key::Numpad3}, {75, Key::Numpad4}, {76, Key::Numpad5},
        {77, Key::Numpad6}, {71, Key::Numpad7}, {72, Key::Numpad8}, {73, Key::Numpad9}, {83, Key::NumpadDecimal},
        {55, Key::NumpadMultiply}, {74, Key::NumpadSubtract}, {78, Key::NumpadAdd}, {96, Key::NumpadEnter}, {98, Key::NumpadDivide},
        {117, Key::NumpadEqual},
        {42, Key::ShiftLeft}, {54, Key::ShiftRight}, {29, Key::ControlLeft}, {97, Key::ControlRight}, {56, Key::AltLeft},
        {100, Key::AltRight}, {125, Key::MetaLeft}, {126, Key::MetaRight},
        {110, Key::Insert}, {111, Key::Delete}, {106, Key::Right}, {105, Key::Left}, {108, Key::Down}, {103, Key::Up},
        {104, Key::PageUp}, {109, Key::PageDown}, {102, Key::Home}, {107, Key::End},
        {99, Key::PrintScreen}, {119, Key::Pause}, {127, Key::Menu}, {139, Key::Menu},
        {87, Key::F11}, {88, Key::F12},
    };
    if (code >= 59 && code <= 68) return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(code - 59));
    if (code >= 183 && code <= 194) return static_cast<Key>(static_cast<int>(Key::F13) + static_cast<int>(code - 183));
    for (const auto& entry : table)
        if (entry.code == code) return entry.key;
    return Key::Unknown;
}

int64_t nowMillis() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<int64_t>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

// WW_WAYLAND_TRACE=1: what the compositor asks and what is done, to standard error, for finding
// out what a desktop does (2026-11-16: KDE's resizing).
bool tracing() {
    static const bool on = [] {
        const char* v = std::getenv("WW_WAYLAND_TRACE");
        return v != nullptr && v[0] == '1';
    }();
    return on;
}
#define WW_TRACE(...)                                      \
    do {                                                   \
        if (tracing()) {                                   \
            std::fprintf(stderr, "wayland: " __VA_ARGS__); \
            std::fputc('\n', stderr);                      \
        }                                                  \
    } while (0)
// Under the trace, where the program is (which call of the window layer's, or none: the program's
// own code), and a thread that says so where it stays in one for seconds: a hang found where it
// is, as the compositor says the window is not responding.
std::atomic<const char*> stage{"starting"};
std::atomic<uint64_t> stageCount{0};
void at(const char* where) {
    if (!tracing()) return;
    stage.store(where);
    stageCount.fetch_add(1);
}
// The main thread's stack, printed by the main thread itself as the watchdog asks (SIGUSR1):
// addresses within ww, for addr2line.
pthread_t mainThread;
void printStack(int) {
    void* frames[64];
    const int n = backtrace(frames, 64);
    static const char said[] = "wayland: watchdog: the main thread's stack:\n";
    if (write(2, said, sizeof said - 1) < 0) {
    }
    backtrace_symbols_fd(frames, n, 2);
}
void watch() {
    mainThread = pthread_self();
    signal(SIGUSR1, printStack);
    std::thread([] {
        uint64_t seen = 0;
        int still = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint64_t now = stageCount.load();
            still = now == seen && std::strncmp(stage.load(), "wait:", 5) != 0 ? still + 1 : 0;  // (waiting with nothing to do is no hang)
            seen = now;
            if (still == 3 || (still > 3 && still % 10 == 0)) WW_TRACE("watchdog: in %s for %d s", stage.load(), still);
            if (still == 5) pthread_kill(mainThread, SIGUSR1);
        }
    }).detach();
}

class LinuxPlatform;

// ---- A window ----

class LinuxWindow final : public PlatformWindow {
public:
    LinuxPlatform& platform;
    wl_surface* surface = nullptr;
    xdg_surface* shell = nullptr;      // its role while shown
    xdg_toplevel* toplevel = nullptr;
    zxdg_toplevel_decoration_v1* decoration = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_v1* fractional = nullptr;
    wl_egl_window* eglWindow = nullptr;
    EGLSurface eglSurface = EGL_NO_SURFACE;

    std::string title;
    bool dialog = false, resizable = true;
    double fixedScale = 0;             // a scale given by a test, in place of the screen's
    double preferred = 1;              // the compositor's for this window
    int logicalWidth = 1, logicalHeight = 1;  // the size in the compositor's units, which the layout's are
    int pendingWidth = 0, pendingHeight = 0;  // a configure's, until it is done
    int minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;  // device pixels
    int x = 0, y = 0;                  // kept, not used: Wayland places windows itself
    bool shown = false, configured = false;
    bool configurePending = false;      // a configure came, not yet done
    uint32_t configureSerial = 0;
    LinuxWindow* owner = nullptr;
    Cursor cursor = Cursor::Default;
    PointerMode pointerMode = PointerMode::Normal;
    zwp_locked_pointer_v1* lock = nullptr;  // while the pointer mode is Locked
    zxdg_exported_v2* exported = nullptr;   // while shown: its name for other programs' windows (the portal's dialogs)
    std::string exportHandle;               // that name, once given; read by a dialog's thread (LinuxPlatform::handlesMutex)
    bool lockActive = false;                // the compositor holds the pointer now (it may let go and take it again)
    int buttons = 0;

    explicit LinuxWindow(LinuxPlatform& p) : platform(p) {}
    ~LinuxWindow() override;

    void emit(const RawEvent& event) {
        if (onEvent) onEvent(event);
    }

    double scale() const override { return fixedScale > 0 ? fixedScale : preferred; }
    int width() const override { return std::max(1, static_cast<int>(std::lround(logicalWidth * scale()))); }
    int height() const override { return std::max(1, static_cast<int>(std::lround(logicalHeight * scale()))); }

    void show() override;
    void hide() override;
    bool visible() const override { return shown; }
    void setTitle(const std::string& text) override {
        title = text;
        if (toplevel != nullptr) xdg_toplevel_set_title(toplevel, title.c_str());
    }
    void setSize(int width, int height) override {
        WW_TRACE("setSize %d x %d at scale %.4f", width, height, scale());
        logicalWidth = std::max(1, static_cast<int>(std::lround(std::max(width, 1) / scale())));
        logicalHeight = std::max(1, static_cast<int>(std::lround(std::max(height, 1) / scale())));
        sized();
    }
    void position(int& px, int& py) const override {
        px = x;
        py = y;
    }
    void setPosition(int px, int py) override {
        x = px;
        y = py;
    }
    void setLimits(int minW, int minH, int maxW, int maxH) override {
        WW_TRACE("setLimits %d %d %d %d", minW, minH, maxW, maxH);
        minWidth = minW;
        minHeight = minH;
        maxWidth = maxW;
        maxHeight = maxH;
        applyLimits();
    }
    void setResizable(bool on) override {
        resizable = on;
        applyLimits();
    }
    void toFront() override;
    void requestAttention() override;
    void setOwner(PlatformWindow* to) override {
        owner = static_cast<LinuxWindow*>(to);
        if (toplevel != nullptr) xdg_toplevel_set_parent(toplevel, owner != nullptr ? owner->toplevel : nullptr);
    }
    bool focused() const override;
    void setIcon(const std::vector<uint8_t>&, int, int) override {}  // Wayland's common protocols have none: the desktop file's
    void setCursor(Cursor shape) override;
    void setPointerMode(PointerMode mode) override;
    void postForTest(const std::string& what) override;
    bool makeCurrent() override;
    void swapBuffers() override;

    // The limits in the compositor's units; a window not resizable held at its size.
    void applyLimits() {
        if (toplevel == nullptr) return;
        const double s = scale();
        auto units = [&](int px) { return px > 0 ? std::max(1, static_cast<int>(std::lround(px / s))) : 0; };
        if (!resizable) {
            xdg_toplevel_set_min_size(toplevel, logicalWidth, logicalHeight);
            xdg_toplevel_set_max_size(toplevel, logicalWidth, logicalHeight);
        } else {
            xdg_toplevel_set_min_size(toplevel, units(minWidth), units(minHeight));
            xdg_toplevel_set_max_size(toplevel, maxWidth > 0 && maxHeight > 0 ? units(maxWidth) : 0, maxWidth > 0 && maxHeight > 0 ? units(maxHeight) : 0);
        }
    }
    // The picture's size from the window's size and scale: the EGL window, and the viewport that
    // shows it at the window's size where the scale is fractional (else the buffer's own scale).
    void sized();
    // The newest configure done: acknowledged, the size it says taken (0 by 0: the program's
    // own), and the program told and asked to draw.
    void configure() {
        if (!configurePending || shell == nullptr) return;
        configurePending = false;
        xdg_surface_ack_configure(shell, configureSerial);
        const bool first = !configured;
        configured = true;
        const int oldWidth = logicalWidth, oldHeight = logicalHeight;
        if (pendingWidth > 0 && pendingHeight > 0) {
            logicalWidth = pendingWidth;
            logicalHeight = pendingHeight;
        }
        WW_TRACE("configure %u done: %d x %d", configureSerial, logicalWidth, logicalHeight);
        const int64_t began = nowMillis();
        if (first || oldWidth != logicalWidth || oldHeight != logicalHeight) sized();
        at("configure: the program drawing");
        if (onExpose) onExpose();
        if (const int64_t took = nowMillis() - began; took > 50) WW_TRACE("configure: the program's answer took %lld ms", static_cast<long long>(took));
    }
    void scaleChanged(double now) {
        WW_TRACE("preferred scale %.4f (was %.4f, fixed %.4f)", now, preferred, fixedScale);
        if (fixedScale > 0 || std::fabs(now - preferred) < 1e-9) return;
        preferred = now;
        RawEvent e;
        e.kind = RawEvent::Scale;
        e.scale = scale();
        emit(e);
        sized();
        applyLimits();
    }
};

// ---- The platform ----

class LinuxPlatform final : public Platform {
public:
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    uint32_t compositorVersion = 0;
    wl_shm* shm = nullptr;
    xdg_wm_base* shell = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    zxdg_decoration_manager_v1* decorations = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractionalScales = nullptr;
    xdg_activation_v1* activation = nullptr;
    zxdg_exporter_v2* exporter = nullptr;
    std::mutex handlesMutex;           // the windows' exportHandle, which a dialog's thread reads
    zwp_pointer_constraints_v1* constraints = nullptr;
    zwp_relative_pointer_manager_v1* relativePointers = nullptr;
    zwp_relative_pointer_v1* relativePointer = nullptr;
    // The clipboard and what is dragged in: the compositor's offers (each with the kinds of data it
    // can give, as it said them), the clipboard's now, ours where we set it, and a drag's while
    // it is over one of our windows.
    wl_data_device_manager* dataManager = nullptr;
    uint32_t dataManagerVersion = 0;
    wl_data_device* dataDevice = nullptr;
    std::vector<std::pair<wl_data_offer*, std::vector<std::string>>> offers;
    wl_data_offer* selectionOffer = nullptr;
    wl_data_source* ownSource = nullptr;
    std::string ownText;
    wl_data_offer* dropOffer = nullptr;
    LinuxWindow* dropWindow = nullptr;
    double dropX = 0, dropY = 0;       // device pixels
    // The last key or button pressed, or the keyboard's coming in: Wayland lets a program set the
    // clipboard, or ask for a window to come to the front, only with such a serial.
    uint32_t inputSerial = 0;
    std::string startToken;            // XDG_ACTIVATION_TOKEN, as a launcher gave it: the first window shown with it
    struct Activation {
        LinuxPlatform* platform;
        LinuxWindow* target;           // null once it is gone
    };
    std::vector<Activation*> activations;  // tokens asked for, not yet given
    struct Output {
        wl_output* output;
        uint32_t name;
        int scale;
    };
    std::vector<Output> outputs;
    int wakeFd = -1;

    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface currentSurface = EGL_NO_SURFACE;
    bool contextCurrent = false;

    xkb_context* keys = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* keyState = nullptr;
    xkb_state* plainState = nullptr;  // the layout alone, no modifiers: what a key types plainly
    xkb_compose_state* compose = nullptr;
    int32_t repeatRate = 25, repeatDelay = 600;
    uint32_t repeating = 0;            // the key code repeating, plus 1; 0 for none
    int64_t repeatAt = 0;

    std::vector<LinuxWindow*> windows;
    LinuxWindow* pointerFocus = nullptr;
    LinuxWindow* keyboardFocus = nullptr;
    uint32_t pointerSerial = 0;        // the last entry's, which setting the pointer's shape needs
    double pointerX = 0, pointerY = 0; // device pixels, in the window under it
    double wheelX = 0, wheelY = 0;     // a frame's wheel, in notches
    bool wheelDiscrete = false;
    wl_cursor_theme* cursorTheme = nullptr;
    int cursorThemeSize = 0;
    wl_surface* cursorSurface = nullptr;
    std::vector<std::pair<LinuxWindow*, RawEvent>> posted;  // events made for tests, handed out as events are pumped
    // The pointer's moves, made one as Windows makes them (2026-11-16): Wayland sends each, and a
    // program slower to draw than the pointer moves (one drawing for each, in a virtual machine)
    // fell seconds behind them. The moves of one batch of events are one, the last place with the
    // whole distance; any other event sends it first, so that the order holds.
    LinuxWindow* moveWindow = nullptr;
    RawEvent move;
    void flushMove();

    bool start(std::string& error);

    LinuxWindow* windowOf(wl_surface* s) const {
        if (s == nullptr) return nullptr;
        for (LinuxWindow* w : windows)
            if (w->surface == s) return w;
        return nullptr;
    }

    std::unique_ptr<PlatformWindow> createWindow(const WindowSettings& settings, PlatformWindow* owner, std::string& error) override;

    bool use(EGLSurface s) {
        if (contextCurrent && currentSurface == s) return true;
        if (!egl.MakeCurrent(eglDisplay, s, s, context)) return false;
        currentSurface = s;
        contextCurrent = true;
        return true;
    }
    bool makeCurrentOffscreen() override { return use(EGL_NO_SURFACE); }
    // An EGL surface on a window's EGL window, shown at once when drawn: no wait for the screen's
    // refresh in EGL itself (a window the compositor does not show would wait for ever); the
    // toolkit's own timer paces animations (LinuxWindow::swapBuffers).
    EGLSurface windowSurface(wl_egl_window* window) {
        EGLSurface s = egl.CreateWindowSurface(eglDisplay, config, reinterpret_cast<EGLNativeWindowType>(window), nullptr);
        if (s != EGL_NO_SURFACE && use(s)) egl.SwapInterval(eglDisplay, 0);
        return s;
    }

    // Events handled (pump, with the runtime's lock held), and waited for (wait, without it:
    // only waiting, nothing handled), as the runtime's event loop asks.
    void pump();
    void wait(int64_t millis);
    void pumpEvents() override { pump(); }
    void waitEvents(int64_t millis) override { wait(millis); }
    void wake() override {
        const uint64_t one = 1;
        if (write(wakeFd, &one, sizeof one) < 0) {
        }
    }

    // The desktop's own dialog, through its portal (KDE's under KDE, GNOME's under GNOME), on
    // the calling thread (not the one pumping events: SystemFileDialog's), in front of `owner`.
    bool chooseFile(int kind, const std::string& title, const std::vector<std::pair<std::string, std::string>>& filters, const std::string& startDir,
                    const std::string& name, PlatformWindow* owner, std::string& path, std::string& error) override {
        std::string parent;
        if (owner != nullptr) {
            std::lock_guard<std::mutex> hold(handlesMutex);
            const std::string& handle = static_cast<LinuxWindow*>(owner)->exportHandle;
            if (!handle.empty()) parent = "wayland:" + handle;
        }
        return portal::chooseFile(kind, title, filters, startDir, name, parent, path, error);
    }
    std::string clipboardText() override;
    void setClipboardText(const std::string& text) override;
    const std::vector<std::string>* typesOf(wl_data_offer* offer) const {
        for (const auto& [o, types] : offers)
            if (o == offer) return &types;
        return nullptr;
    }
    bool hasType(wl_data_offer* offer, const char* type) const {
        const std::vector<std::string>* types = typesOf(offer);
        return types != nullptr && std::find(types->begin(), types->end(), type) != types->end();
    }
    void forget(wl_data_offer* offer) {
        if (offer == nullptr) return;
        offers.erase(std::remove_if(offers.begin(), offers.end(), [&](const auto& o) { return o.first == offer; }), offers.end());
        wl_data_offer_destroy(offer);
    }
    // What an offer gives as `type`, read through a pipe from the program that offers it (at most
    // 2 s of waiting for it).
    std::string readOffer(wl_data_offer* offer, const char* type);
    // A window brought to the front (with the last input's serial, which says the user asked for
    // it) or marked as wanting attention (without: the compositor will not raise it unasked).
    void activate(LinuxWindow& target, bool asked);
    void lockPointer(LinuxWindow& w, bool on);
    // A pointer move given to the window, gathered with the others of one batch of events.
    void moved(LinuxWindow* w, const RawEvent& e);
    int doubleClickTime() override { return 400; }
    double screenScale() override {
        int most = 1;
        for (const Output& o : outputs) most = std::max(most, o.scale);
        return most;
    }

    // The pointer's shape over the window it is in, from the cursor theme (the shapes' names as
    // CSS's and X's both have them).
    void showCursor();
    void pressKey(uint32_t code, bool down, bool repeat);
    int modifiers() const {
        if (keyState == nullptr) return 0;
        int bits = 0;
        if (xkb.state_mod_name_is_active(keyState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) bits |= kShift;
        if (xkb.state_mod_name_is_active(keyState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) bits |= kControl;
        if (xkb.state_mod_name_is_active(keyState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) bits |= kAlt;
        if (xkb.state_mod_name_is_active(keyState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) bits |= kMeta;
        return bits;
    }
};

LinuxPlatform* thePlatform = nullptr;

void LinuxPlatform::flushMove() {
    if (moveWindow == nullptr) return;
    LinuxWindow* w = moveWindow;
    moveWindow = nullptr;
    w->emit(move);
}

void LinuxPlatform::moved(LinuxWindow* w, const RawEvent& e) {
    if (moveWindow == w) {
        move.x = e.x;
        move.y = e.y;
        move.dx += e.dx;
        move.dy += e.dy;
        move.buttons = e.buttons;
        move.modifiers = e.modifiers;
        return;
    }
    flushMove();
    move = e;
    moveWindow = w;
}

// ---- Listeners ----
// Each event of each object, as the protocol's header lists them, in its order. An object is
// bound at no higher a version than these handle.

// The registry: what the compositor offers.
void onGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
void onGlobalRemove(void* data, wl_registry*, uint32_t name) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    for (size_t i = 0; i < p.outputs.size(); i++)
        if (p.outputs[i].name == name) {
            wl_output_destroy(p.outputs[i].output);
            p.outputs.erase(p.outputs.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}
const wl_registry_listener registryListener = {onGlobal, onGlobalRemove};

void onPing(void*, xdg_wm_base* shell, uint32_t serial) {
    WW_TRACE("ping %u", serial);
    xdg_wm_base_pong(shell, serial);
}
const xdg_wm_base_listener shellListener = {onPing};

// A screen: its whole scale, where the compositor gives no fractional one.
void onOutputScale(void* data, wl_output* output, int32_t factor) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    for (auto& o : p.outputs)
        if (o.output == output) o.scale = std::max(1, factor);
}
const wl_output_listener outputListener = {
    [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {},
    [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    [](void*, wl_output*) {},
    onOutputScale,
    [](void*, wl_output*, const char*) {},
    [](void*, wl_output*, const char*) {},
};

// A window's surface: the scale the compositor would have its picture drawn at (whole), where it
// gives no fractional one.
void onSurfaceScale(void* data, wl_surface*, int32_t factor) {
    auto* w = static_cast<LinuxWindow*>(data);
    if (w->platform.fractionalScales == nullptr) w->scaleChanged(std::max(1, factor));
}
const wl_surface_listener surfaceListener = {
    [](void*, wl_surface*, wl_output*) {},
    [](void*, wl_surface*, wl_output*) {},
    onSurfaceScale,
    [](void*, wl_surface*, uint32_t) {},
};

void onFractionalScale(void* data, wp_fractional_scale_v1*, uint32_t scale120) { static_cast<LinuxWindow*>(data)->scaleChanged(scale120 / 120.0); }
const wp_fractional_scale_v1_listener fractionalListener = {onFractionalScale};

// The toplevel's size and states, then the surface's configure saying they are all there.
void onToplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array* states) {
    auto* w = static_cast<LinuxWindow*>(data);
    if (tracing()) {
        std::string said;
        for (size_t i = 0; i + 4 <= states->size; i += 4) {
            uint32_t s;
            std::memcpy(&s, static_cast<const char*>(states->data) + i, 4);
            said += " " + std::to_string(s);
        }
        WW_TRACE("toplevel configure %d x %d, states%s", width, height, said.c_str());
    }
    w->pendingWidth = width;
    w->pendingHeight = height;
}
void onToplevelClose(void* data, xdg_toplevel*) {
    static_cast<LinuxWindow*>(data)->platform.flushMove();
    RawEvent e;
    e.kind = RawEvent::Close;
    static_cast<LinuxWindow*>(data)->emit(e);
}
const xdg_toplevel_listener toplevelListener = {
    onToplevelConfigure,
    onToplevelClose,
    [](void*, xdg_toplevel*, int32_t, int32_t) {},  // configure_bounds
    [](void*, xdg_toplevel*, wl_array*) {},         // wm_capabilities
};
// Only noted: a drag of an edge sends one for nearly each move of the pointer, and a program that
// redrew for each fell a minute behind (2026-11-16, KDE). The newest is done once the events
// waiting are all read (LinuxWindow::configure), and only it acknowledged, as xdg-shell allows.
void onShellConfigure(void* data, xdg_surface*, uint32_t serial) {
    auto* w = static_cast<LinuxWindow*>(data);
    WW_TRACE("surface configure %u: pending %d x %d", serial, w->pendingWidth, w->pendingHeight);
    w->configureSerial = serial;
    w->configurePending = true;
}
const xdg_surface_listener shellSurfaceListener = {onShellConfigure};

void onDecorationConfigure(void*, zxdg_toplevel_decoration_v1*, uint32_t) {}
const zxdg_toplevel_decoration_v1_listener decorationListener = {onDecorationConfigure};

// ---- The pointer ----

void onPointerEnter(void* data, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    p.flushMove();
    p.pointerFocus = p.windowOf(surface);
    p.pointerSerial = serial;
    if (p.pointerFocus == nullptr) return;
    const double s = p.pointerFocus->scale();
    p.pointerX = wl_fixed_to_double(sx) * s;
    p.pointerY = wl_fixed_to_double(sy) * s;
    p.showCursor();
    RawEvent e;
    e.kind = RawEvent::PointerMove;
    e.x = static_cast<int>(std::floor(p.pointerX));
    e.y = static_cast<int>(std::floor(p.pointerY));
    e.buttons = p.pointerFocus->buttons;
    e.modifiers = p.modifiers();
    p.pointerFocus->emit(e);
}
void onPointerLeave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    p.flushMove();
    if (p.pointerFocus != nullptr) {
        RawEvent e;
        e.kind = RawEvent::PointerLeave;
        p.pointerFocus->emit(e);
    }
    p.pointerFocus = nullptr;
}
void onPointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (p.pointerFocus == nullptr) return;
    const double s = p.pointerFocus->scale();
    const double nx = wl_fixed_to_double(sx) * s, ny = wl_fixed_to_double(sy) * s;
    RawEvent e;
    e.kind = RawEvent::PointerMove;
    e.x = static_cast<int>(std::floor(nx));
    e.y = static_cast<int>(std::floor(ny));
    e.dx = nx - p.pointerX;
    e.dy = ny - p.pointerY;
    e.buttons = p.pointerFocus->buttons;
    e.modifiers = p.modifiers();
    p.pointerX = nx;
    p.pointerY = ny;
    p.moved(p.pointerFocus, e);
}
void onPointerButton(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    p.flushMove();
    if (p.pointerFocus == nullptr) return;
    // evdev's buttons: left, right, middle, side (back), extra (forward).
    if (button < 0x110 || button > 0x114) return;
    p.pointerSerial = serial;
    const int which = static_cast<int>(button - 0x110);
    const bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
    LinuxWindow& w = *p.pointerFocus;
    if (down) p.inputSerial = serial;
    if (down) w.buttons |= 1 << which;
    else w.buttons &= ~(1 << which);
    RawEvent e;
    e.kind = down ? RawEvent::PointerDown : RawEvent::PointerUp;
    e.x = static_cast<int>(std::floor(p.pointerX));
    e.y = static_cast<int>(std::floor(p.pointerY));
    e.button = which;
    e.buttons = w.buttons;
    e.modifiers = p.modifiers();
    w.emit(e);
}
// A wheel's turn: in notches where the compositor gives them (value120, or discrete steps),
// else from the distance (a touchpad's, 10 units a notch); up and left positive.
void onPointerAxis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (p.wheelDiscrete) return;
    const double notches = -wl_fixed_to_double(value) / 10.0;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) p.wheelY += notches;
    else p.wheelX += notches;
}
void onPointerAxisValue120(void* data, wl_pointer*, uint32_t axis, int32_t value120) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (!p.wheelDiscrete) p.wheelX = p.wheelY = 0;  // the distances of this frame were the same turn
    p.wheelDiscrete = true;
    const double notches = -value120 / 120.0;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) p.wheelY += notches;
    else p.wheelX += notches;
}
void onPointerAxisDiscrete(void* data, wl_pointer* pointer, uint32_t axis, int32_t discrete) { onPointerAxisValue120(data, pointer, axis, discrete * 120); }
void onPointerFrame(void* data, wl_pointer*) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (p.pointerFocus != nullptr && (p.wheelX != 0 || p.wheelY != 0)) {
        p.flushMove();
        RawEvent e;
        e.kind = RawEvent::Wheel;
        e.x = static_cast<int>(std::floor(p.pointerX));
        e.y = static_cast<int>(std::floor(p.pointerY));
        e.dx = p.wheelX;
        e.dy = p.wheelY;
        e.modifiers = p.modifiers();
        p.pointerFocus->emit(e);
    }
    p.wheelX = p.wheelY = 0;
    p.wheelDiscrete = false;
}
const wl_pointer_listener pointerListener = {
    onPointerEnter,
    onPointerLeave,
    onPointerMotion,
    onPointerButton,
    onPointerAxis,
    onPointerFrame,
    [](void*, wl_pointer*, uint32_t) {},            // axis_source
    [](void*, wl_pointer*, uint32_t, uint32_t) {},  // axis_stop
    onPointerAxisDiscrete,
    onPointerAxisValue120,
    [](void*, wl_pointer*, uint32_t, uint32_t) {},  // axis_relative_direction
};

// ---- The keyboard ----

void onKeymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    void* text = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (text == MAP_FAILED) return;
    xkb_keymap* made = xkb.keymap_new_from_string(p.keys, static_cast<const char*>(text), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(text, size);
    if (made == nullptr) return;
    if (p.keyState != nullptr) xkb.state_unref(p.keyState);
    if (p.plainState != nullptr) xkb.state_unref(p.plainState);
    if (p.keymap != nullptr) xkb.keymap_unref(p.keymap);
    p.keymap = made;
    p.keyState = xkb.state_new(made);
    p.plainState = xkb.state_new(made);
}
void onKeyboardEnter(void* data, wl_keyboard*, uint32_t serial, wl_surface* surface, wl_array*) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    p.flushMove();
    p.keyboardFocus = p.windowOf(surface);
    p.inputSerial = serial;
    if (p.keyboardFocus == nullptr) return;
    RawEvent e;
    e.kind = RawEvent::Focus;
    e.on = true;
    p.keyboardFocus->emit(e);
}
void onKeyboardLeave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    p.flushMove();
    p.repeating = 0;
    if (p.keyboardFocus != nullptr) {
        RawEvent e;
        e.kind = RawEvent::Focus;
        e.on = false;
        p.keyboardFocus->emit(e);
    }
    p.keyboardFocus = nullptr;
}
void onKey(void* data, wl_keyboard*, uint32_t serial, uint32_t, uint32_t code, uint32_t state) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) p.inputSerial = serial;
    p.pressKey(code, state == WL_KEYBOARD_KEY_STATE_PRESSED, false);
}
void onModifiers(void* data, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (p.keyState == nullptr) return;
    xkb.state_update_mask(p.keyState, depressed, latched, locked, 0, 0, group);
    xkb.state_update_mask(p.plainState, 0, 0, 0, 0, 0, group);
}
void onRepeatInfo(void* data, wl_keyboard*, int32_t rate, int32_t delay) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    p.repeatRate = rate;
    p.repeatDelay = delay;
}
const wl_keyboard_listener keyboardListener = {onKeymap, onKeyboardEnter, onKeyboardLeave, onKey, onModifiers, onRepeatInfo};

// ---- The clipboard, and files dragged in ----

// Each kind of data an offer can give, as it is told, before the offer is used.
void onOfferType(void* data, wl_data_offer* offer, const char* type) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    for (auto& [o, types] : p.offers)
        if (o == offer) types.emplace_back(type);
}
const wl_data_offer_listener offerListener = {onOfferType, [](void*, wl_data_offer*, uint32_t) {}, [](void*, wl_data_offer*, uint32_t) {}};

void onDataOffer(void* data, wl_data_device*, wl_data_offer* offer) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    p.offers.emplace_back(offer, std::vector<std::string>());
    wl_data_offer_add_listener(offer, &offerListener, &p);
}
// A drag over one of our windows: files (text/uri-list) taken, to be copied; nothing else.
void onDragEnter(void* data, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t x, wl_fixed_t y, wl_data_offer* offer) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    p.flushMove();
    if (p.dropOffer != nullptr && p.dropOffer != p.selectionOffer) p.forget(p.dropOffer);
    p.dropOffer = offer;
    p.dropWindow = p.windowOf(surface);
    const double s = p.dropWindow != nullptr ? p.dropWindow->scale() : 1;
    p.dropX = wl_fixed_to_double(x) * s;
    p.dropY = wl_fixed_to_double(y) * s;
    if (offer == nullptr) return;
    const bool files = p.dropWindow != nullptr && p.hasType(offer, "text/uri-list");
    wl_data_offer_accept(offer, serial, files ? "text/uri-list" : nullptr);
    if (p.dataManagerVersion >= 3)
        wl_data_offer_set_actions(offer, files ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE,
                                  files ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
}
void onDragLeave(void* data, wl_data_device*) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (p.dropOffer != nullptr && p.dropOffer != p.selectionOffer) p.forget(p.dropOffer);
    p.dropOffer = nullptr;
    p.dropWindow = nullptr;
}
void onDragMotion(void* data, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    const double s = p.dropWindow != nullptr ? p.dropWindow->scale() : 1;
    p.dropX = wl_fixed_to_double(x) * s;
    p.dropY = wl_fixed_to_double(y) * s;
}
// %XX in a URI, as the byte it stands for.
std::string unescaped(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}
// Dropped: the files' URIs read (one to a line, file:// ones only, as paths) and given to the
// window as Windows gives its dropped files.
void onDrop(void* data, wl_data_device*) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    wl_data_offer* offer = p.dropOffer;
    LinuxWindow* w = p.dropWindow;
    p.dropOffer = nullptr;
    p.dropWindow = nullptr;
    if (offer == nullptr) return;
    RawEvent e;
    e.kind = RawEvent::FilesDropped;
    e.x = static_cast<int>(std::floor(p.dropX));
    e.y = static_cast<int>(std::floor(p.dropY));
    if (w != nullptr && p.hasType(offer, "text/uri-list")) {
        const std::string list = p.readOffer(offer, "text/uri-list");
        size_t at = 0;
        while (at < list.size()) {
            size_t end = list.find('\n', at);
            if (end == std::string::npos) end = list.size();
            std::string line = list.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("file://", 0) != 0) continue;  // comments (#) and what is not a file
            const size_t path = line.find('/', 7);       // past file:// and any host
            if (path != std::string::npos) e.files.push_back(unescaped(line.substr(path)));
        }
        if (p.dataManagerVersion >= 3) wl_data_offer_finish(offer);
    }
    if (offer != p.selectionOffer) p.forget(offer);
    if (w != nullptr && !e.files.empty()) {
        p.flushMove();
        w->emit(e);
    }
}
// The clipboard now holds another offer (or nothing): the one before let go.
void onSelection(void* data, wl_data_device*, wl_data_offer* offer) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (p.selectionOffer != nullptr && p.selectionOffer != offer && p.selectionOffer != p.dropOffer) p.forget(p.selectionOffer);
    p.selectionOffer = offer;
    if (tracing()) {
        std::string types;
        if (const std::vector<std::string>* t = p.typesOf(offer))
            for (const std::string& type : *t) types += " " + type;
        WW_TRACE("clipboard: %s%s", offer != nullptr ? "an offer of" : "empty", types.c_str());
    }
}
const wl_data_device_listener dataDeviceListener = {onDataOffer, onDragEnter, onDragLeave, onDragMotion, onDrop, onSelection};

// Our text on the clipboard, written to whoever pastes it; let go of when another program sets
// the clipboard.
void writeAll(int fd, const std::string& text) {
    // A reader gone before the end would raise SIGPIPE, which ends the program: held back while
    // writing, and any it raised taken.
    sigset_t pipeOnly, before;
    sigemptyset(&pipeOnly);
    sigaddset(&pipeOnly, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeOnly, &before);
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    sigset_t waiting;
    sigpending(&waiting);
    if (sigismember(&waiting, SIGPIPE)) {
        const timespec none = {0, 0};
        sigtimedwait(&pipeOnly, nullptr, &none);
    }
    pthread_sigmask(SIG_SETMASK, &before, nullptr);
}
void onSourceSend(void* data, wl_data_source*, const char*, int32_t fd) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    writeAll(fd, p.ownText);
    close(fd);
}
void onSourceCancelled(void* data, wl_data_source* source) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    if (p.ownSource == source) p.ownSource = nullptr;
    wl_data_source_destroy(source);
}
const wl_data_source_listener sourceListener = {
    [](void*, wl_data_source*, const char*) {},  // target
    onSourceSend,
    onSourceCancelled,
    [](void*, wl_data_source*) {},            // dnd_drop_performed
    [](void*, wl_data_source*) {},            // dnd_finished
    [](void*, wl_data_source*, uint32_t) {},  // action
};

// ---- A window to the front ----

void onActivationToken(void* data, xdg_activation_token_v1* token, const char* name) {
    auto* request = static_cast<LinuxPlatform::Activation*>(data);
    LinuxPlatform& p = *request->platform;
    if (request->target != nullptr && request->target->surface != nullptr && p.activation != nullptr)
        xdg_activation_v1_activate(p.activation, name, request->target->surface);
    xdg_activation_token_v1_destroy(token);
    p.activations.erase(std::remove(p.activations.begin(), p.activations.end(), request), p.activations.end());
    delete request;
}
const xdg_activation_token_v1_listener activationTokenListener = {onActivationToken};

// A shown window's name for other programs' windows, which the portal's dialogs are told to
// stand in front of.
void onExportHandle(void* data, zxdg_exported_v2*, const char* handle) {
    auto* w = static_cast<LinuxWindow*>(data);
    std::lock_guard<std::mutex> hold(w->platform.handlesMutex);
    w->exportHandle = handle;
}
const zxdg_exported_v2_listener exportedListener = {onExportHandle};

// ---- The pointer held (PointerMode::Locked) ----

void onLocked(void* data, zwp_locked_pointer_v1*) {
    auto* w = static_cast<LinuxWindow*>(data);
    w->lockActive = true;
}
void onUnlocked(void* data, zwp_locked_pointer_v1*) {
    auto* w = static_cast<LinuxWindow*>(data);
    w->lockActive = false;
}
const zwp_locked_pointer_v1_listener lockedListener = {onLocked, onUnlocked};

// While held, the pointer does not move; how far the mouse moved comes here, as a move of the
// window's with the pointer where it was held (as Windows' locked pointer, put back in the middle).
void onRelativeMotion(void* data, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t, wl_fixed_t) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    LinuxWindow* w = p.pointerFocus;
    if (w == nullptr || !w->lockActive) return;
    const double s = w->scale();
    RawEvent e;
    e.kind = RawEvent::PointerMove;
    e.x = static_cast<int>(std::floor(p.pointerX));
    e.y = static_cast<int>(std::floor(p.pointerY));
    e.dx = wl_fixed_to_double(dx) * s;
    e.dy = wl_fixed_to_double(dy) * s;
    e.buttons = w->buttons;
    e.modifiers = p.modifiers();
    p.moved(w, e);
}
const zwp_relative_pointer_v1_listener relativeListener = {onRelativeMotion};

// ---- The seat: its pointer and keyboard as they come and go ----

void onSeatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    const bool hasPointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    const bool hasKeyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (hasPointer && p.pointer == nullptr) {
        p.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(p.pointer, &pointerListener, &p);
        if (p.relativePointers != nullptr) {
            p.relativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer(p.relativePointers, p.pointer);
            zwp_relative_pointer_v1_add_listener(p.relativePointer, &relativeListener, &p);
        }
    } else if (!hasPointer && p.pointer != nullptr) {
        if (p.relativePointer != nullptr) zwp_relative_pointer_v1_destroy(p.relativePointer);
        p.relativePointer = nullptr;
        for (LinuxWindow* w : p.windows) p.lockPointer(*w, false);
        wl_pointer_destroy(p.pointer);
        p.pointer = nullptr;
        p.pointerFocus = nullptr;
    }
    if (hasKeyboard && p.keyboard == nullptr) {
        p.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(p.keyboard, &keyboardListener, &p);
    } else if (!hasKeyboard && p.keyboard != nullptr) {
        wl_keyboard_destroy(p.keyboard);
        p.keyboard = nullptr;
        p.keyboardFocus = nullptr;
    }
}
const wl_seat_listener seatListener = {onSeatCapabilities, [](void*, wl_seat*, const char*) {}};

void onGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& p = *static_cast<LinuxPlatform*>(data);
    const std::string what = interface;
    auto bind = [&](const wl_interface* of, uint32_t most) {
        return wl_registry_bind(registry, name, of, std::min(version, most));
    };
    if (what == "wl_compositor") {
        p.compositorVersion = std::min(version, 6u);
        p.compositor = static_cast<wl_compositor*>(bind(&wl_compositor_interface, 6));
    } else if (what == "wl_shm") {
        p.shm = static_cast<wl_shm*>(bind(&wl_shm_interface, 1));
    } else if (what == "xdg_wm_base") {
        p.shell = static_cast<xdg_wm_base*>(bind(&xdg_wm_base_interface, 5));
        xdg_wm_base_add_listener(p.shell, &shellListener, &p);
    } else if (what == "wl_seat" && p.seat == nullptr) {
        p.seat = static_cast<wl_seat*>(bind(&wl_seat_interface, 8));
        wl_seat_add_listener(p.seat, &seatListener, &p);
    } else if (what == "wl_output") {
        auto* output = static_cast<wl_output*>(bind(&wl_output_interface, 4));
        p.outputs.push_back(LinuxPlatform::Output{output, name, 1});
        wl_output_add_listener(output, &outputListener, &p);
    } else if (what == "zxdg_decoration_manager_v1") {
        p.decorations = static_cast<zxdg_decoration_manager_v1*>(bind(&zxdg_decoration_manager_v1_interface, 1));
    } else if (what == "wp_viewporter" && std::getenv("WW_WAYLAND_NO_VIEWPORT") != nullptr) {
        // (WW_WAYLAND_NO_VIEWPORT: left out, to tell whether a driver's picture is shown wrong through it.)
    } else if (what == "wp_viewporter") {
        p.viewporter = static_cast<wp_viewporter*>(bind(&wp_viewporter_interface, 1));
    } else if (what == "wp_fractional_scale_manager_v1" && std::getenv("WW_WAYLAND_NO_VIEWPORT") != nullptr) {
    } else if (what == "wp_fractional_scale_manager_v1") {
        p.fractionalScales = static_cast<wp_fractional_scale_manager_v1*>(bind(&wp_fractional_scale_manager_v1_interface, 1));
    } else if (what == "wl_data_device_manager") {
        p.dataManagerVersion = std::min(version, 3u);
        p.dataManager = static_cast<wl_data_device_manager*>(bind(&wl_data_device_manager_interface, 3));
    } else if (what == "xdg_activation_v1") {
        p.activation = static_cast<xdg_activation_v1*>(bind(&xdg_activation_v1_interface, 1));
    } else if (what == "zxdg_exporter_v2") {
        p.exporter = static_cast<zxdg_exporter_v2*>(bind(&zxdg_exporter_v2_interface, 1));
    } else if (what == "zwp_pointer_constraints_v1") {
        p.constraints = static_cast<zwp_pointer_constraints_v1*>(bind(&zwp_pointer_constraints_v1_interface, 1));
    } else if (what == "zwp_relative_pointer_manager_v1") {
        p.relativePointers = static_cast<zwp_relative_pointer_manager_v1*>(bind(&zwp_relative_pointer_manager_v1_interface, 1));
    }
}

// ---- The platform's start ----

bool LinuxPlatform::start(std::string& error) {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr && std::getenv("WAYLAND_SOCKET") == nullptr) {
        error = "there is no Wayland display (WAYLAND_DISPLAY is not set): windows need a Wayland session";
        return false;
    }
    if (!loadLibraries(error)) return false;
    display = wl_display_connect(nullptr);
    if (display == nullptr) {
        error = "cannot connect to the Wayland display";
        return false;
    }
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, this);
    wl_display_roundtrip(display);  // the globals
    wl_display_roundtrip(display);  // what each sends as it is bound (the seat's, the outputs')
    if (compositor == nullptr || shell == nullptr) {
        error = "the Wayland compositor offers no windows (xdg-shell)";
        return false;
    }
    wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (dataManager != nullptr && seat != nullptr) {
        dataDevice = wl_data_device_manager_get_data_device(dataManager, seat);
        wl_data_device_add_listener(dataDevice, &dataDeviceListener, this);
    }
    // A launcher's token, to bring the first window shown to the front; not handed on to
    // programs this one starts.
    if (const char* token = std::getenv("XDG_ACTIVATION_TOKEN")) {
        startToken = token;
        unsetenv("XDG_ACTIVATION_TOKEN");
    }

    keys = xkb.context_new(XKB_CONTEXT_NO_FLAGS);
    if (keys == nullptr) {
        error = "cannot start xkbcommon";
        return false;
    }
    const char* locale = std::getenv("LC_ALL");
    if (locale == nullptr || *locale == 0) locale = std::getenv("LC_CTYPE");
    if (locale == nullptr || *locale == 0) locale = std::getenv("LANG");
    if (locale == nullptr || *locale == 0) locale = "C";
    if (xkb_compose_table* table = xkb.compose_table_new_from_locale(keys, locale, XKB_COMPOSE_COMPILE_NO_FLAGS)) {
        compose = xkb.compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
        xkb.compose_table_unref(table);
    }

    // OpenGL 3.3, core, through EGL on the Wayland display: one context for every window, current
    // on whichever is being drawn, and on none (surfaceless) for pictures.
    // Mesa says on standard error what it makes of the machine as EGL starts ("failed to get
    // driver name", "ZINK: failed to choose pdev" under WSLg), which would be the program's own
    // output: held back while it starts (an EGL that does not start is said below, in our words),
    // unless the trace is on or the user asks for EGL's (EGL_LOG_LEVEL).
    const bool quiet = !tracing() && std::getenv("EGL_LOG_LEVEL") == nullptr;
    const int savedError = quiet ? dup(2) : -1;
    if (savedError >= 0) {
        const int nothing = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nothing >= 0) {
            dup2(nothing, 2);
            close(nothing);
        }
    }
    eglDisplay = egl.GetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, nullptr);
    EGLint major = 0, minor = 0;
    const bool initialized = eglDisplay != EGL_NO_DISPLAY && egl.Initialize(eglDisplay, &major, &minor);
    if (savedError >= 0) {
        dup2(savedError, 2);
        close(savedError);
    }
    if (!initialized) {
        error = "cannot start EGL on the Wayland display";
        return false;
    }
    const char* extensions = egl.QueryString(eglDisplay, EGL_EXTENSIONS);
    if (extensions == nullptr || std::strstr(extensions, "EGL_KHR_surfaceless_context") == nullptr) {
        error = "EGL here cannot draw without a window (EGL_KHR_surfaceless_context)";
        return false;
    }
    if (!egl.BindAPI(EGL_OPENGL_API)) {
        error = "EGL here has no OpenGL";
        return false;
    }
    const EGLint wanted[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                             EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLint count = 0;
    if (!egl.ChooseConfig(eglDisplay, wanted, &config, 1, &count) || count < 1) {
        error = "no EGL configuration that OpenGL can draw in";
        return false;
    }
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    context = egl.CreateContext(eglDisplay, config, EGL_NO_CONTEXT, attributes);
    if (context == EGL_NO_CONTEXT || !use(EGL_NO_SURFACE)) {
        error = "OpenGL 3.3 is not available on this machine";
        return false;
    }
    const char* missing = nullptr;
    if (!loadGl([](const char* name) -> void* { return reinterpret_cast<void*>(egl.GetProcAddress(name)); }, missing)) {
        error = std::string("the OpenGL driver lacks ") + missing;
        return false;
    }
    if (tracing()) watch();
    if (tracing()) {
        using GetString = const unsigned char* (*)(unsigned int);
        if (auto getString = reinterpret_cast<GetString>(egl.GetProcAddress("glGetString")))
            WW_TRACE("OpenGL: %s, %s", reinterpret_cast<const char*>(getString(0x1F01)), reinterpret_cast<const char*>(getString(0x1F02)));
        WW_TRACE("protocols: decorations %s, viewporter %s, fractional scale %s, compositor %u", decorations != nullptr ? "yes" : "no",
                 viewporter != nullptr ? "yes" : "no", fractionalScales != nullptr ? "yes" : "no", compositorVersion);
    }
    return true;
}

// ---- Events ----

// Every event waiting handled, on the thread that pumps, with the runtime's lock held (VM's
// event loop, loopPump): those read already and any on the connection now, without waiting; then
// the pointer's moves made one, each window's newest configure done, and a held key repeated.
void LinuxPlatform::pump() {
    at("pump: handling events (the program's handlers)");
    struct Out {
        ~Out() { at("the program (between events)"); }
    } out;
    // Events made for tests first, as the system's own would come.
    if (!posted.empty()) {
        auto events = std::move(posted);
        posted.clear();
        for (auto& [window, event] : events) {
            bool there = false;
            for (LinuxWindow* w : windows) there = there || w == window;
            if (there) window->emit(event);
        }
    }
    while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
    wl_display_flush(display);
    pollfd fd = {wl_display_get_fd(display), POLLIN, 0};
    if (poll(&fd, 1, 0) > 0 && (fd.revents & POLLIN) != 0) wl_display_read_events(display);
    else wl_display_cancel_read(display);
    wl_display_dispatch_pending(display);
    flushMove();
    // Each window's newest configure, once all waiting are read (a copy of the list: drawing
    // may open or close windows).
    const std::vector<LinuxWindow*> now = windows;
    for (LinuxWindow* w : now)
        if (std::find(windows.begin(), windows.end(), w) != windows.end()) w->configure();
    if (repeating != 0 && nowMillis() >= repeatAt) {
        pressKey(repeating - 1, true, true);
        repeatAt += repeatRate > 0 ? std::max(1, 1000 / repeatRate) : 1000;
    }
}

// Waiting only, as Windows' waiting for a message is: for an event to come (read into its
// queue, handled by the next pump), wake(), a held key's next repeat, or `millis` (-1: as long as
// it takes). Nothing is handled here: the runtime waits with its lock let go, and a handler run
// without it left the lock held for good (2026-11-16, KDE: the window stopped answering).
void LinuxPlatform::wait(int64_t millis) {
    at("wait: waiting for events (poll)");
    struct Out {
        ~Out() { at("the program (between events)"); }
    } out;
    if (!posted.empty()) return;
    for (LinuxWindow* w : windows)
        if (w->configurePending) return;
    if (wl_display_prepare_read(display) != 0) return;  // events read already, for the pump
    wl_display_flush(display);
    int64_t limit = millis;
    if (repeating != 0) {
        const int64_t until = std::max<int64_t>(0, repeatAt - nowMillis());
        limit = limit < 0 ? until : std::min(limit, until);
    }
    pollfd fds[2] = {{wl_display_get_fd(display), POLLIN, 0}, {wakeFd, POLLIN, 0}};
    const int ready = poll(fds, 2, limit < 0 ? -1 : static_cast<int>(std::min<int64_t>(limit, 0x7FFFFFFF)));
    if (ready > 0 && (fds[0].revents & POLLIN) != 0) wl_display_read_events(display);
    else wl_display_cancel_read(display);
    if (ready > 0 && (fds[1].revents & POLLIN) != 0) {
        uint64_t count = 0;
        if (read(wakeFd, &count, sizeof count) < 0) {
        }
    }
}

void LinuxPlatform::pressKey(uint32_t code, bool down, bool repeat) {
    flushMove();
    LinuxWindow* w = keyboardFocus;
    if (w == nullptr || keyState == nullptr) return;
    const xkb_keycode_t keycode = code + 8;  // xkb's codes are evdev's plus 8
    RawEvent e;
    e.kind = down ? RawEvent::KeyDown : RawEvent::KeyUp;
    e.key = keyOf(code);
    e.modifiers = modifiers();
    e.repeat = repeat;
    // What the key types plainly in the layout, upper case, where it is a letter or a digit.
    const xkb_keysym_t plain = xkb.state_key_get_one_sym(plainState, keycode);
    const uint32_t typed = xkb.keysym_to_utf32(plain);
    if ((typed >= '0' && typed <= '9') || (typed >= 'A' && typed <= 'Z')) e.letter = typed;
    else if (typed >= 'a' && typed <= 'z') e.letter = typed - 'a' + 'A';
    else if (typed >= 0x80 && xkb.keysym_to_upper != nullptr) {
        const xkb_keysym_t upper = xkb.keysym_to_upper(plain);
        if (upper != plain) e.letter = xkb.keysym_to_utf32(upper);
    }
    w->emit(e);
    if (!down) {
        if (repeating == code + 1) repeating = 0;
        return;
    }
    if (!repeat) {
        if (repeatRate > 0 && xkb.keymap_key_repeats(keymap, keycode)) {
            repeating = code + 1;
            repeatAt = nowMillis() + repeatDelay;
        } else {
            repeating = 0;
        }
    }
    // The text it types, after the layout and any composing (a dead key, then a letter).
    std::string text;
    const xkb_keysym_t sym = xkb.state_key_get_one_sym(keyState, keycode);
    if (compose != nullptr && !repeat) xkb.compose_state_feed(compose, sym);
    const xkb_compose_status status = compose != nullptr ? xkb.compose_state_get_status(compose) : XKB_COMPOSE_NOTHING;
    if (status == XKB_COMPOSE_COMPOSING) return;
    if (status == XKB_COMPOSE_COMPOSED) {
        char buffer[64];
        const int n = xkb.compose_state_get_utf8(compose, buffer, sizeof buffer);
        if (n > 0) text.assign(buffer, static_cast<size_t>(std::min<int>(n, sizeof buffer - 1)));
        xkb.compose_state_reset(compose);
    } else if (status == XKB_COMPOSE_CANCELLED) {
        xkb.compose_state_reset(compose);
        return;
    } else {
        char buffer[64];
        const int n = xkb.state_key_get_utf8(keyState, keycode, buffer, sizeof buffer);
        if (n > 0) text.assign(buffer, static_cast<size_t>(std::min<int>(n, sizeof buffer - 1)));
    }
    // Enter, Backspace, Tab and the like come as keys (as do Control and a letter, which types a control character).
    if (text.empty() || static_cast<unsigned char>(text[0]) < 32 || text == "\x7F") return;
    RawEvent t;
    t.kind = RawEvent::Text;
    t.text = text;
    w->emit(t);
}

void LinuxPlatform::showCursor() {
    LinuxWindow* w = pointerFocus;
    if (w == nullptr || pointer == nullptr) return;
    if (w->pointerMode != PointerMode::Normal) {
        wl_pointer_set_cursor(pointer, pointerSerial, nullptr, 0, 0);
        return;
    }
    if (shm == nullptr) return;
    const int scale = std::max(1, static_cast<int>(std::ceil(w->scale())));
    const int size = 24 * scale;
    if (cursorTheme == nullptr || cursorThemeSize != size) {
        if (cursorTheme != nullptr) wl_cursor_theme_destroy(cursorTheme);
        const char* named = std::getenv("XCURSOR_THEME");
        cursorTheme = wl_cursor_theme_load(named, size, shm);
        cursorThemeSize = size;
    }
    if (cursorTheme == nullptr) return;
    static const char* const names[][2] = {
        {"default", "left_ptr"}, {"ew-resize", "sb_h_double_arrow"}, {"ns-resize", "sb_v_double_arrow"}, {"text", "xterm"}, {"pointer", "hand2"},
        {"move", "fleur"},       {"nwse-resize", "bottom_right_corner"}, {"nesw-resize", "bottom_left_corner"}, {"crosshair", "crosshair"},
    };
    const int index = std::clamp(static_cast<int>(w->cursor), 0, 8);
    wl_cursor* shape = wl_cursor_theme_get_cursor(cursorTheme, names[index][0]);
    if (shape == nullptr) shape = wl_cursor_theme_get_cursor(cursorTheme, names[index][1]);
    if (shape == nullptr) shape = wl_cursor_theme_get_cursor(cursorTheme, "left_ptr");
    if (shape == nullptr || shape->image_count == 0) return;
    wl_cursor_image* image = shape->images[0];
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (buffer == nullptr) return;
    if (cursorSurface == nullptr) cursorSurface = wl_compositor_create_surface(compositor);
    if (compositorVersion >= 3) wl_surface_set_buffer_scale(cursorSurface, scale);
    wl_pointer_set_cursor(pointer, pointerSerial, cursorSurface, static_cast<int32_t>(image->hotspot_x) / scale, static_cast<int32_t>(image->hotspot_y) / scale);
    wl_surface_attach(cursorSurface, buffer, 0, 0);
    wl_surface_damage(cursorSurface, 0, 0, static_cast<int32_t>(image->width), static_cast<int32_t>(image->height));
    wl_surface_commit(cursorSurface);
}

// ---- Windows ----

std::unique_ptr<PlatformWindow> LinuxPlatform::createWindow(const WindowSettings& settings, PlatformWindow* owner, std::string& error) {
    auto window = std::make_unique<LinuxWindow>(*this);
    LinuxWindow& w = *window;
    w.title = settings.title;
    w.dialog = settings.dialog;
    w.resizable = settings.resizable;
    w.fixedScale = settings.scale;
    w.preferred = screenScale();
    w.owner = static_cast<LinuxWindow*>(owner);
    w.surface = wl_compositor_create_surface(compositor);
    if (w.surface == nullptr) {
        error = "cannot create a Wayland surface";
        return nullptr;
    }
    wl_surface_add_listener(w.surface, &surfaceListener, &w);
    if (fractionalScales != nullptr && viewporter != nullptr) {
        w.fractional = wp_fractional_scale_manager_v1_get_fractional_scale(fractionalScales, w.surface);
        wp_fractional_scale_v1_add_listener(w.fractional, &fractionalListener, &w);
        w.viewport = wp_viewporter_get_viewport(viewporter, w.surface);
    }
    // The size asked for is of what can be drawn on, in layout units: the compositor's own.
    w.logicalWidth = std::max(1, settings.width);
    w.logicalHeight = std::max(1, settings.height);
    windows.push_back(&w);
    return window;
}

LinuxWindow::~LinuxWindow() {
    hide();
    LinuxPlatform& p = platform;
    if (p.pointerFocus == this) p.pointerFocus = nullptr;
    if (p.moveWindow == this) p.moveWindow = nullptr;
    if (p.keyboardFocus == this) {
        p.keyboardFocus = nullptr;
        p.repeating = 0;
    }
    for (LinuxWindow* w : p.windows)
        if (w->owner == this) w->owner = nullptr;
    p.posted.erase(std::remove_if(p.posted.begin(), p.posted.end(), [&](const auto& posted) { return posted.first == this; }), p.posted.end());
    if (p.dropWindow == this) p.dropWindow = nullptr;
    for (LinuxPlatform::Activation* request : p.activations)
        if (request->target == this) request->target = nullptr;
    p.lockPointer(*this, false);
    if (fractional != nullptr) wp_fractional_scale_v1_destroy(fractional);
    if (viewport != nullptr) wp_viewport_destroy(viewport);
    if (surface != nullptr) wl_surface_destroy(surface);
    p.windows.erase(std::remove(p.windows.begin(), p.windows.end(), this), p.windows.end());
    wl_display_flush(p.display);
}

// Shown: the surface given xdg-shell's toplevel role, its title and limits, a title bar from the
// compositor where it draws them; then, once the compositor has said its size (configure), the
// EGL window drawn into.
void LinuxWindow::show() {
    if (shown) return;
    LinuxPlatform& p = platform;
    shown = true;
    configured = false;
    shell = xdg_wm_base_get_xdg_surface(p.shell, surface);
    xdg_surface_add_listener(shell, &shellSurfaceListener, this);
    toplevel = xdg_surface_get_toplevel(shell);
    xdg_toplevel_add_listener(toplevel, &toplevelListener, this);
    xdg_toplevel_set_title(toplevel, title.c_str());
    xdg_toplevel_set_app_id(toplevel, "winterwolf");
    if (owner != nullptr && owner->toplevel != nullptr) xdg_toplevel_set_parent(toplevel, owner->toplevel);
    applyLimits();
    if (p.decorations != nullptr) {
        decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(p.decorations, toplevel);
        zxdg_toplevel_decoration_v1_add_listener(decoration, &decorationListener, this);
        zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    if (p.exporter != nullptr) {
        exported = zxdg_exporter_v2_export_toplevel(p.exporter, surface);
        zxdg_exported_v2_add_listener(exported, &exportedListener, this);
    }
    wl_surface_commit(surface);
    // Its first configure waited for, so that the program's first picture has a place to go.
    for (int i = 0; i < 100 && !configurePending && shown; i++) wl_display_roundtrip(p.display);
    configure();
    if (!p.startToken.empty() && p.activation != nullptr) {
        xdg_activation_v1_activate(p.activation, p.startToken.c_str(), surface);
        p.startToken.clear();
        wl_display_flush(p.display);
    }
}

void LinuxWindow::hide() {
    if (!shown) return;
    LinuxPlatform& p = platform;
    shown = false;
    configured = false;
    if (p.currentSurface == eglSurface && eglSurface != EGL_NO_SURFACE) p.use(EGL_NO_SURFACE);
    if (eglSurface != EGL_NO_SURFACE) egl.DestroySurface(p.eglDisplay, eglSurface);
    eglSurface = EGL_NO_SURFACE;
    if (eglWindow != nullptr) wl_egl_window_destroy(eglWindow);
    eglWindow = nullptr;
    if (decoration != nullptr) zxdg_toplevel_decoration_v1_destroy(decoration);
    decoration = nullptr;
    if (exported != nullptr) zxdg_exported_v2_destroy(exported);
    exported = nullptr;
    {
        std::lock_guard<std::mutex> hold(p.handlesMutex);
        exportHandle.clear();
    }
    if (toplevel != nullptr) xdg_toplevel_destroy(toplevel);
    toplevel = nullptr;
    if (shell != nullptr) xdg_surface_destroy(shell);
    shell = nullptr;
    wl_surface_attach(surface, nullptr, 0, 0);
    wl_surface_commit(surface);
    wl_display_flush(p.display);
}

void LinuxWindow::sized() {
    const int w = width(), h = height();
    WW_TRACE("sized: logical %d x %d, scale %.4f, device %d x %d, configured %d, egl window %s, viewport %s", logicalWidth, logicalHeight, scale(), w, h, configured ? 1 : 0,
             eglWindow != nullptr ? "resized" : "made", viewport != nullptr ? "set" : "none");
    if (configured) {
        if (eglWindow == nullptr) {
            eglWindow = wl_egl_window_create(surface, w, h);
            if (eglWindow != nullptr) eglSurface = platform.windowSurface(eglWindow);
        } else {
            wl_egl_window_resize(eglWindow, w, h, 0, 0);
        }
        // The picture drawn at the scale, shown at the window's size: by the viewport where the
        // scale is fractional, else by the buffer's own (whole) scale.
        if (viewport != nullptr) {
            wp_viewport_set_destination(viewport, logicalWidth, logicalHeight);
        } else if (platform.compositorVersion >= 3) {
            wl_surface_set_buffer_scale(surface, std::max(1, static_cast<int>(std::lround(scale()))));
        }
    }
    RawEvent e;
    e.kind = RawEvent::Resize;
    e.width = w;
    e.height = h;
    emit(e);
}

bool LinuxWindow::focused() const { return platform.keyboardFocus == this; }

void LinuxWindow::setCursor(Cursor shape) {
    cursor = shape;
    if (platform.pointerFocus == this) platform.showCursor();
}
void LinuxWindow::setPointerMode(PointerMode mode) {
    pointerMode = mode;
    platform.lockPointer(*this, mode == PointerMode::Locked);
    if (platform.pointerFocus == this) platform.showCursor();
}

void LinuxWindow::toFront() { platform.activate(*this, true); }
void LinuxWindow::requestAttention() { platform.activate(*this, false); }

// Held where it is, for as long as the mode is Locked: given back when the window loses the pointer
// and held again as it comes back (the lock lasts), the moves then coming as distances.
void LinuxPlatform::lockPointer(LinuxWindow& w, bool on) {
    if (on && w.lock == nullptr && constraints != nullptr && pointer != nullptr && w.surface != nullptr) {
        w.lock = zwp_pointer_constraints_v1_lock_pointer(constraints, w.surface, pointer, nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(w.lock, &lockedListener, &w);
        if (pointerFocus == &w) {
            const double s = w.scale();
            zwp_locked_pointer_v1_set_cursor_position_hint(w.lock, wl_fixed_from_double(pointerX / s), wl_fixed_from_double(pointerY / s));
        }
        wl_surface_commit(w.surface);
    } else if (!on && w.lock != nullptr) {
        zwp_locked_pointer_v1_destroy(w.lock);
        w.lock = nullptr;
        w.lockActive = false;
    }
    wl_display_flush(display);
}

void LinuxPlatform::activate(LinuxWindow& target, bool asked) {
    if (activation == nullptr || target.surface == nullptr || !target.shown) return;
    auto* request = new Activation{this, &target};
    activations.push_back(request);
    xdg_activation_token_v1* token = xdg_activation_v1_get_activation_token(activation);
    xdg_activation_token_v1_add_listener(token, &activationTokenListener, request);
    if (asked && inputSerial != 0 && seat != nullptr) xdg_activation_token_v1_set_serial(token, inputSerial, seat);
    LinuxWindow* from = keyboardFocus != nullptr ? keyboardFocus : &target;
    xdg_activation_token_v1_set_surface(token, from->surface);
    xdg_activation_token_v1_set_app_id(token, "winterwolf");
    xdg_activation_token_v1_commit(token);
    wl_display_flush(display);
}

// ---- The clipboard ----

// Ours where we set it (Wayland would ask us for it, and we would wait on ourselves), else the
// offer's, as UTF-8 where it gives that.
std::string LinuxPlatform::clipboardText() {
    WW_TRACE("clipboard asked for: %s", ownSource != nullptr ? "ours" : (selectionOffer == nullptr ? "none" : (typesOf(selectionOffer) == nullptr ? "an offer not known" : "an offer")));
    if (ownSource != nullptr) return ownText;
    if (selectionOffer == nullptr) return std::string();
    for (const char* type : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"})
        if (hasType(selectionOffer, type)) return readOffer(selectionOffer, type);
    return std::string();
}

// Set as a source of text in every name for it that programs ask by, with the serial of the last
// input: the compositor takes it from a program the user is using (Control+C), not otherwise.
void LinuxPlatform::setClipboardText(const std::string& text) {
    if (dataManager == nullptr || dataDevice == nullptr) return;
    ownText = text;
    wl_data_source* source = wl_data_device_manager_create_data_source(dataManager);
    wl_data_source_add_listener(source, &sourceListener, this);
    for (const char* type : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"}) wl_data_source_offer(source, type);
    wl_data_device_set_selection(dataDevice, source, inputSerial);
    if (ownSource != nullptr) wl_data_source_destroy(ownSource);
    ownSource = source;
    wl_display_flush(display);
}

std::string LinuxPlatform::readOffer(wl_data_offer* offer, const char* type) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return std::string();
    wl_data_offer_receive(offer, type, fds[1]);
    close(fds[1]);
    wl_display_flush(display);
    std::string text;
    char buffer[4096];
    const int64_t until = nowMillis() + 2000;
    for (;;) {
        const int64_t left = until - nowMillis();
        if (left <= 0) break;
        pollfd fd = {fds[0], POLLIN, 0};
        const int ready = poll(&fd, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        const ssize_t n = read(fds[0], buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        text.append(buffer, static_cast<size_t>(n));
    }
    close(fds[0]);
    WW_TRACE("read %zu bytes of %s", text.size(), type);
    return text;
}

void LinuxWindow::postForTest(const std::string& what) {
    if (what != "close") return;  // no loop of the system's own while a window moves: nothing to say
    RawEvent e;
    e.kind = RawEvent::Close;
    platform.posted.emplace_back(this, e);
    platform.wake();
}

bool LinuxWindow::makeCurrent() { return platform.use(shown && eglSurface != EGL_NO_SURFACE ? eglSurface : EGL_NO_SURFACE); }

// The picture shown, at once. Not paced by the compositor's frame callbacks: the toolkit paces its
// animations by its own timer, and in a slow virtual machine KDE's callbacks came later than 50
// ms, so that waiting for each added that to every picture (2026-11-16).
void LinuxWindow::swapBuffers() {
    LinuxPlatform& p = platform;
    if (!shown || eglSurface == EGL_NO_SURFACE) return;
    if (!p.use(eglSurface)) {
        WW_TRACE("swap: cannot make the surface current (EGL error 0x%x)", egl.GetError());
        return;
    }
    if (tracing()) {
        EGLint ew = 0, eh = 0;
        egl.QuerySurface(p.eglDisplay, eglSurface, EGL_WIDTH, &ew);
        egl.QuerySurface(p.eglDisplay, eglSurface, EGL_HEIGHT, &eh);
        static int lastW = -1, lastH = -1;
        if (ew != lastW || eh != lastH) WW_TRACE("swap: EGL surface %d x %d, window %d x %d", ew, eh, width(), height());
        lastW = ew;
        lastH = eh;
    }
    at("eglSwapBuffers (Mesa)");
    // WW_WAYLAND_READBACK=<path>: the picture about to be shown, read back from the window's own
    // buffer (once a size), saved raw as <path>_<w>x<h>.rgba: whether a driver drew it wrong or the
    // compositor shows it wrong (2026-11-16, VirtualBox's SVGA3D).
    if (const char* readback = std::getenv("WW_WAYLAND_READBACK")) {
        static int lastW = -1, lastH = -1;
        const int w = width(), h = height();
        using ReadPixels = void (*)(int, int, int, int, unsigned int, unsigned int, void*);
        if ((w != lastW || h != lastH) && w > 0 && h > 0) {
            if (auto readPixels = reinterpret_cast<ReadPixels>(egl.GetProcAddress("glReadPixels"))) {
                std::vector<uint8_t> rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
                readPixels(0, 0, w, h, 0x1908, 0x1401, rgba.data());  // GL_RGBA, GL_UNSIGNED_BYTE: bottom row first
                const std::string path = std::string(readback) + "_" + std::to_string(w) + "x" + std::to_string(h) + ".rgba";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    for (int row = h - 1; row >= 0; row--) std::fwrite(rgba.data() + static_cast<size_t>(row) * static_cast<size_t>(w) * 4, 1, static_cast<size_t>(w) * 4, f);
                    std::fclose(f);
                    WW_TRACE("read back %s", path.c_str());
                }
            }
            lastW = w;
            lastH = h;
        }
    }
    const int64_t before = nowMillis();
    if (!egl.SwapBuffers(p.eglDisplay, eglSurface)) WW_TRACE("swap failed: EGL error 0x%x", egl.GetError());
    if (const int64_t took = nowMillis() - before; took > 20) WW_TRACE("swap: eglSwapBuffers took %lld ms", static_cast<long long>(took));
    at("the program (after a swap)");
}

}  // namespace

Platform* Platform::get(std::string& error) {
    static std::string failure;
    static bool tried = false;
    if (!tried) {
        tried = true;
        auto* platform = new LinuxPlatform();  // lives as long as the program
        if (platform->start(failure)) thePlatform = platform;
    }
    if (thePlatform == nullptr) error = failure;
    return thePlatform;
}

}  // namespace ww::jade

// ---- The protocols' descriptions, made from their XML as ww is built ----
#include "core-protocol.c"
#include "fractional-scale-protocol.c"
#include "viewporter-protocol.c"
#include "xdg-decoration-protocol.c"
#include "pointer-constraints-protocol.c"
#include "relative-pointer-protocol.c"
#include "xdg-activation-protocol.c"
#include "xdg-foreign-protocol.c"
#include "xdg-shell-protocol.c"

#endif

