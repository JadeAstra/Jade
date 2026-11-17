// Jade's native layer on Linux: the desktop portal over D-Bus; see portal.h.

#include "portal.h"

#if defined(__linux__)
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#endif

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace ww::jade::portal {

#if defined(__linux__)

namespace {

// ---- libdbus, as far as it is used ----
// A connection and a message only through pointers; an error and an iterator laid out by the
// caller, as libdbus's headers lay them out (the iterator given more room than it takes), so
// that no header of libdbus is needed to build ww.

struct DBusConnection;
struct DBusMessage;
struct DBusError {
    const char* name;
    const char* message;
    unsigned int bits;
    void* padding;
};
struct DBusMessageIter {
    alignas(8) unsigned char room[128];
};
using dbool = uint32_t;
const int kSessionBus = 0;
const int kTypeByte = 'y', kTypeBoolean = 'b', kTypeUint32 = 'u', kTypeDouble = 'd', kTypeString = 's', kTypeObjectPath = 'o',
          kTypeArray = 'a', kTypeVariant = 'v', kTypeStruct = 'r', kTypeDictEntry = 'e';

struct Dbus {
    dbool (*threads_init_default)() = nullptr;
    void (*error_init)(DBusError*) = nullptr;
    void (*error_free)(DBusError*) = nullptr;
    dbool (*error_is_set)(const DBusError*) = nullptr;
    DBusConnection* (*bus_get_private)(int, DBusError*) = nullptr;
    const char* (*bus_get_unique_name)(DBusConnection*) = nullptr;
    void (*bus_add_match)(DBusConnection*, const char*, DBusError*) = nullptr;
    void (*connection_set_exit_on_disconnect)(DBusConnection*, dbool) = nullptr;
    void (*connection_close)(DBusConnection*) = nullptr;
    void (*connection_unref)(DBusConnection*) = nullptr;
    DBusMessage* (*connection_send_with_reply_and_block)(DBusConnection*, DBusMessage*, int, DBusError*) = nullptr;
    dbool (*connection_read_write)(DBusConnection*, int) = nullptr;
    DBusMessage* (*connection_pop_message)(DBusConnection*) = nullptr;
    DBusMessage* (*message_new_method_call)(const char*, const char*, const char*, const char*) = nullptr;
    void (*message_unref)(DBusMessage*) = nullptr;
    dbool (*message_is_signal)(DBusMessage*, const char*, const char*) = nullptr;
    const char* (*message_get_path)(DBusMessage*) = nullptr;
    void (*message_iter_init_append)(DBusMessage*, DBusMessageIter*) = nullptr;
    dbool (*message_iter_append_basic)(DBusMessageIter*, int, const void*) = nullptr;
    dbool (*message_iter_append_fixed_array)(DBusMessageIter*, int, const void*, int) = nullptr;
    dbool (*message_iter_open_container)(DBusMessageIter*, int, const char*, DBusMessageIter*) = nullptr;
    dbool (*message_iter_close_container)(DBusMessageIter*, DBusMessageIter*) = nullptr;
    dbool (*message_iter_init)(DBusMessage*, DBusMessageIter*) = nullptr;
    int (*message_iter_get_arg_type)(DBusMessageIter*) = nullptr;
    void (*message_iter_recurse)(DBusMessageIter*, DBusMessageIter*) = nullptr;
    void (*message_iter_get_basic)(DBusMessageIter*, void*) = nullptr;
    dbool (*message_iter_next)(DBusMessageIter*) = nullptr;
};

// libdbus, opened the first time it is wanted; null where it is not installed.
const Dbus* dbus() {
    static const Dbus* const loaded = []() -> const Dbus* {
        void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) return nullptr;
        static Dbus d;
        bool ok = true;
#define WW_DBUS(name) ok = ((d.name = reinterpret_cast<decltype(d.name)>(dlsym(lib, "dbus_" #name))) != nullptr) && ok
        WW_DBUS(threads_init_default);
        WW_DBUS(error_init);
        WW_DBUS(error_free);
        WW_DBUS(error_is_set);
        WW_DBUS(bus_get_private);
        WW_DBUS(bus_get_unique_name);
        WW_DBUS(bus_add_match);
        WW_DBUS(connection_set_exit_on_disconnect);
        WW_DBUS(connection_close);
        WW_DBUS(connection_unref);
        WW_DBUS(connection_send_with_reply_and_block);
        WW_DBUS(connection_read_write);
        WW_DBUS(connection_pop_message);
        WW_DBUS(message_new_method_call);
        WW_DBUS(message_unref);
        WW_DBUS(message_is_signal);
        WW_DBUS(message_get_path);
        WW_DBUS(message_iter_init_append);
        WW_DBUS(message_iter_append_basic);
        WW_DBUS(message_iter_append_fixed_array);
        WW_DBUS(message_iter_open_container);
        WW_DBUS(message_iter_close_container);
        WW_DBUS(message_iter_init);
        WW_DBUS(message_iter_get_arg_type);
        WW_DBUS(message_iter_recurse);
        WW_DBUS(message_iter_get_basic);
        WW_DBUS(message_iter_next);
#undef WW_DBUS
        if (!ok) return nullptr;
        d.threads_init_default();  // connections are used from more than one thread
        return &d;
    }();
    return loaded;
}

// An error of libdbus's, freed when done with.
struct Error {
    const Dbus& d;
    DBusError e;
    explicit Error(const Dbus& lib) : d(lib) { d.error_init(&e); }
    ~Error() { d.error_free(&e); }
    bool set() const { return d.error_is_set(&e) != 0; }
    std::string text() const { return std::string(e.name != nullptr ? e.name : "") + (e.message != nullptr ? std::string(": ") + e.message : ""); }
};

// A message, let go of when done with.
struct Message {
    const Dbus& d;
    DBusMessage* m;
    Message(const Dbus& lib, DBusMessage* message) : d(lib), m(message) {}
    ~Message() {
        if (m != nullptr) d.message_unref(m);
    }
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
};

// A connection of our own to the session's bus, closed when done with (one that is not shared:
// libdbus's shared one is the program's, and must not be closed).
struct Bus {
    const Dbus& d;
    DBusConnection* c = nullptr;
    Bus(const Dbus& lib, std::string& error) : d(lib) {
        Error e(d);
        c = d.bus_get_private(kSessionBus, &e.e);
        if (c == nullptr) {
            error = "cannot reach the desktop's session bus (D-Bus)" + (e.set() ? " (" + e.text() + ")" : std::string());
            return;
        }
        d.connection_set_exit_on_disconnect(c, 0);
    }
    ~Bus() {
        if (c != nullptr) {
            d.connection_close(c);
            d.connection_unref(c);
        }
    }
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;
};

const char* const kDesktop = "org.freedesktop.portal.Desktop";
const char* const kDesktopPath = "/org/freedesktop/portal/desktop";

// ---- Settings ----

// The connection settings are read on, kept: a program may ask on each frame.
std::mutex settingsMutex;
Bus* settingsBus = nullptr;

// One of the portal's settings, read with ReadOne, or with Read where the portal is older (its
// value then in a variant of its own); `take` handed the iterator at the value. False where it
// cannot be read.
template <typename Take>
bool readSetting(const char* group, const char* key, Take take) {
    const Dbus* d = dbus();
    if (d == nullptr) return false;
    std::lock_guard<std::mutex> hold(settingsMutex);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (settingsBus == nullptr) {
            std::string error;
            settingsBus = new Bus(*d, error);
            if (settingsBus->c == nullptr) {
                delete settingsBus;
                settingsBus = nullptr;
                return false;
            }
        }
        for (const char* method : {"ReadOne", "Read"}) {
            Message call(*d, d->message_new_method_call(kDesktop, kDesktopPath, "org.freedesktop.portal.Settings", method));
            if (call.m == nullptr) return false;
            DBusMessageIter args;
            d->message_iter_init_append(call.m, &args);
            d->message_iter_append_basic(&args, kTypeString, &group);
            d->message_iter_append_basic(&args, kTypeString, &key);
            Error e(*d);
            Message reply(*d, d->connection_send_with_reply_and_block(settingsBus->c, call.m, 1000, &e.e));
            if (reply.m == nullptr) {
                const std::string name = e.set() && e.e.name != nullptr ? e.e.name : "";
                if (name == "org.freedesktop.DBus.Error.UnknownMethod") continue;  // a portal older than ReadOne
                if (name == "org.freedesktop.DBus.Error.Disconnected" || name == "org.freedesktop.DBus.Error.NoReply") break;
                return false;  // no such setting, no portal
            }
            DBusMessageIter value;
            if (!d->message_iter_init(reply.m, &value)) return false;
            while (d->message_iter_get_arg_type(&value) == kTypeVariant) {
                DBusMessageIter inside;
                d->message_iter_recurse(&value, &inside);
                value = inside;
            }
            return take(*d, value);
        }
        // The connection gone (the bus restarted): made again, once.
        delete settingsBus;
        settingsBus = nullptr;
    }
    return false;
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

// ---- The options of a portal's call (a{sv}) ----

struct Options {
    const Dbus& d;
    DBusMessageIter dict;
    Options(const Dbus& lib, DBusMessageIter& args) : d(lib) { d.message_iter_open_container(&args, kTypeArray, "{sv}", &dict); }
    void close(DBusMessageIter& args) { d.message_iter_close_container(&args, &dict); }
    // One entry, its value written by `write` into a variant of `signature`.
    template <typename Write>
    void add(const char* key, const char* signature, Write write) {
        DBusMessageIter entry, variant;
        d.message_iter_open_container(&dict, kTypeDictEntry, nullptr, &entry);
        d.message_iter_append_basic(&entry, kTypeString, &key);
        d.message_iter_open_container(&entry, kTypeVariant, signature, &variant);
        write(variant);
        d.message_iter_close_container(&entry, &variant);
        d.message_iter_close_container(&dict, &entry);
    }
    void addString(const char* key, const std::string& value) {
        add(key, "s", [&](DBusMessageIter& v) {
            const char* text = value.c_str();
            d.message_iter_append_basic(&v, kTypeString, &text);
        });
    }
    void addBool(const char* key, bool value) {
        add(key, "b", [&](DBusMessageIter& v) {
            const dbool on = value ? 1 : 0;
            d.message_iter_append_basic(&v, kTypeBoolean, &on);
        });
    }
    // A path as the portal takes one: its bytes, ending in 0.
    void addPath(const char* key, const std::string& path) {
        add(key, "ay", [&](DBusMessageIter& v) {
            DBusMessageIter bytes;
            d.message_iter_open_container(&v, kTypeArray, "y", &bytes);
            const unsigned char* start = reinterpret_cast<const unsigned char*>(path.c_str());
            d.message_iter_append_fixed_array(&bytes, kTypeByte, &start, static_cast<int>(path.size() + 1));
            d.message_iter_close_container(&v, &bytes);
        });
    }
};

std::atomic<unsigned> requests{0};

}  // namespace

