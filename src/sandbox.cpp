// A sandbox's host (M6.6.2): files kept in memory, faults that make their rare failures happen,
// and nothing of the system reached. What `Sandbox` in the standard library (sandbox.jade) is
// built on; see the M6.6.2 plan, in docs/plans-and-gaps.html.
#include "sandbox.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>

#include "builtins.h"
#include "vm.h"

namespace jade {

namespace {

int64_t nowNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Errors with a message a fault gives ("the device is not ready"): each message kept once, its
// number the error's value.
class FaultCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "sandbox"; }
    std::string message(int value) const override {
        std::lock_guard<std::mutex> hold(mutex_);
        // Numbered from 1: an error numbered 0 is none.
        return value >= 1 && static_cast<size_t>(value) <= messages_.size() ? messages_[static_cast<size_t>(value) - 1] : "a fault";
    }
    std::error_code make(const std::string& text) {
        std::lock_guard<std::mutex> hold(mutex_);
        for (size_t i = 0; i < messages_.size(); i++)
            if (messages_[i] == text) return std::error_code(static_cast<int>(i + 1), *this);
        messages_.push_back(text);
        return std::error_code(static_cast<int>(messages_.size()), *this);
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

FaultCategory& faults() {
    static FaultCategory category;
    return category;
}

std::error_code errc(std::errc e) { return std::make_error_code(e); }

struct Node {
    bool directory = false;
    std::string name;  // as it was written; the key it is kept by may differ in case
    std::string data;
    int64_t modified = 0;
    bool writable = true;
    std::map<std::string, std::unique_ptr<Node>> children;
};

class VirtualHost;

// A file of the sandbox, opened as a stream: read from where it was read to, or written at its
// end. The file is looked up by its path at each step, as the system's would be by its handle.
class FileChannel final : public Channel {
public:
    FileChannel(std::shared_ptr<VirtualHost> host, std::string path, bool reading) : host_(std::move(host)), path_(std::move(path)), reading_(reading) {}
    int read(char* out, size_t max, std::string& error, bool& timedOut) override;
    bool write(const char* data, size_t size, std::string& error) override;
    void close() override { closed_ = true; }

private:
    std::shared_ptr<VirtualHost> host_;
    std::string path_;
    bool reading_;
    size_t position_ = 0;  // read so far; for writing, written so far
    bool closed_ = false;
};

class VirtualHost final : public SandboxHost, public std::enable_shared_from_this<VirtualHost> {
public:
    VirtualHost(bool windows, int64_t diskSize) : windows_(windows), diskSize_(diskSize) {
        root_.directory = true;
        root_.modified = nowNanoseconds();
        std::error_code ec;
        makeDirectory(windows_ ? "C:\\Temp" : "/tmp", true, ec);
    }

    // ---- Paths ----

    bool isSeparator(char c) const { return c == '/' || (windows_ && c == '\\'); }
    std::string keyOf(const std::string& name) const {
        if (!windows_) return name;
        std::string key = name;
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return key;
    }
    // The names from the root to what a path names, `.` and `..` worked out, from the current
    // directory (the root) where the path does not start at one. False for one on another drive.
    bool split(const std::string& text, std::vector<std::string>& names) const {
        std::string_view path = text;
        if (windows_ && path.size() >= 2 && path[1] == ':') {
            if (std::tolower(static_cast<unsigned char>(path[0])) != 'c') return false;  // the one drive, C:
            path.remove_prefix(2);
        }
        size_t at = 0;
        while (at <= path.size()) {
            size_t end = at;
            while (end < path.size() && !isSeparator(path[end])) end++;
            const std::string_view name = path.substr(at, end - at);
            if (name == "..") {
                if (!names.empty()) names.pop_back();
            } else if (!name.empty() && name != ".") {
                names.emplace_back(name);
            }
            at = end + 1;
        }
        return true;
    }
    std::string display(const std::vector<std::string>& names) const {
        std::string text = windows_ ? "C:\\" : "/";
        for (size_t i = 0; i < names.size(); i++) {
            if (i > 0) text += windows_ ? '\\' : '/';
            text += names[i];
        }
        return text;
    }
    // The path written plainly: what a fault is matched against.
    std::string normal(const std::string& text) const {
        std::vector<std::string> names;
        return split(text, names) ? display(names) : text;
    }

    // ---- The tree; with the lock held ----

    Node* find(const std::vector<std::string>& names) {
        Node* node = &root_;
        for (const std::string& name : names) {
            if (!node->directory) return nullptr;
            auto child = node->children.find(keyOf(name));
            if (child == node->children.end()) return nullptr;
            node = child->second.get();
        }
        return node;
    }
    Node* find(const std::string& path) {
        std::vector<std::string> names;
        return split(path, names) ? find(names) : nullptr;
    }
    // The directory the last name is in, and that name; null where the directory is not one.
    Node* parentOf(const std::string& path, std::string& last, std::error_code& ec) {
        std::vector<std::string> names;
        if (!split(path, names) || names.empty()) {
            ec = errc(std::errc::invalid_argument);
            return nullptr;
        }
        last = names.back();
        names.pop_back();
        Node* parent = find(names);
        if (parent == nullptr) ec = errc(std::errc::no_such_file_or_directory);
        else if (!parent->directory) ec = errc(std::errc::not_a_directory);
        return ec ? nullptr : parent;
    }
    uint64_t used(const Node& node) const {
        uint64_t total = node.data.size();
        for (const auto& [key, child] : node.children) total += used(*child);
        return total;
    }
    bool roomFor(int64_t more) const { return diskSize_ < 0 || static_cast<int64_t>(used(root_)) + more <= diskSize_; }

    // The fault for an operation on a path, if any.
    const SandboxFault* faultFor(const std::string& operation, const std::string& path) const {
        const std::string written = normal(path);
        for (const SandboxFault& fault : faults_)
            if ((fault.operation == operation || fault.operation == "any") && (fault.path == "*" || normal(fault.path) == written)) return &fault;
        return nullptr;
    }
    std::error_code faultError(const SandboxFault& fault) { return faults().make(fault.error); }

    // ---- Files ----

    std::error_code readWhole(const std::string& path, std::string& data) override {
        std::lock_guard<std::mutex> hold(mutex_);
        if (const SandboxFault* fault = faultFor("open", path)) return faultError(*fault);
        Node* node = find(path);
        if (node == nullptr) return errc(std::errc::no_such_file_or_directory);
        if (node->directory) return errc(std::errc::is_a_directory);
        if (const SandboxFault* fault = faultFor("read", path); fault != nullptr && static_cast<int64_t>(node->data.size()) > fault->after)
            return faultError(*fault);
        data = node->data;
        return {};
    }
    std::error_code writeWhole(const std::string& path, const std::string& data, bool append) override {
        std::lock_guard<std::mutex> hold(mutex_);
        std::string last;
        std::error_code ec;
        if (const SandboxFault* fault = faultFor("open", path)) return faultError(*fault);
        Node* parent = parentOf(path, last, ec);
        if (parent == nullptr) return ec;
        auto existing = parent->children.find(keyOf(last));
        if (existing != parent->children.end() && existing->second->directory) return errc(std::errc::is_a_directory);
        if (existing != parent->children.end() && !existing->second->writable) return errc(std::errc::permission_denied);
        const size_t current = existing != parent->children.end() ? existing->second->data.size() : 0;
        const size_t before = append ? current : 0;
        size_t take = data.size();
        const SandboxFault* failing = faultFor("write", path);
        if (failing != nullptr && static_cast<int64_t>(data.size()) > failing->after) take = static_cast<size_t>(std::max<int64_t>(failing->after, 0));
        else failing = nullptr;
        // Room for it is judged before anything is made, so that nothing is left half made.
        if (!roomFor(static_cast<int64_t>(before + take) - static_cast<int64_t>(current))) return errc(std::errc::no_space_on_device);
        auto& slot = parent->children[keyOf(last)];
        if (slot == nullptr) {
            slot = std::make_unique<Node>();
            slot->name = last;
        }
        if (!append) slot->data.clear();
        slot->data.append(data, 0, take);
        slot->modified = nowNanoseconds();
        return failing != nullptr ? faultError(*failing) : std::error_code();
    }

    bool exists(const std::string& path) override {
        std::lock_guard<std::mutex> hold(mutex_);
        return find(path) != nullptr;
    }
    bool isFile(const std::string& path) override {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        return node != nullptr && !node->directory;
    }
    bool isDirectory(const std::string& path) override {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        return node != nullptr && node->directory;
    }
    bool isSymbolicLink(const std::string&) override { return false; }  // a sandbox has none
    std::optional<uint64_t> size(const std::string& path) override {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        if (node == nullptr || node->directory) return std::nullopt;
        return node->data.size();
    }
    std::optional<int64_t> lastModified(const std::string& path) override {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        if (node == nullptr) return std::nullopt;
        return node->modified;
    }
    std::error_code setLastModified(const std::string& path, int64_t nanoseconds) override {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        if (node == nullptr) return errc(std::errc::no_such_file_or_directory);
        node->modified = nanoseconds;
        return {};
    }

    bool remove(const std::string& path, std::error_code& ec) override {
        std::lock_guard<std::mutex> hold(mutex_);
        if (const SandboxFault* fault = faultFor("delete", path)) {
            ec = faultError(*fault);
            return false;
        }
        std::string last;
        Node* parent = parentOf(path, last, ec);
        if (parent == nullptr) {
            ec.clear();  // nothing there
            return false;
        }
        auto child = parent->children.find(keyOf(last));
        if (child == parent->children.end()) return false;
        if (child->second->directory && !child->second->children.empty()) {
            ec = errc(std::errc::directory_not_empty);
            return false;
        }
        parent->children.erase(child);
        return true;
    }
    uint64_t removeAll(const std::string& path, std::error_code& ec) override {
        std::lock_guard<std::mutex> hold(mutex_);
        if (const SandboxFault* fault = faultFor("delete", path)) {
            ec = faultError(*fault);
            return 0;
        }
        std::string last;
        Node* parent = parentOf(path, last, ec);
        if (parent == nullptr) {
            ec.clear();
            return 0;
        }
        auto child = parent->children.find(keyOf(last));
        if (child == parent->children.end()) return 0;
        const uint64_t count = counted(*child->second);
        parent->children.erase(child);
        return count;
    }
    static uint64_t counted(const Node& node) {
        uint64_t count = 1;
        for (const auto& [key, child] : node.children) count += counted(*child);
        return count;
    }
    void makeDirectory(const std::string& path, bool parents, std::error_code& ec) override {
        std::lock_guard<std::mutex> hold(mutex_);
        if (const SandboxFault* fault = faultFor("mkdir", path)) {
            ec = faultError(*fault);
            return;
        }
        std::vector<std::string> names;
        if (!split(path, names)) {
            ec = errc(std::errc::invalid_argument);
            return;
        }
        Node* node = &root_;
        for (size_t i = 0; i < names.size(); i++) {
            if (!node->directory) {
                ec = errc(std::errc::not_a_directory);
                return;
            }
            auto& slot = node->children[keyOf(names[i])];
            if (slot == nullptr) {
                if (!parents && i + 1 < names.size()) {
                    node->children.erase(keyOf(names[i]));
                    ec = errc(std::errc::no_such_file_or_directory);
                    return;
                }
                slot = std::make_unique<Node>();
                slot->directory = true;
                slot->name = names[i];
                slot->modified = nowNanoseconds();
            } else if (i + 1 == names.size() && !slot->directory) {
                ec = errc(std::errc::file_exists);
                return;
            }
            node = slot.get();
        }
    }
    std::error_code move(const std::string& from, const std::string& to) override {
        std::lock_guard<std::mutex> hold(mutex_);
        if (const SandboxFault* fault = faultFor("move", from)) return faultError(*fault);
        std::vector<std::string> source, target;
        if (!split(from, source) || !split(to, target) || source.empty() || target.empty()) return errc(std::errc::invalid_argument);
        if (target.size() > source.size() && std::equal(source.begin(), source.end(), target.begin(),
                                                        [&](const std::string& a, const std::string& b) { return keyOf(a) == keyOf(b); }))
            return errc(std::errc::invalid_argument);  // into itself
        std::string fromLast, toLast;
        std::error_code ec;
        Node* fromParent = parentOf(from, fromLast, ec);
        if (fromParent == nullptr) return ec;
        auto moving = fromParent->children.find(keyOf(fromLast));
        if (moving == fromParent->children.end()) return errc(std::errc::no_such_file_or_directory);
        Node* toParent = parentOf(to, toLast, ec);
        if (toParent == nullptr) return ec;
        auto there = toParent->children.find(keyOf(toLast));
        if (there != toParent->children.end() && there->second.get() != moving->second.get()) {
            if (there->second->directory && (!moving->second->directory || !there->second->children.empty()))
                return errc(there->second->children.empty() ? std::errc::is_a_directory : std::errc::directory_not_empty);
            if (!there->second->directory && moving->second->directory) return errc(std::errc::not_a_directory);
        }
        std::unique_ptr<Node> node = std::move(moving->second);
        fromParent->children.erase(moving);
        node->name = toLast;
        toParent->children[keyOf(toLast)] = std::move(node);
        return {};
    }
    bool copy(const std::string& from, const std::string& to, bool overwrite, std::error_code& ec) override {
        std::string data;
        {
            std::lock_guard<std::mutex> hold(mutex_);
            if (const SandboxFault* fault = faultFor("copy", from)) {
                ec = faultError(*fault);
                return false;
            }
            Node* source = find(from);
            if (source == nullptr) {
                ec = errc(std::errc::no_such_file_or_directory);
                return false;
            }
            if (source->directory) {
                ec = errc(std::errc::is_a_directory);
                return false;
            }
            Node* target = find(to);
            if (target != nullptr && target->directory) {
                ec = errc(std::errc::is_a_directory);
                return false;
            }
            if (target != nullptr && !overwrite) return false;
            data = source->data;
        }
        ec = writeWhole(to, data, false);
        return !ec;
    }
    std::error_code list(const std::string& path, std::vector<std::string>& names) override {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        if (node == nullptr) return errc(std::errc::no_such_file_or_directory);
        if (!node->directory) return errc(std::errc::not_a_directory);
        const SandboxFault* fault = faultFor("list", path);
        for (const auto& [key, child] : node->children) {
            if (fault != nullptr && static_cast<int64_t>(names.size()) >= fault->after) return faultError(*fault);
            names.push_back(child->name);
        }
        if (fault != nullptr) return faultError(*fault);  // fewer names than it lets through: it fails at the end
        return {};
    }
    // As list, with what each is: the sandbox has files and directories, no links.
    std::error_code entries(const std::string& path, std::vector<Entry>& found) override {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        if (node == nullptr) return errc(std::errc::no_such_file_or_directory);
        if (!node->directory) return errc(std::errc::not_a_directory);
        const SandboxFault* fault = faultFor("list", path);
        for (const auto& [key, child] : node->children) {
            if (fault != nullptr && static_cast<int64_t>(found.size()) >= fault->after) return faultError(*fault);
            Entry made;
            made.name = child->name;
            made.directory = child->directory;
            made.file = !child->directory;
            made.size = child->directory ? 0 : child->data.size();
            made.modified = child->modified;
            found.push_back(std::move(made));
        }
        if (fault != nullptr) return faultError(*fault);
        return {};
    }
    std::string canonical(const std::string& path, std::error_code& ec) override {
        std::lock_guard<std::mutex> hold(mutex_);
        std::vector<std::string> names;
        if (!split(path, names) || find(names) == nullptr) {
            ec = errc(std::errc::no_such_file_or_directory);
            return path;
        }
        return display(names);
    }
    std::string absolute(const std::string& path) override { return normal(path); }
    int permissions(const std::string& path) override {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        if (node == nullptr) return 0;
        if (node->directory) return 7;
        return 4 | (node->writable ? 2 : 0);
    }
    std::error_code setWritable(const std::string& path, bool writable) override {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        if (node == nullptr) return errc(std::errc::no_such_file_or_directory);
        node->writable = writable;
        return {};
    }
    std::optional<std::string> createTemp(const std::string& prefix, const std::string& suffix, bool directory, std::string& base,
                                          std::error_code& ec) override {
        base = windows_ ? "C:\\Temp" : "/tmp";
        for (int tries = 0; tries < 100; tries++) {
            const std::string made = base + (windows_ ? "\\" : "/") + prefix + std::to_string(++tempCounter_) + suffix;
            if (exists(made)) continue;
            if (directory) {
                makeDirectory(made, false, ec);
                if (ec) return std::nullopt;
            } else if ((ec = writeWhole(made, "", false))) {
                return std::nullopt;
            }
            return made;
        }
        return std::nullopt;
    }
    bool windowsPaths() const override { return windows_; }

    // ---- Streams ----

    std::shared_ptr<Channel> openFile(const std::string& path, const std::string& mode, std::string& error) override {
        const bool reading = mode == "r";
        {
            std::lock_guard<std::mutex> hold(mutex_);
            if (const SandboxFault* fault = faultFor("open", path)) {
                error = "cannot open '" + path + "': " + fault->error;
                return nullptr;
            }
            Node* node = find(path);
            if (reading && (node == nullptr || node->directory)) {
                error = "cannot open '" + path + "': " + errc(node == nullptr ? std::errc::no_such_file_or_directory : std::errc::is_a_directory).message();
                return nullptr;
            }
        }
        if (!reading) {
            // Made, or emptied for "w", as the system's open does.
            std::string none;
            const bool fresh = mode == "w" || !exists(path);
            if (const std::error_code ec = fresh ? writeWhole(path, none, false) : std::error_code()) {
                error = "cannot open '" + path + "' to write: " + ec.message();
                return nullptr;
            }
        }
        return std::make_shared<FileChannel>(shared_from_this(), path, reading);
    }

    // For a FileChannel: up to `max` bytes from `position`, as the file is now.
    int readAt(const std::string& path, size_t& position, char* out, size_t max, std::string& error) {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        if (node == nullptr || node->directory) {
            error = "cannot read '" + path + "': it is gone";
            return -1;
        }
        size_t limit = node->data.size();
        const SandboxFault* fault = faultFor("read", path);
        if (fault != nullptr) limit = std::min<size_t>(limit, static_cast<size_t>(std::max<int64_t>(fault->after, 0)));
        if (position >= limit) {
            if (fault != nullptr && node->data.size() > limit) {
                error = "cannot read '" + path + "': " + fault->error;
                return -1;
            }
            return 0;
        }
        const size_t count = std::min(max, limit - position);
        std::copy_n(node->data.data() + position, count, out);
        position += count;
        return static_cast<int>(count);
    }
    // For a FileChannel: bytes added at the end, `written` so far through it.
    bool writeAt(const std::string& path, size_t& written, const char* data, size_t size, std::string& error) {
        std::lock_guard<std::mutex> hold(mutex_);
        Node* node = find(path);
        if (node == nullptr || node->directory) {
            error = "cannot write '" + path + "': it is gone";
            return false;
        }
        size_t take = size;
        const SandboxFault* fault = faultFor("write", path);
        if (fault != nullptr) {
            const size_t room = written >= static_cast<size_t>(std::max<int64_t>(fault->after, 0)) ? 0 : static_cast<size_t>(fault->after) - written;
            take = std::min(take, room);
        }
        if (!roomFor(static_cast<int64_t>(take))) {
            error = "cannot write '" + path + "': " + errc(std::errc::no_space_on_device).message();
            return false;
        }
        node->data.append(data, take);
        node->modified = nowNanoseconds();
        written += take;
        if (take < size) {
            error = "cannot write '" + path + "': " + fault->error;
            return false;
        }
        return true;
    }

    // ---- What the sandbox is asked ----

    void addFault(SandboxFault fault) override {
        std::lock_guard<std::mutex> hold(mutex_);
        faults_.push_back(std::move(fault));
    }
    std::string faultMessage(const std::string& operation, const std::string& path) override {
        std::lock_guard<std::mutex> hold(mutex_);
        const SandboxFault* fault = faultFor(operation, path);
        return fault != nullptr ? fault->error : std::string();
    }

    // Everything as it is now: each path with whether it is a directory, and a file's contents.
    std::map<std::string, std::pair<bool, std::string>> snapshot() {
        std::lock_guard<std::mutex> hold(mutex_);
        std::map<std::string, std::pair<bool, std::string>> all;
        std::vector<std::string> names;
        std::function<void(const Node&)> walk = [&](const Node& node) {
            for (const auto& [key, child] : node.children) {
                names.push_back(child->name);
                all[display(names)] = {child->directory, child->directory ? std::string() : child->data};
                if (child->directory) walk(*child);
                names.pop_back();
            }
        };
        walk(root_);
        return all;
    }
    void keepStart() override { start_ = snapshot(); }
    std::vector<std::pair<std::string, std::string>> changes() override {
        const auto now = snapshot();
        std::vector<std::pair<std::string, std::string>> found;
        for (const auto& [path, entry] : now) {
            auto before = start_.find(path);
            if (before == start_.end()) found.emplace_back("made", path);
            else if (!entry.first && before->second.second != entry.second) found.emplace_back("changed", path);
        }
        for (const auto& [path, entry] : start_)
            if (now.find(path) == now.end()) found.emplace_back("deleted", path);
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        return found;
    }
    // A directory of the real system copied in, as `name` at the root.
    std::error_code copyIn(Host& real, const std::string& from, const std::string& name) override {
        const std::string into = (windows_ ? "C:\\" : "/") + name;
        std::error_code ec;
        makeDirectory(into, true, ec);
        if (ec) return ec;
        std::vector<std::string> names;
        if (std::error_code listed = real.list(from, names)) return listed;
        const char realSeparator = real.windowsPaths() ? '\\' : '/';
        for (const std::string& child : names) {
            const std::string source = from + realSeparator + child;
            const std::string target = into + (windows_ ? "\\" : "/") + child;
            if (real.isDirectory(source)) {
                if (std::error_code inner = copyIn(real, source, name + (windows_ ? "\\" : "/") + child)) return inner;
            } else {
                std::string data;
                if (std::error_code read = real.readWhole(source, data)) return read;
                if (std::error_code written = writeWhole(target, data, false)) return written;
            }
        }
        return {};
    }

private:
    std::mutex mutex_;
    Node root_;
    bool windows_;
    int64_t diskSize_;  // the most its files may hold together, in bytes; -1 for no limit
    std::vector<SandboxFault> faults_;
    std::map<std::string, std::pair<bool, std::string>> start_;
    std::atomic<uint64_t> tempCounter_{0};
};

int FileChannel::read(char* out, size_t max, std::string& error, bool&) {
    if (closed_) {
        error = "the stream is closed";
        return -1;
    }
    if (!reading_) {
        error = "the stream is not open for reading";
        return -1;
    }
    return host_->readAt(path_, position_, out, max, error);
}

bool FileChannel::write(const char* data, size_t size, std::string& error) {
    if (closed_) {
        error = "the stream is closed";
        return false;
    }
    if (reading_) {
        error = "the stream is not open for writing";
        return false;
    }
    return host_->writeAt(path_, position_, data, size, error);
}

}  // namespace

std::shared_ptr<SandboxHost> makeSandbox(bool windowsPaths, int64_t diskSize) { return std::make_shared<VirtualHost>(windowsPaths, diskSize); }

namespace {

// The sandboxes the program holds, by number: what the handle of a Sandbox object stands for.
// Its entry goes when the program's Sandbox is collected; code still running in it keeps it.
std::mutex registryMutex;
std::unordered_map<int64_t, std::shared_ptr<SandboxHost>> registry;
std::atomic<int64_t> nextSandbox{1};

void forget(int64_t id) {
    std::lock_guard<std::mutex> hold(registryMutex);
    registry.erase(id);
}

std::shared_ptr<SandboxHost> sandboxOf(VM& vm, Value handle) {
    if (handle.isObjType(ObjType::Resource)) {
        std::lock_guard<std::mutex> hold(registryMutex);
        auto found = registry.find(static_cast<ObjResource*>(handle.as.obj)->id);
        if (found != registry.end()) return found->second;
    }
    throw vm.runtimeError(Thrown::IllegalArgumentException, "not a sandbox");
}

}  // namespace

void registerSandbox(VM& vm) {
    ObjClass* box = vm.alloc<ObjClass>(vm.intern("Sandbox"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        box->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("Sandbox.") + name, minArgs, maxArgs, fn)));
    };

    // make(windowsPaths, diskSize, copyFrom, seed): a new sandbox, its files empty but for its
    // temporary directory, or with a directory of the real system copied in; diskSize -1 for no
    // limit; its randomness from `seed`.
    // make(windowsPaths, diskSize, copyFrom, seed, fontsFrom)
    def("make", 5, 5, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<SandboxHost> made = makeSandbox(args[0].truthy(), vm.toInt(args[1], "disk size"));
        made->random.seed(static_cast<uint64_t>(vm.toInt(args[3], "seed")));
        if (!args[2].isNull()) {
            const std::string from = vm.toStr(args[2], "directory");
            if (!realHost().isDirectory(from)) throw vm.runtimeError(Thrown::IllegalArgumentException, "cannot copy '" + from + "' into a sandbox: it is no directory");
            std::string name = from;
            while (name.size() > 1 && (name.back() == '/' || name.back() == '\\')) name.pop_back();
            const size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            if (const std::error_code ec = made->copyIn(realHost(), from, name))
                throw vm.runtimeError(Thrown::IllegalArgumentException, "cannot copy '" + from + "' into a sandbox: " + ec.message());
        }
        if (!args[4].isNull()) {
            const std::string fonts = vm.toStr(args[4], "directory");
            if (!realHost().isDirectory(fonts)) throw vm.runtimeError(Thrown::IllegalArgumentException, "cannot take fonts from '" + fonts + "': it is no directory");
            made->fontsFrom = realHost().absolute(fonts);
        }
        made->keepStart();
        made->id = nextSandbox++;
        {
            std::lock_guard<std::mutex> hold(registryMutex);
            registry[made->id] = made;
        }
        return Value::object(vm.alloc<ObjResource>("sandbox", made->id, &forget));
    });
    // The code running now reaches the sandbox, until leave.
    def("enter", 1, 1, [](VM& vm, int, Value* args) -> Value {
        vm.enterHost(sandboxOf(vm, args[0]));
        return Value::null();
    });
    def("leave", 0, 0, [](VM& vm, int, Value*) -> Value {
        vm.leaveHost();
        return Value::null();
    });
    def("id", 1, 1, [](VM& vm, int, Value* args) -> Value { return Value::integer(sandboxOf(vm, args[0])->id); });
    // What a fault says an operation on a path or host fails with; null where none does.
    def("fault", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const std::string message = sandboxOf(vm, args[0])->faultMessage(vm.toStr(args[1], "operation"), vm.toStr(args[2], "path"));
        return message.empty() ? Value::null() : vm.str(message);
    });
    // Its clock moved on by so many nanoseconds, as if that much time had passed (step 3).
    def("advance", 2, 2, [](VM& vm, int, Value* args) -> Value {
        sandboxOf(vm, args[0])->advance(vm.toInt(args[1], "nanoseconds"));
        return Value::null();
    });
    // The number of the sandbox the code running now is in; null on the real system.
    def("currentId", 0, 0, [](VM& vm, int, Value*) -> Value {
        Host& here = vm.currentHost();
        return here.isSandbox() ? Value::integer(static_cast<SandboxHost&>(here).id) : Value::null();
    });
    // addFault(sandbox, operation, path, after, error)
    def("addFault", 5, 5, [](VM& vm, int, Value* args) -> Value {
        SandboxFault fault;
        fault.operation = vm.toStr(args[1], "operation");
        fault.path = vm.toStr(args[2], "path");
        fault.after = vm.toInt(args[3], "after");
        fault.error = vm.toStr(args[4], "error");
        sandboxOf(vm, args[0])->addFault(std::move(fault));
        return Value::null();
    });
    // What has been made, changed and deleted: [kind, path] for each, by path.
    def("changes", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto found = sandboxOf(vm, args[0])->changes();
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        for (const auto& [kind, path] : found) {
            ObjList* pair = newList(vm);
            TempRoot keepPair(vm, Value::object(pair));
            pair->values().push_back(vm.str(kind));
            pair->values().push_back(vm.str(path));
            list->values().push_back(Value::object(pair));
        }
        return Value::object(list);
    });

    vm.defineGlobal("__sandbox", Value::object(box));
}

}  // namespace jade
