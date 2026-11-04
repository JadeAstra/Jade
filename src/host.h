#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace astra {

// One end of something read and written byte by byte that a sandbox keeps (M6.6.2): a file of its
// own opened as a stream. A stream of the system's is the system's handle instead (io.cpp).
class Channel {
public:
    virtual ~Channel() = default;
    // Up to `max` bytes: how many; 0 at the end; -1 with `error` filled in.
    virtual int read(char* out, size_t max, std::string& error) = 0;
    virtual bool write(const char* data, size_t size, std::string& error) = 0;
    virtual void close() = 0;
};

// What a program reaches outside itself (M6.6.2): files now; streams, processes, the network,
// the clock and fonts as the sandbox comes. The real host is the system's (host.cpp); a
// sandbox's keeps everything in memory. The standard library's C++ goes through the VM's host
// (VM::host) for each of these, and never to the system itself.
//
// Paths are the program's text, UTF-8. Each call may come from any thread: from the VM's, or
// from one doing a blocking job for it (VM::blocking), so a host keeps itself safe for that.
// Failures come back as std::error_code, whose message is what the program is told.
class Host {
public:
    virtual ~Host() = default;

    // ---- Files ----

    // A whole file read, or written (or added to); an error where it cannot be.
    virtual std::error_code readWhole(const std::string& path, std::string& data) = 0;
    virtual std::error_code writeWhole(const std::string& path, const std::string& data, bool append) = 0;

    virtual bool exists(const std::string& path) = 0;
    virtual bool isFile(const std::string& path) = 0;
    virtual bool isDirectory(const std::string& path) = 0;
    // Whether the path itself is a symbolic link (on Windows a junction too), whatever it leads to.
    virtual bool isSymbolicLink(const std::string& path) = 0;
    // In bytes; nothing where it is not there.
    virtual std::optional<uint64_t> size(const std::string& path) = 0;
    // When it was last written, in nanoseconds since the start of 1970; nothing where it is not there.
    virtual std::optional<int64_t> lastModified(const std::string& path) = 0;
    virtual std::error_code setLastModified(const std::string& path, int64_t nanoseconds) = 0;

    // A file, or a directory with nothing in it: whether it was there and is gone.
    virtual bool remove(const std::string& path, std::error_code& ec) = 0;
    // It and everything in it: how many were removed.
    virtual uint64_t removeAll(const std::string& path, std::error_code& ec) = 0;
    // The directory, and with `parents` any missing above it.
    virtual void makeDirectory(const std::string& path, bool parents, std::error_code& ec) = 0;
    virtual std::error_code move(const std::string& from, const std::string& to) = 0;
    // False, with no error, where something is at `to` and `overwrite` is not given.
    virtual bool copy(const std::string& from, const std::string& to, bool overwrite, std::error_code& ec) = 0;
    // The names in a directory, in no order; an error where it is not one, or where reading it
    // fails part of the way.
    virtual std::error_code list(const std::string& path, std::vector<std::string>& names) = 0;
    // With `.`, `..` and links worked out.
    virtual std::string canonical(const std::string& path, std::error_code& ec) = 0;
    // From the current directory, `.` and `..` worked out but not links; the path itself where
    // that cannot be done.
    virtual std::string absolute(const std::string& path) = 0;
    // What this program may do to it, as bits: 4 read, 2 write, 1 run; 0 if it is not there.
    virtual int permissions(const std::string& path) = 0;
    virtual std::error_code setWritable(const std::string& path, bool writable) = 0;
    // A new empty file or directory in the temporary directory, its name starting with
    // `prefix` (and a file's ending with `suffix`): its path, or nothing, with the directory it
    // was to be in, where none can be made.
    virtual std::optional<std::string> createTemp(const std::string& prefix, const std::string& suffix, bool directory,
                                                  std::string& base, std::error_code& ec) = 0;
    // Whether its paths are Windows' (`C:\data\a.txt`, either slash between names) rather than
    // Linux's (`/data/a.txt`): what the path functions (name, parent, join) go by.
    virtual bool windowsPaths() const = 0;

    // ---- Streams ----

    // A file opened as a stream ("r", "w" or "a"): a sandbox's channel to it, or null with
    // `error` where it cannot be. The real host gives none, and the stream is the system's.
    virtual std::shared_ptr<Channel> openFile(const std::string&, const std::string&, std::string&) { return nullptr; }

    // Whether it is a sandbox's: what it has no form of yet is refused there, never done on the
    // system.
    virtual bool isSandbox() const { return false; }
};

// The system's.
Host& realHost();

}  // namespace astra