int colorScheme() {
    uint32_t scheme = 0;
    const bool read = readSetting("org.freedesktop.appearance", "color-scheme", [&](const Dbus& d, DBusMessageIter& value) {
        if (d.message_iter_get_arg_type(&value) != kTypeUint32) return false;
        d.message_iter_get_basic(&value, &scheme);
        return true;
    });
    return read && scheme <= 2 ? static_cast<int>(scheme) : -1;
}

bool accentColor(uint32_t& rgb) {
    return readSetting("org.freedesktop.appearance", "accent-color", [&](const Dbus& d, DBusMessageIter& value) {
        if (d.message_iter_get_arg_type(&value) != kTypeStruct) return false;
        DBusMessageIter part;
        d.message_iter_recurse(&value, &part);
        uint32_t made = 0;
        for (int i = 0; i < 3; i++) {
            if (d.message_iter_get_arg_type(&part) != kTypeDouble) return false;
            double c = 0;
            d.message_iter_get_basic(&part, &c);
            if (!(c >= 0 && c <= 1)) return false;  // out of 0 to 1: none set (the portal's way of saying so)
            made = (made << 8) | static_cast<uint32_t>(c * 255 + 0.5);
            d.message_iter_next(&part);
        }
        rgb = made;
        return true;
    });
}

