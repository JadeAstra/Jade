// The real host (M6.6.2): files as the system has them. What was in the standard library's file
// functions (stdlib.cpp) until the host came, and does the same.
#include "host.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace astra {

namespace {

namespace fs = std::filesystem;

// A path as the program wrote it is UTF-8, whatever the system's own encoding is.
fs::path toPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string fromPath(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

class RealHost final : public Host {
public:
    // The system says no more than that it failed: the standard library's message says the rest.
    std::error_code readWhole(const std::string& path, std::string& data) override {
        std::ifstream in(toPath(path), std::ios::binary);
        if (!in) return std::make_error_code(std::errc::io_error);
        data.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return {};
    }

    std::error_code writeWhole(const std::string& path, const std::string& data, bool append) override {
        std::ofstream out(toPath(path), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        if (out && out.write(data.data(), static_cast<std::streamsize>(data.size())).flush()) return {};
        return std::make_error_code(std::errc::io_error);
    }

    bool exists(const std::string& path) override {
        std::error_code ec;
        return fs::exists(toPath(path), ec);
    }
    bool isFile(const std::string& path) override {
        std::error_code ec;
        return fs::is_regular_file(toPath(path), ec);
    }
    bool isDirectory(const std::string& path) override {
        std::error_code ec;
        return fs::is_directory(toPath(path), ec);
    }
    bool isSymbolicLink(const std::string& path) override {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(toPath(path), ec);
        if (ec) return false;
#ifdef _WIN32
        if (status.type() == fs::file_type::junction) return true;
#endif
        return status.type() == fs::file_type::symlink;
    }
    std::optional<uint64_t> size(const std::string& path) override {
        std::error_code ec;
        const auto size = fs::file_size(toPath(path), ec);
        if (ec) return std::nullopt;
        return static_cast<uint64_t>(size);
    }
    std::optional<int64_t> lastModified(const std::string& path) override {
        std::error_code ec;
        const auto time = fs::last_write_time(toPath(path), ec);
        if (ec) return std::nullopt;
        const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()).count();
    }
    std::error_code setLastModified(const std::string& path, int64_t nanoseconds) override {
        std::error_code ec;
        const auto system = std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(nanoseconds)));
        fs::last_write_time(toPath(path), std::chrono::clock_cast<fs::file_time_type::clock>(system), ec);
        return ec;
    }

    bool remove(const std::string& path, std::error_code& ec) override { return fs::remove(toPath(path), ec) && !ec; }
    uint64_t removeAll(const std::string& path, std::error_code& ec) override {
        return static_cast<uint64_t>(fs::remove_all(toPath(path), ec));
    }
    void makeDirectory(const std::string& path, bool parents, std::error_code& ec) override {
        if (parents) fs::create_directories(toPath(path), ec);
        else fs::create_directory(toPath(path), ec);
    }
    std::error_code move(const std::string& from, const std::string& to) override {
        std::error_code ec;
        fs::rename(toPath(from), toPath(to), ec);
        return ec;
    }
    bool copy(const std::string& from, const std::string& to, bool overwrite, std::error_code& ec) override {
        return fs::copy_file(toPath(from), toPath(to), overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
    }
    std::error_code list(const std::string& path, std::vector<std::string>& names) override {
        std::error_code ec;
        fs::directory_iterator it(toPath(path), ec);
        if (ec) return ec;
        for (const fs::directory_iterator end; it != end; it.increment(ec)) names.push_back(fromPath(it->path().filename()));
        return ec;  // a failed step leaves the iterator at the end, the error in ec
    }
    std::string canonical(const std::string& path, std::error_code& ec) override { return fromPath(fs::canonical(toPath(path), ec)); }
    std::string absolute(const std::string& path) override {
        std::error_code ec;
        const fs::path written = toPath(path);
        const fs::path absolute = fs::absolute(written, ec);
        return fromPath(ec ? written : absolute.lexically_normal());
    }
    int permissions(const std::string& path) override {
        std::error_code ec;
        const fs::path native = toPath(path);
        const fs::file_status status = fs::status(native, ec);
        if (ec || !fs::exists(status)) return 0;
#ifdef _WIN32
        const fs::perms p = status.permissions();
        int bits = (p & fs::perms::owner_read) != fs::perms::none ? 4 : 0;
        if ((p & fs::perms::owner_write) != fs::perms::none) bits |= 2;
        // Windows runs a file by its extension, which PATHEXT lists.
        std::string ext = fromPath(native.extension());
        for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const char* runnable = std::getenv("PATHEXT");
        const std::string runnables = runnable != nullptr ? runnable : ".COM;.EXE;.BAT;.CMD";
        if (fs::is_regular_file(status) && !ext.empty() && (";" + runnables + ";").find(";" + ext + ";") != std::string::npos) bits |= 1;
#else
        // As the system judges it for this program: by owner, group or anyone, and for root.
        const std::string text = native.native();
        int bits = ::access(text.c_str(), R_OK) == 0 ? 4 : 0;
        if (::access(text.c_str(), W_OK) == 0) bits |= 2;
        if (::access(text.c_str(), X_OK) == 0) bits |= 1;
#endif
        return bits;
    }
    // On Windows its read-only flag; elsewhere by its owner, as Java's setWritable has it, and
    // kept from being written by anyone.
    std::error_code setWritable(const std::string& path, bool writable) override {
        std::error_code ec;
        fs::permissions(toPath(path), writable ? fs::perms::owner_write : fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        writable ? fs::perm_options::add : fs::perm_options::remove, ec);
        return ec;
    }
    std::optional<std::string> createTemp(const std::string& prefix, const std::string& suffix, bool directory, std::string& base,
                                          std::error_code& ec) override {
        const fs::path folder = fs::temp_directory_path(ec);
        if (ec) return std::nullopt;
        base = fromPath(folder);
        static std::atomic<uint64_t> counter{0};
        for (int tries = 0; tries < 100; tries++) {
            const uint64_t mixed = (static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) * 6364136223846793005ULL) +
                                   (++counter * 1442695040888963407ULL);
            const fs::path made = folder / toPath(prefix + std::to_string(mixed % 1000000000000ULL) + suffix);
            if (directory) {
                if (fs::create_directory(made, ec)) return fromPath(made);
            } else {
                // Made only if nothing is there: two programs never get the same file.
#ifdef _WIN32
                HANDLE h = CreateFileW(made.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                    return fromPath(made);
                }
#else
                if (!fs::exists(made, ec)) {
                    std::ofstream(made).close();
                    return fromPath(made);
                }
#endif
            }
        }
        ec.clear();
        return std::nullopt;
    }
    bool windowsPaths() const override {
#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }
};

}  // namespace

Host& realHost() {
    static RealHost host;
    return host;
}

}  // namespace astra