void watchAppearance(std::function<void()> changed) {
    const Dbus* d = dbus();
    if (d == nullptr) return;
    std::thread([d, changed] {
        std::string error;
        Bus bus(*d, error);
        if (bus.c == nullptr) return;
        Error e(*d);
        d->bus_add_match(bus.c, "type='signal',interface='org.freedesktop.portal.Settings',member='SettingChanged',arg0='org.freedesktop.appearance'", &e.e);
        if (e.set()) return;
        for (;;) {
            DBusMessage* next = d->connection_pop_message(bus.c);
            if (next == nullptr) {
                if (!d->connection_read_write(bus.c, -1)) return;  // the bus closed
                continue;
            }
            Message signal(*d, next);
            if (!d->message_is_signal(signal.m, "org.freedesktop.portal.Settings", "SettingChanged")) continue;
            DBusMessageIter it;
            if (!d->message_iter_init(signal.m, &it) || d->message_iter_get_arg_type(&it) != kTypeString) continue;
            const char* group = nullptr;
            d->message_iter_get_basic(&it, &group);
            if (group != nullptr && std::strcmp(group, "org.freedesktop.appearance") == 0) changed();
        }
    }).detach();
}

bool textSetting(const std::string& group, const std::string& key, std::string& value) {
    return readSetting(group.c_str(), key.c_str(), [&](const Dbus& d, DBusMessageIter& v) {
        if (d.message_iter_get_arg_type(&v) != kTypeString) return false;
        const char* text = nullptr;
        d.message_iter_get_basic(&v, &text);
        value = text != nullptr ? text : "";
        return true;
    });
}

bool chooseFile(int kind, const std::string& title, const std::vector<std::pair<std::string, std::string>>& filters, const std::string& startDir,
                const std::string& name, const std::string& parent, std::string& path, std::string& error) {
    const Dbus* d = dbus();
    if (d == nullptr) {
        error = "no file dialog: libdbus-1, which the desktop's dialogs are reached through, is not installed";
        return false;
    }
    Bus bus(*d, error);
    if (bus.c == nullptr) return false;
    // Where the portal will answer, worked out before asking, so that an answer cannot come
    // before it is listened for: its request path, from our name on the bus and a token.
    std::string sender = d->bus_get_unique_name(bus.c) != nullptr ? d->bus_get_unique_name(bus.c) : "";
    if (!sender.empty() && sender[0] == ':') sender.erase(0, 1);
    for (char& c : sender)
        if (c == '.') c = '_';
    const std::string token = "ww" + std::to_string(getpid()) + "_" + std::to_string(++requests);
    const std::string expected = std::string(kDesktopPath) + "/request/" + sender + "/" + token;
    auto listen = [&](const std::string& at) {
        Error e(*d);
        const std::string rule = "type='signal',sender='" + std::string(kDesktop) + "',interface='org.freedesktop.portal.Request',member='Response',path='" + at + "'";
        d->bus_add_match(bus.c, rule.c_str(), &e.e);
    };
    listen(expected);

    Message call(*d, d->message_new_method_call(kDesktop, kDesktopPath, "org.freedesktop.portal.FileChooser", kind == 1 ? "SaveFile" : "OpenFile"));
    if (call.m == nullptr) {
        error = "cannot ask for the desktop's file dialog";
        return false;
    }
    DBusMessageIter args;
    d->message_iter_init_append(call.m, &args);
    const char* parentText = parent.c_str();
    const char* titleText = title.c_str();
    d->message_iter_append_basic(&args, kTypeString, &parentText);
    d->message_iter_append_basic(&args, kTypeString, &titleText);
    Options options(*d, args);
    options.addString("handle_token", token);
    options.addBool("modal", true);
    if (kind == 2) options.addBool("directory", true);
    if (kind != 2 && !filters.empty()) {
        // a(sa(us)): each filter its name and its patterns ("*.txt;*.md"), 0 for a pattern.
        options.add("filters", "a(sa(us))", [&](DBusMessageIter& v) {
            DBusMessageIter list;
            d->message_iter_open_container(&v, kTypeArray, "(sa(us))", &list);
            for (const auto& [what, patterns] : filters) {
                DBusMessageIter filter, globs;
                d->message_iter_open_container(&list, kTypeStruct, nullptr, &filter);
                const char* label = what.c_str();
                d->message_iter_append_basic(&filter, kTypeString, &label);
                d->message_iter_open_container(&filter, kTypeArray, "(us)", &globs);
                size_t at = 0;
                while (at <= patterns.size()) {
                    size_t end = patterns.find(';', at);
                    if (end == std::string::npos) end = patterns.size();
                    std::string glob = patterns.substr(at, end - at);
                    while (!glob.empty() && glob.front() == ' ') glob.erase(0, 1);
                    while (!glob.empty() && glob.back() == ' ') glob.pop_back();
                    if (glob == "*.*") glob = "*";
                    if (!glob.empty()) {
                        DBusMessageIter one;
                        d->message_iter_open_container(&globs, kTypeStruct, nullptr, &one);
                        const uint32_t isGlob = 0;
                        const char* text = glob.c_str();
                        d->message_iter_append_basic(&one, kTypeUint32, &isGlob);
                        d->message_iter_append_basic(&one, kTypeString, &text);
                        d->message_iter_close_container(&globs, &one);
                    }
                    at = end + 1;
                }
                d->message_iter_close_container(&filter, &globs);
                d->message_iter_close_container(&list, &filter);
            }
            d->message_iter_close_container(&v, &list);
        });
    }
    if (!startDir.empty()) options.addPath("current_folder", startDir);
    if (kind == 1 && !name.empty()) options.addString("current_name", name);
    options.close(args);

    std::string handle;
    {
        Error e(*d);
        Message reply(*d, d->connection_send_with_reply_and_block(bus.c, call.m, 10000, &e.e));
        if (reply.m == nullptr) {
            const std::string why = e.set() ? e.text() : std::string("no answer");
            error = why.find("ServiceUnknown") != std::string::npos || why.find("UnknownMethod") != std::string::npos
                        ? "no file dialog: the desktop has no portal for it (xdg-desktop-portal)"
                        : "the desktop's file dialog could not be shown (" + why + ")";
            return false;
        }
        DBusMessageIter result;
        if (d->message_iter_init(reply.m, &result) && d->message_iter_get_arg_type(&result) == kTypeObjectPath) {
            const char* at = nullptr;
            d->message_iter_get_basic(&result, &at);
            if (at != nullptr) handle = at;
        }
    }
    if (!handle.empty() && handle != expected) listen(handle);  // a portal older than handle_token

    // Waited for, as long as the user takes: the request's Response, its code (0 chosen, 1
    // cancelled, 2 otherwise ended) and its results, the URIs chosen among them.
    for (;;) {
        DBusMessage* next = d->connection_pop_message(bus.c);
        if (next == nullptr) {
            if (!d->connection_read_write(bus.c, -1)) {
                error = "the desktop's file dialog went away (the session bus closed)";
                return false;
            }
            continue;
        }
        Message signal(*d, next);
        if (!d->message_is_signal(signal.m, "org.freedesktop.portal.Request", "Response")) continue;
        const char* at = d->message_get_path(signal.m);
        if (at == nullptr || (expected != at && handle != at)) continue;
        DBusMessageIter it;
        if (!d->message_iter_init(signal.m, &it) || d->message_iter_get_arg_type(&it) != kTypeUint32) {
            error = "the desktop's file dialog answered in a way not understood";
            return false;
        }
        uint32_t code = 2;
        d->message_iter_get_basic(&it, &code);
        if (code == 1) return false;  // cancelled
        if (code != 0) {
            error = "the desktop's file dialog ended without an answer";
            return false;
        }
        std::string uri;
        if (d->message_iter_next(&it) && d->message_iter_get_arg_type(&it) == kTypeArray) {
            DBusMessageIter entries;
            d->message_iter_recurse(&it, &entries);
            while (d->message_iter_get_arg_type(&entries) == kTypeDictEntry) {
                DBusMessageIter entry;
                d->message_iter_recurse(&entries, &entry);
                const char* key = nullptr;
                if (d->message_iter_get_arg_type(&entry) == kTypeString) d->message_iter_get_basic(&entry, &key);
                if (key != nullptr && std::strcmp(key, "uris") == 0 && d->message_iter_next(&entry) && d->message_iter_get_arg_type(&entry) == kTypeVariant) {
                    DBusMessageIter variant, uris;
                    d->message_iter_recurse(&entry, &variant);
                    if (d->message_iter_get_arg_type(&variant) == kTypeArray) {
                        d->message_iter_recurse(&variant, &uris);
                        if (d->message_iter_get_arg_type(&uris) == kTypeString) {
                            const char* first = nullptr;
                            d->message_iter_get_basic(&uris, &first);
                            if (first != nullptr) uri = first;
                        }
                    }
                }
                d->message_iter_next(&entries);
            }
        }
        if (uri.rfind("file://", 0) != 0) {
            error = uri.empty() ? "the desktop's file dialog chose nothing" : "the desktop's file dialog chose " + uri + ", which is not a file";
            return false;
        }
        const size_t slash = uri.find('/', 7);  // past file:// and any host
        path = unescaped(slash != std::string::npos ? uri.substr(slash) : std::string("/"));
        return true;
    }
}

#else

int colorScheme() { return -1; }
bool accentColor(uint32_t&) { return false; }
bool textSetting(const std::string&, const std::string&, std::string&) { return false; }
void watchAppearance(std::function<void()>) {}
bool chooseFile(int, const std::string&, const std::vector<std::pair<std::string, std::string>>&, const std::string&, const std::string&, const std::string&,
                std::string&, std::string& error) {
    error = "the desktop portal is Linux's";
    return false;
}

#endif

}  // namespace ww::jade::portal
