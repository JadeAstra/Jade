// Streams, child processes and sockets: the built-in __io, which src/std/io.astra is
// written on.
//
// Everything here that can wait (a read, a write, an accept, waiting for a process to end)
// goes through VM::blocking. Called on a thread, it blocks that thread with the VM's lock
// released, so the other threads go on. Called in a task on the main thread, the task is
// set aside while a worker thread waits, and the event loop goes on. There is one set of
// functions for both.
//
// What the operating system does is below in two parts, one for Windows and one for the
// systems with POSIX's calls, Linux and macOS (G56): a Stream, reading and writing it, and the
// operations on files, processes and sockets. The functions programs call are written once,
// on those.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
extern char** environ;
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "builtins.h"
#include "charset.h"
#include "sandbox.h"
#include "vm.h"

namespace astra {

namespace {

AstraThrow ioError(VM& vm, const std::string& message) { return vm.errorOf("IOException", message); }

// What a blocking piece of work leaves for the function that turns it into a value.
struct Stream;
struct Outcome {
    std::string data;
    std::string error;
    bool end = false;
    int64_t number = 0;
    std::shared_ptr<Stream> made;
    bool timedOut = false;  // a read ran past the socket's readTimeout
};

// A read that ran past a socket's readTimeout is a SocketTimeoutException, an IOException as in
// Java (D120), told by the mark the wait left, not by the message's text (8.8f).
AstraThrow ioError(VM& vm, const Outcome& out) { return vm.errorOf(out.timedOut ? "SocketTimeoutException" : "IOException", out.error); }

// ---- A sandbox's programs and pipes (M6.6.2 step 4) ----

// A pipe between two ends in a sandbox: what is written is kept until it is read, however much.
struct PipeBuffer {
    std::mutex mutex;
    std::condition_variable changed;
    std::string data;
    bool writerClosed = false;
    bool readerClosed = false;
};

class PipeEnd final : public Channel {
public:
    PipeEnd(std::shared_ptr<PipeBuffer> buffer, bool reading) : buffer_(std::move(buffer)), reading_(reading) {}
    int read(char* out, size_t max, std::string& error, bool&) override {
        if (!reading_) {
            error = "the stream is not open for reading";
            return -1;
        }
        std::unique_lock<std::mutex> hold(buffer_->mutex);
        buffer_->changed.wait(hold, [&] { return !buffer_->data.empty() || buffer_->writerClosed || buffer_->readerClosed; });
        if (buffer_->readerClosed) {
            error = "the stream is closed";
            return -1;
        }
        if (buffer_->data.empty()) return 0;  // the writer has closed its end
        const size_t count = std::min(max, buffer_->data.size());
        std::copy_n(buffer_->data.data(), count, out);
        buffer_->data.erase(0, count);
        return static_cast<int>(count);
    }
    bool write(const char* data, size_t size, std::string& error) override {
        std::lock_guard<std::mutex> hold(buffer_->mutex);
        if (reading_ || buffer_->writerClosed || buffer_->readerClosed) {
            error = reading_ ? "the stream is not open for writing" : "the pipe is closed";
            return false;
        }
        buffer_->data.append(data, size);
        buffer_->changed.notify_all();
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> hold(buffer_->mutex);
        (reading_ ? buffer_->readerClosed : buffer_->writerClosed) = true;
        buffer_->changed.notify_all();
    }

private:
    std::shared_ptr<PipeBuffer> buffer_;
    bool reading_;
};

// A connection in a sandbox (step 5): what each end writes, kept for the other until read.
// Either may reset it; an end may wait for ever, so that a read of the other's with a time limit
// is up at once, by the sandbox's clock.
struct SocketLink {
    std::mutex mutex;
    std::condition_variable changed;
    std::string forEnd[2];         // what is waiting to be read by end 0 and by end 1
    bool doneWriting[2] = {false, false};
    bool gone[2] = {false, false};  // closed entirely
    bool hanging[2] = {false, false};
    bool reset = false;
    std::shared_ptr<Host> host;
};

class SocketEnd final : public Channel {
public:
    SocketEnd(std::shared_ptr<SocketLink> link, int side, std::string remoteHost, int remotePort, int localPort)
        : remoteHost(std::move(remoteHost)), remotePort(remotePort), localPort(localPort), link_(std::move(link)), side_(side) {}
    int read(char* out, size_t max, std::string& error, bool& timedOut) override {
        SocketLink& l = *link_;
        const int other = 1 - side_;
        std::unique_lock<std::mutex> hold(l.mutex);
        l.changed.wait(hold, [&] {
            return !l.forEnd[side_].empty() || l.doneWriting[other] || l.gone[other] || l.reset || l.gone[side_] || (l.hanging[other] && timeout_ > 0);
        });
        if (l.gone[side_]) {
            error = "the socket is closed";
            return -1;
        }
        // What was sent before a reset is read first, so that a test sees the same thing however
        // the threads ran.
        if (!l.forEnd[side_].empty()) {
            const size_t count = std::min(max, l.forEnd[side_].size());
            std::copy_n(l.forEnd[side_].data(), count, out);
            l.forEnd[side_].erase(0, count);
            return static_cast<int>(count);
        }
        if (l.reset) {
            error = "the connection was reset";
            return -1;
        }
        if (l.doneWriting[other] || l.gone[other]) return 0;
        // The other side waits for ever: the time limit is up, the clock moved on by it.
        if (l.host != nullptr && l.host->isSandbox()) static_cast<SandboxHost&>(*l.host).advance(timeout_ * 1000000);
        timedOut = true;
        error = "the read timed out";
        return -1;
    }
    bool write(const char* data, size_t size, std::string& error) override {
        SocketLink& l = *link_;
        std::lock_guard<std::mutex> hold(l.mutex);
        if (l.reset || l.gone[1 - side_]) {
            error = l.reset ? "the connection was reset" : "the other side has closed the connection";
            return false;
        }
        if (l.doneWriting[side_] || l.gone[side_]) {
            error = "the socket is closed";
            return false;
        }
        l.forEnd[1 - side_].append(data, size);
        l.changed.notify_all();
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> hold(link_->mutex);
        link_->gone[side_] = true;
        link_->doneWriting[side_] = true;
        link_->changed.notify_all();
    }
    void shutdownOutput() override {
        std::lock_guard<std::mutex> hold(link_->mutex);
        link_->doneWriting[side_] = true;
        link_->changed.notify_all();
    }
    void setReadTimeout(int64_t ms) override { timeout_ = ms; }
    void resetLink() {
        std::lock_guard<std::mutex> hold(link_->mutex);
        link_->reset = true;
        link_->changed.notify_all();
    }
    // This end waits for ever: until the connection is closed or reset.
    void hang() {
        std::unique_lock<std::mutex> hold(link_->mutex);
        link_->hanging[side_] = true;
        link_->changed.notify_all();
        link_->changed.wait(hold, [&] { return link_->reset || link_->gone[0] || link_->gone[1]; });
    }

    const std::string remoteHost;
    const int remotePort;
    const int localPort;

private:
    std::shared_ptr<SocketLink> link_;
    int side_;
    int64_t timeout_ = 0;
};

// A port a program listens on in a sandbox: the connections made to it, waiting for accept.
struct SandboxListener {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Stream>> waiting;
    bool closed = false;
    int port = 0;
};

// A program of a sandbox's: an Astra thread running what the test declared, its end told here.
struct SimulatedProcess {
    std::mutex mutex;
    std::condition_variable changed;
    bool ended = false;
    int code = 0;
    bool hanging = false;  // it waits for ever (SandboxProcess.waitForever): a time limit on it is up at once
    int64_t pid = 0;
    std::shared_ptr<Host> host;                          // whose clock such a time limit moves on
    std::vector<std::shared_ptr<Channel>> programEnds;  // its own ends of the pipes, closed when it is killed
};

// Waits for it to end, `limit` ms at most (-1 for none): whether it did. A limit on one that waits
// for ever moves its sandbox's clock on by the limit, and is up at once.
bool waitSimulated(SimulatedProcess& p, int64_t limit, int& code) {
    std::unique_lock<std::mutex> hold(p.mutex);
    for (;;) {
        if (p.ended) {
            code = p.code;
            return true;
        }
        if (limit == 0) return false;
        if (p.hanging && limit > 0) {
            if (p.host != nullptr && p.host->isSandbox()) static_cast<SandboxHost&>(*p.host).advance(limit * 1000000);
            return false;
        }
        p.changed.wait(hold);
    }
}

// Its end, with `code`; killed, its ends of the pipes are closed, so that what reads them ends.
void endSimulated(SimulatedProcess& p, int code, bool killed) {
    std::vector<std::shared_ptr<Channel>> ends;
    {
        std::lock_guard<std::mutex> hold(p.mutex);
        if (p.ended) return;
        p.ended = true;
        p.code = code;
        if (killed) ends = p.programEnds;
        p.changed.notify_all();
    }
    for (const auto& end : ends) end->close();
}

#ifdef _WIN32

// ---- Windows: streams ----

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string narrow(const wchar_t* wide, int length = -1) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, length, utf8.data(), size, nullptr, nullptr);
    if (length < 0 && !utf8.empty() && utf8.back() == '\0') utf8.pop_back();
    return utf8;
}

// What Windows has to say about an error code.
std::string systemMessage(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                                        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string message = length > 0 ? narrow(text, static_cast<int>(length)) : "error " + std::to_string(code);
    if (text != nullptr) LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.' || message.back() == ' ')) message.pop_back();
    return message;
}

// Something that can be read from, written to, waited for or closed: a file, one end of a
// pipe, a socket, a listening socket, a process. Programs know each by a number.
struct Stream {
    enum class Kind { File, Pipe, Socket, Listener, Process };
    Kind kind;
    HANDLE handle = INVALID_HANDLE_VALUE;
    SOCKET socket = INVALID_SOCKET;
    DWORD pid = 0;

    std::atomic<bool> closed{false};
    std::atomic<int> users{0};  // threads inside a system call on it

    // Read side: what has been read from the system and not yet handed out.
    std::mutex readMutex;
    std::string buffer;
    size_t position = 0;
    bool ended = false;
    std::mutex writeMutex;
    // Read in Charset.utf16: whether its start has been looked at for a byte order mark, and
    // the order it says (G53); written in it, whether the mark has been.
    bool orderKnown = false;
    Charset readOrder = Charset::Utf16Be;
    bool markWritten = false;

    // A process that has ended, and what it ended with.
    std::atomic<bool> exited{false};
    std::atomic<int> exitCode{0};

    // A sandbox's file or pipe (M6.6.2): read, written and closed through it, not the system.
    std::shared_ptr<Channel> channel;
    // A sandbox's program, for a Process stream (M6.6.2 step 4).
    std::shared_ptr<SimulatedProcess> simulated;
    // A sandbox's port a program listens on, for a Listener (step 5).
    std::shared_ptr<SandboxListener> listener;

    explicit Stream(Kind k) : kind(k) {}

    ~Stream() { close(); }

    bool isSocket() const { return kind == Kind::Socket || kind == Kind::Listener; }

    // Closes it, first getting any thread that is waiting in it out.
    void close() {
        if (closed.exchange(true)) return;
        if (channel != nullptr) {
            channel->close();
            return;
        }
        if (listener != nullptr) {
            std::lock_guard<std::mutex> hold(listener->mutex);
            listener->closed = true;
            listener->changed.notify_all();
            return;
        }
        if (isSocket()) {
            if (socket != INVALID_SOCKET) {
                shutdown(socket, SD_BOTH);
                closesocket(socket);  // a thread in accept or recv returns with an error
                socket = INVALID_SOCKET;
            }
            return;
        }
        if (handle == INVALID_HANDLE_VALUE) return;
        while (users.load() > 0) {
            CancelIoEx(handle, nullptr);
            Sleep(1);
        }
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
};

struct Use {
    Stream& stream;
    explicit Use(Stream& s) : stream(s) { stream.users++; }
    ~Use() { stream.users--; }
};

// Reads up to `max` bytes. The count read; 0 at the end; -1 with `result.error` filled in.
int rawRead(Stream& s, char* out, size_t max, Outcome& result) {
    Use use(s);
    if (s.closed.load()) {
        result.error = "the stream is closed";
        return -1;
    }
    if (s.channel != nullptr) return s.channel->read(out, std::min<size_t>(max, 1 << 30), result.error, result.timedOut);
    if (s.isSocket()) {
        const int got = recv(s.socket, out, static_cast<int>(std::min<size_t>(max, 1 << 30)), 0);
        if (got >= 0) return got;
        const int code = WSAGetLastError();
        if (s.closed.load()) result.error = "the socket is closed";
        else if (code == WSAETIMEDOUT) {
            result.error = "the read timed out";
            result.timedOut = true;
        }
        else if (code == WSAECONNRESET || code == WSAECONNABORTED) return 0;  // the other side went away
        else result.error = "cannot read from the socket: " + systemMessage(static_cast<DWORD>(code));
        return -1;
    }
    DWORD got = 0;
    if (ReadFile(s.handle, out, static_cast<DWORD>(std::min<size_t>(max, 1 << 30)), &got, nullptr)) return static_cast<int>(got);
    const DWORD code = GetLastError();
    if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) return 0;  // the writer has closed its end
    result.error = code == ERROR_OPERATION_ABORTED || s.closed.load() ? "the stream is closed" : "cannot read: " + systemMessage(code);
    return -1;
}

bool rawWrite(Stream& s, const char* data, size_t size, std::string& error) {
    Use use(s);
    if (s.channel != nullptr) {
        if (s.closed.load()) {
            error = "the stream is closed";
            return false;
        }
        return s.channel->write(data, size, error);
    }
    while (size > 0) {
        if (s.closed.load()) {
            error = "the stream is closed";
            return false;
        }
        if (s.isSocket()) {
            const int sent = send(s.socket, data, static_cast<int>(std::min<size_t>(size, 1 << 30)), 0);
            if (sent == SOCKET_ERROR) {
                error = s.closed.load() ? "the socket is closed" : "cannot write to the socket: " + systemMessage(static_cast<DWORD>(WSAGetLastError()));
                return false;
            }
            data += sent;
            size -= static_cast<size_t>(sent);
        } else {
            DWORD written = 0;
            if (!WriteFile(s.handle, data, static_cast<DWORD>(std::min<size_t>(size, 1 << 30)), &written, nullptr)) {
                const DWORD code = GetLastError();
                error = code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA ? "the other end of the pipe is closed"
                        : code == ERROR_OPERATION_ABORTED || s.closed.load() ? "the stream is closed"
                        : "cannot write: " + systemMessage(code);
                return false;
            }
            data += written;
            size -= written;
        }
    }
    return true;
}

#else  // POSIX

// ---- POSIX: streams (G56) ----

// What the system has to say about an error number.
std::string systemMessage(int code) {
    std::string message = std::strerror(code);
    while (!message.empty() && (message.back() == '.' || message.back() == ' ')) message.pop_back();
    return message;
}

// Something that can be read from, written to, waited for or closed: a file, one end of a
// pipe, a socket, a listening socket, a process. Programs know each by a number.
struct Stream {
    enum class Kind { File, Pipe, Socket, Listener, Process };
    Kind kind;
    int fd = -1;
    pid_t pid = 0;
    int readTimeout = 0;  // a socket's, in ms; 0 for none

    std::atomic<bool> closed{false};
    std::atomic<int> users{0};  // threads inside a system call on it

    // Read side: what has been read from the system and not yet handed out.
    std::mutex readMutex;
    std::string buffer;
    size_t position = 0;
    bool ended = false;
    std::mutex writeMutex;
    // Read in Charset.utf16: whether its start has been looked at for a byte order mark, and
    // the order it says (G53); written in it, whether the mark has been.
    bool orderKnown = false;
    Charset readOrder = Charset::Utf16Be;
    bool markWritten = false;

    // A process that has ended, and what it ended with; one waits for it at a time.
    std::atomic<bool> exited{false};
    std::atomic<int> exitCode{0};
    std::mutex waitMutex;

    // A sandbox's file or pipe (M6.6.2): read, written and closed through it, not the system.
    std::shared_ptr<Channel> channel;
    // A sandbox's program, for a Process stream (M6.6.2 step 4).
    std::shared_ptr<SimulatedProcess> simulated;
    // A sandbox's port a program listens on, for a Listener (step 5).
    std::shared_ptr<SandboxListener> listener;

    explicit Stream(Kind k) : kind(k) {}

    ~Stream() { close(); }

    bool isSocket() const { return kind == Kind::Socket || kind == Kind::Listener; }

    // Closes it, first getting any thread that is waiting in it out: one in a socket's read
    // returns at the shutdown, one waiting for something to read sees `closed` (waitReadable).
    void close() {
        if (closed.exchange(true)) return;
        if (channel != nullptr) {
            channel->close();
            return;
        }
        if (listener != nullptr) {
            std::lock_guard<std::mutex> hold(listener->mutex);
            listener->closed = true;
            listener->changed.notify_all();
            return;
        }
        if (fd < 0) return;
        if (kind == Kind::Socket) ::shutdown(fd, SHUT_RDWR);
        while (users.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ::close(fd);
        fd = -1;
    }
};

struct Use {
    Stream& stream;
    explicit Use(Stream& s) : stream(s) { stream.users++; }
    ~Use() { stream.users--; }
};

// Waits until the stream has something to read (or its end, or an error, which the read then
// says), or it is closed, or `timeout` ms have passed (-1 for no limit). In slices, so that a
// close() is seen at once, which on these systems does not wake a read already waiting.
bool waitReadable(Stream& s, int timeout, Outcome& result) {
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        if (s.closed.load()) {
            result.error = s.isSocket() ? "the socket is closed" : "the stream is closed";
            return false;
        }
        int slice = 100;
        if (timeout >= 0) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            if (spent >= timeout) {
                result.error = "the read timed out";
                result.timedOut = true;
                return false;
            }
            slice = static_cast<int>(std::min<int64_t>(slice, timeout - spent));
        }
        pollfd waiting{s.fd, POLLIN, 0};
        const int ready = ::poll(&waiting, 1, slice);
        if (ready > 0) return true;
        if (ready < 0 && errno != EINTR) {
            result.error = "cannot read: " + systemMessage(errno);
            return false;
        }
    }
}

// Reads up to `max` bytes. The count read; 0 at the end; -1 with `result.error` filled in.
int rawRead(Stream& s, char* out, size_t max, Outcome& result) {
    Use use(s);
    if (s.closed.load()) {
        result.error = "the stream is closed";
        return -1;
    }
    if (s.channel != nullptr) return s.channel->read(out, std::min<size_t>(max, 1 << 30), result.error, result.timedOut);
    // A file has something to read at once; anything else may wait.
    if (s.kind != Stream::Kind::File && !waitReadable(s, s.isSocket() && s.readTimeout > 0 ? s.readTimeout : -1, result)) return -1;
    const size_t wanted = std::min<size_t>(max, 1 << 30);
    for (;;) {
        const ssize_t got = s.isSocket() ? ::recv(s.fd, out, wanted, 0) : ::read(s.fd, out, wanted);
        if (got >= 0) return static_cast<int>(got);
        if (errno == EINTR) continue;
        if (s.isSocket() && (errno == ECONNRESET || errno == ECONNABORTED)) return 0;  // the other side went away
        if (s.closed.load()) result.error = s.isSocket() ? "the socket is closed" : "the stream is closed";
        else result.error = (s.isSocket() ? "cannot read from the socket: " : "cannot read: ") + systemMessage(errno);
        return -1;
    }
}

bool rawWrite(Stream& s, const char* data, size_t size, std::string& error) {
    Use use(s);
    if (s.channel != nullptr) {
        if (s.closed.load()) {
            error = "the stream is closed";
            return false;
        }
        return s.channel->write(data, size, error);
    }
    while (size > 0) {
        if (s.closed.load()) {
            error = s.isSocket() ? "the socket is closed" : "the stream is closed";
            return false;
        }
        const size_t wanted = std::min<size_t>(size, 1 << 30);
        const ssize_t written = s.isSocket() ? ::send(s.fd, data, wanted, 0) : ::write(s.fd, data, wanted);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (s.closed.load()) error = s.isSocket() ? "the socket is closed" : "the stream is closed";
            else if (s.isSocket()) error = "cannot write to the socket: " + systemMessage(errno);
            else error = errno == EPIPE ? "the other end of the pipe is closed" : "cannot write: " + systemMessage(errno);
            return false;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

#endif

// ---- The streams programs hold ----

// The streams that are open, by number. One table for the whole process: a worker has a VM
// and a lock of its own, so the table has its own lock too.
std::mutex streamsMutex;
std::unordered_map<int64_t, std::shared_ptr<Stream>> streams;
int64_t nextStream = 1;

int64_t keep(std::shared_ptr<Stream> stream) {
    std::lock_guard<std::mutex> hold(streamsMutex);
    const int64_t id = nextStream++;
    streams.emplace(id, std::move(stream));
    return id;
}

// The stream of that number, or null; with `take`, it is taken out of the table as well.
std::shared_ptr<Stream> findStream(Value v, bool take = false) {
    std::lock_guard<std::mutex> hold(streamsMutex);
    const auto found = v.isInt() ? streams.find(v.as.i) : streams.end();
    if (found == streams.end()) return nullptr;
    std::shared_ptr<Stream> stream = found->second;
    if (take) streams.erase(found);
    return stream;
}

std::shared_ptr<Stream> streamOf(VM& vm, Value v) {
    std::shared_ptr<Stream> stream = findStream(v);
    if (stream == nullptr) throw ioError(vm, "the stream is closed");
    return stream;
}

Value bytesValue(VM& vm, const std::string& data) {
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data.assign(data.begin(), data.end());
    vm.account(made->data.size());
    return Value::object(made);
}

// More from the system into the stream's buffer. False at the end or on an error.
bool fill(Stream& s, Outcome& out) {
    if (s.ended) return false;
    if (s.position > 0) {
        // What has been handed out goes, so that the buffer holds only what is still to come.
        s.buffer.erase(0, s.position);
        s.position = 0;
    }
    const size_t before = s.buffer.size();
    s.buffer.resize(before + 64 * 1024);
    const int got = rawRead(s, s.buffer.data() + before, 64 * 1024, out);
    s.buffer.resize(before + static_cast<size_t>(got > 0 ? got : 0));
    if (got <= 0) s.ended = got == 0;
    return got > 0;
}

// The set a stream is read in: for Charset.utf16, the order the byte order mark at its start
// says, the mark passed over (G53). Called holding the read lock; false where reading failed.
bool readingOrder(Stream& s, Charset& cs, Outcome& out) {
    if (cs != Charset::Utf16) return true;
    if (!s.orderKnown) {
        while (s.buffer.size() - s.position < 2 && fill(s, out)) {}
        if (!out.error.empty()) return false;
        size_t mark = 0;
        s.readOrder = utf16Order(std::string_view(s.buffer).substr(s.position), Charset::Utf16Be, mark);
        s.position += mark;
        s.orderKnown = true;
    }
    cs = s.readOrder;
    return true;
}

// What spawn is asked for: a program and its arguments, where it runs, where its three
// standard streams go ("pipe", "inherit", "discard", "merge" for the error stream, or ">path",
// ">>path", "<path"), and changes to the environment (a map, or null).
struct Spawn {
    std::vector<std::string> command;
    std::string directory;
    std::string modes[3];
    Value environment;
};

[[noreturn]] void badRedirect(VM& vm, const std::string& mode) {
    throw vm.runtimeError(Thrown::IllegalArgumentException, "a process's stream goes to pipe, inherit, discard, merge or a file, not '" + mode + "'");
}

#ifdef _WIN32

// ---- Windows: files, processes and sockets ----

bool openFile(const std::string& path, const std::string& mode, Outcome& out) {
    const bool reading = mode == "r";
    HANDLE handle = CreateFileW(widen(path).c_str(), reading ? GENERIC_READ : (mode == "a" ? FILE_APPEND_DATA : GENERIC_WRITE),
                                FILE_SHARE_READ | (reading ? FILE_SHARE_WRITE : 0), nullptr,
                                reading ? OPEN_EXISTING : (mode == "a" ? OPEN_ALWAYS : CREATE_ALWAYS), FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        out.error = std::string(reading ? "cannot read file '" : "cannot write file '") + path + "': " + systemMessage(GetLastError());
        return false;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::File);
    out.made->handle = handle;
    return true;
}

// One argument as it has to be written on a Windows command line for the program started to
// read it back unchanged.
void appendArgument(std::wstring& line, const std::wstring& argument) {
    if (!line.empty()) line += L' ';
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += argument;
        return;
    }
    line += L'"';
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < argument.size() && argument[i] == L'\\') {
            i++;
            backslashes++;
        }
        if (i == argument.size()) {
            line.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"') line.append(backslashes * 2 + 1, L'\\');
        else line.append(backslashes, L'\\');
        line += argument[i];
    }
    line += L'"';
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    HANDLE release() {
        HANDLE out = h;
        h = INVALID_HANDLE_VALUE;
        return out;
    }
};

// Where one of a new process's three standard streams goes.
struct Redirect {
    Handle child;   // the end the new process gets
    Handle parent;  // the end this program keeps, for "pipe"
};

void openRedirect(VM& vm, const std::string& mode, bool childReads, DWORD standard, Redirect& out) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (mode == "pipe") {
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inheritable, 0)) throw ioError(vm, "cannot make a pipe: " + systemMessage(GetLastError()));
        out.child.h = childReads ? read : write;
        out.parent.h = childReads ? write : read;
        SetHandleInformation(out.parent.h, HANDLE_FLAG_INHERIT, 0);  // only the child's end goes to the child
    } else if (mode == "inherit") {
        HANDLE own = GetStdHandle(standard);
        if (own == nullptr || own == INVALID_HANDLE_VALUE) own = INVALID_HANDLE_VALUE;
        else if (!DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &out.child.h, 0, TRUE, DUPLICATE_SAME_ACCESS)) out.child.h = INVALID_HANDLE_VALUE;
        if (out.child.h == INVALID_HANDLE_VALUE) openRedirect(vm, "discard", childReads, standard, out);  // this program has none itself
    } else if (mode == "discard") {
        out.child.h = CreateFileW(L"NUL", childReads ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
    } else if (!mode.empty() && (mode[0] == '>' || mode[0] == '<')) {
        const bool append = mode.compare(0, 2, ">>") == 0;
        const std::wstring path = widen(mode.substr(append ? 2 : 1));
        out.child.h = mode[0] == '<' ? CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, &inheritable, OPEN_EXISTING, 0, nullptr)
                                     : CreateFileW(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                                                   append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out.child.h == INVALID_HANDLE_VALUE)
            throw ioError(vm, "cannot open '" + mode.substr(append ? 2 : 1) + "' for the process: " + systemMessage(GetLastError()));
    } else {
        badRedirect(vm, mode);
    }
}

// The environment of this program with `changes` made to it, as CreateProcess wants it.
std::wstring environmentBlock(VM& vm, Value changes) {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    if (wchar_t* own = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = own; *entry != L'\0'; entry += wcslen(entry) + 1) {
            const std::wstring text(entry);
            const size_t equals = text.find(L'=', 1);  // names such as "=C:" start with one
            if (equals != std::wstring::npos) entries.emplace_back(text.substr(0, equals), text.substr(equals + 1));
        }
        FreeEnvironmentStringsW(own);
    }
    for (const ValueMap::Entry& change : asMap(changes)->map.entries()) {
        if (!change.live) continue;
        const std::wstring name = widen(vm.toStr(change.key, "environment name"));
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) { return _wcsicmp(entry.first.c_str(), name.c_str()) == 0; }),
                      entries.end());
        if (!change.value.isNull()) entries.emplace_back(name, widen(vm.stringify(change.value)));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::wstring block;
    for (const auto& entry : entries) {
        block += entry.first + L'=' + entry.second;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

// Starts the program: the process, then the ends of its streams this program keeps (null for
// those that are not "pipe").
std::array<std::shared_ptr<Stream>, 4> startProcess(VM& vm, const Spawn& spawn) {
    std::wstring commandLine;
    for (const std::string& part : spawn.command) appendArgument(commandLine, widen(part));
    const std::string& program = spawn.command[0];
    const std::wstring directory = widen(spawn.directory);
    const std::string& errorMode = spawn.modes[2];

    Redirect input, output, error;
    openRedirect(vm, spawn.modes[0], true, STD_INPUT_HANDLE, input);
    openRedirect(vm, spawn.modes[1], false, STD_OUTPUT_HANDLE, output);
    if (errorMode != "merge") openRedirect(vm, errorMode, false, STD_ERROR_HANDLE, error);
    std::wstring environment;
    if (spawn.environment.isObjType(ObjType::Map)) environment = environmentBlock(vm, spawn.environment);

    // The new process is given these three handles and no others, whatever else this
    // program has open at the moment.
    HANDLE inherited[3] = {input.child.h, output.child.h, errorMode == "merge" ? output.child.h : error.child.h};
    const DWORD handleCount = inherited[2] == inherited[1] ? 2 : 3;
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<char> attributeMemory(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeMemory.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, handleCount * sizeof(HANDLE), nullptr, nullptr))
        throw ioError(vm, "cannot start '" + program + "': " + systemMessage(GetLastError()));

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inherited[0];
    startup.StartupInfo.hStdOutput = inherited[1];
    startup.StartupInfo.hStdError = inherited[2];
    startup.lpAttributeList = attributes;

    // A program without a console of its own does not make one appear for what it starts.
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | (GetConsoleWindow() == nullptr ? CREATE_NO_WINDOW : 0);
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> line(commandLine.begin(), commandLine.end());
    line.push_back(L'\0');
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, flags, environment.empty() ? nullptr : environment.data(),
                                        directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &info);
    const DWORD failure = started ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!started) throw ioError(vm, "cannot start '" + program + "': " + systemMessage(failure));
    CloseHandle(info.hThread);

    std::array<std::shared_ptr<Stream>, 4> made;
    made[0] = std::make_shared<Stream>(Stream::Kind::Process);
    made[0]->handle = info.hProcess;
    made[0]->pid = info.dwProcessId;
    Redirect* ends[3] = {&input, &output, &error};
    for (int k = 0; k < 3; k++) {
        if (ends[k]->parent.h == INVALID_HANDLE_VALUE) continue;
        made[k + 1] = std::make_shared<Stream>(Stream::Kind::Pipe);
        made[k + 1]->handle = ends[k]->parent.release();
    }
    return made;
}

// Waits for a process to end, `limit` ms at most (-1 for as long as it takes): whether it
// ended, its exit code then kept.
bool waitProcess(Stream& s, int64_t limit) {
    if (s.simulated != nullptr) {
        int code = 0;
        if (!waitSimulated(*s.simulated, limit, code)) return false;
        s.exitCode = code;
        s.exited = true;
        return true;
    }
    if (s.exited.load()) return true;
    if (s.closed.load() || WaitForSingleObject(s.handle, limit < 0 ? INFINITE : static_cast<DWORD>(std::min<int64_t>(limit, 4000000000))) != WAIT_OBJECT_0)
        return false;
    DWORD code = 0;
    GetExitCodeProcess(s.handle, &code);
    s.exitCode = static_cast<int>(code);
    s.exited = true;
    return true;
}

void killProcess(Stream& s) {
    if (s.simulated != nullptr) return endSimulated(*s.simulated, 1, true);
    TerminateProcess(s.handle, 1);
}

int64_t currentProcessId() { return GetCurrentProcessId(); }

void startNetworking(VM& vm) {
    static std::mutex winsockMutex;
    static bool winsockStarted = false;
    std::lock_guard<std::mutex> hold(winsockMutex);
    if (winsockStarted) return;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw ioError(vm, "networking is not available");
    winsockStarted = true;
}

// A TCP connection, tried for at most `timeout` ms where that is not negative.
void connectTo(const std::string& host, int64_t port, int64_t timeout, Outcome& out) {
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfoW* found = nullptr;
    if (GetAddrInfoW(widen(host).c_str(), std::to_wstring(port).c_str(), &hints, &found) != 0 || found == nullptr) {
        out.error = "cannot find host '" + host + "'";
        return;
    }
    int lastError = 0;
    for (const addrinfoW* address = found; address != nullptr && out.made == nullptr; address = address->ai_next) {
        SOCKET s = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        bool connected;
        if (timeout < 0) {
            connected = ::connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0;
            if (!connected) lastError = WSAGetLastError();
        } else {
            // With a time limit: start connecting without waiting, then wait that long.
            u_long nonBlocking = 1;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            connected = ::connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0;
            if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set writable, failed;
                FD_ZERO(&writable);
                FD_ZERO(&failed);
                FD_SET(s, &writable);
                FD_SET(s, &failed);
                timeval limit{static_cast<long>(timeout / 1000), static_cast<long>((timeout % 1000) * 1000)};
                const int ready = select(0, nullptr, &writable, &failed, &limit);
                connected = ready > 0 && FD_ISSET(s, &writable);
                lastError = ready == 0 ? WSAETIMEDOUT : WSAECONNREFUSED;
            }
            nonBlocking = 0;
            ioctlsocket(s, FIONBIO, &nonBlocking);
        }
        if (!connected) {
            closesocket(s);
            continue;
        }
        out.made = std::make_shared<Stream>(Stream::Kind::Socket);
        out.made->socket = s;
    }
    FreeAddrInfoW(found);
    if (out.made == nullptr)
        out.error = "cannot connect to " + host + ":" + std::to_string(port) + ": " +
                    (lastError == WSAETIMEDOUT ? "timed out" : systemMessage(static_cast<DWORD>(lastError)));
}

std::shared_ptr<Stream> listenOn(VM& vm, int64_t port, const std::string& address, int backlog) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<u_short>(port));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!address.empty() && InetPtonW(AF_INET, widen(address == "localhost" ? "127.0.0.1" : address).c_str(), &local.sin_addr) != 1)
        throw ioError(vm, "'" + address + "' is not an address to listen on");

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 || ::listen(s, backlog) != 0) {
        const std::string why = systemMessage(static_cast<DWORD>(WSAGetLastError()));
        if (s != INVALID_SOCKET) closesocket(s);
        throw ioError(vm, "cannot listen on port " + std::to_string(port) + ": " + why);
    }
    auto listener = std::make_shared<Stream>(Stream::Kind::Listener);
    listener->socket = s;
    return listener;
}

void acceptOn(Stream& s, Outcome& out) {
    Use use(s);
    const SOCKET connection = ::accept(s.socket, nullptr, nullptr);
    if (connection == INVALID_SOCKET) {
        out.error = s.closed.load() ? "the socket is closed" : "cannot accept a connection: " + systemMessage(static_cast<DWORD>(WSAGetLastError()));
        return;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::Socket);
    out.made->socket = connection;
}

// The local port, and the remote address and port, of a socket; false for one it cannot say.
bool localPort(Stream& s, int& port) {
    sockaddr_storage address{};
    int size = sizeof(address);
    if (getsockname(s.socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    port = ntohs(address.ss_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6&>(address).sin6_port : reinterpret_cast<const sockaddr_in&>(address).sin_port);
    return true;
}
bool remoteEnd(Stream& s, std::string& host, int& port) {
    sockaddr_storage address{};
    int size = sizeof(address);
    if (getpeername(s.socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    wchar_t text[64] = {};
    const void* raw = address.ss_family == AF_INET6 ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6&>(address).sin6_addr)
                                                    : static_cast<const void*>(&reinterpret_cast<const sockaddr_in&>(address).sin_addr);
    InetNtopW(address.ss_family, raw, text, 64);
    host = narrow(text);
    port = ntohs(address.ss_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6&>(address).sin6_port : reinterpret_cast<const sockaddr_in&>(address).sin_port);
    return true;
}

void setReadTimeout(Stream& s, int64_t limit) {
    const DWORD ms = static_cast<DWORD>(limit);
    setsockopt(s.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

void shutdownOutput(Stream& s) { shutdown(s.socket, SD_SEND); }

#else  // POSIX

// ---- POSIX: files, processes and sockets (G56) ----

bool openFile(const std::string& path, const std::string& mode, Outcome& out) {
    const bool reading = mode == "r";
    const int flags = reading ? O_RDONLY : O_WRONLY | O_CREAT | (mode == "a" ? O_APPEND : O_TRUNC);
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC, 0666);
    if (fd < 0) {
        out.error = std::string(reading ? "cannot read file '" : "cannot write file '") + path + "': " + systemMessage(errno);
        return false;
    }
    out.made = std::make_shared<Stream>(Stream::Kind::File);
    out.made->fd = fd;
    return true;
}

// The file descriptors a spawn opens, closed when it is done with them.
struct Descriptors {
    std::vector<int> open;
    ~Descriptors() {
        for (int fd : open) ::close(fd);
    }
    int add(int fd) {
        open.push_back(fd);
        return fd;
    }
    int release(int fd) {
        open.erase(std::remove(open.begin(), open.end(), fd), open.end());
        return fd;
    }
};

// Starts the program: the process, then the ends of its streams this program keeps (null for
// those that are not "pipe"). Every descriptor this program opens is closed in what it starts
// (O_CLOEXEC), but for the three it is given.
std::array<std::shared_ptr<Stream>, 4> startProcess(VM& vm, const Spawn& spawn) {
    Descriptors fds;
    int child[3] = {-1, -1, -1}, parent[3] = {-1, -1, -1};
    for (int k = 0; k < 3; k++) {
        const std::string& mode = spawn.modes[k];
        const bool childReads = k == 0;
        if (mode == "pipe") {
            int ends[2];
            if (::pipe(ends) != 0) throw ioError(vm, "cannot make a pipe: " + systemMessage(errno));
            for (int end : ends) ::fcntl(fds.add(end), F_SETFD, FD_CLOEXEC);
            child[k] = childReads ? ends[0] : ends[1];
            parent[k] = childReads ? ends[1] : ends[0];
        } else if (mode == "inherit" || (k == 2 && mode == "merge")) {
            // The same as this program's, or (merge) its output.
        } else if (mode == "discard") {
            child[k] = fds.add(::open("/dev/null", (childReads ? O_RDONLY : O_WRONLY) | O_CLOEXEC));
        } else if (!mode.empty() && (mode[0] == '>' || mode[0] == '<')) {
            const bool append = mode.compare(0, 2, ">>") == 0;
            const std::string path = mode.substr(append ? 2 : 1);
            const int fd = mode[0] == '<' ? ::open(path.c_str(), O_RDONLY | O_CLOEXEC)
                                          : ::open(path.c_str(), O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC) | O_CLOEXEC, 0666);
            if (fd < 0) throw ioError(vm, "cannot open '" + path + "' for the process: " + systemMessage(errno));
            child[k] = fds.add(fd);
        } else {
            badRedirect(vm, mode);
        }
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int k = 0; k < 3; k++)
        if (child[k] >= 0) posix_spawn_file_actions_adddup2(&actions, child[k], k);
    if (spawn.modes[2] == "merge") posix_spawn_file_actions_adddup2(&actions, 1, 2);
    if (!spawn.directory.empty()) posix_spawn_file_actions_addchdir_np(&actions, spawn.directory.c_str());
    // What this program ignores (SIGPIPE, below) the new one does not.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> argv;
    for (const std::string& part : spawn.command) argv.push_back(const_cast<char*>(part.c_str()));
    argv.push_back(nullptr);
    // This program's environment, with the changes asked for made to it.
    std::vector<std::string> entries;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; entry++) entries.emplace_back(*entry);
    if (spawn.environment.isObjType(ObjType::Map)) {
        for (const ValueMap::Entry& change : asMap(spawn.environment)->map.entries()) {
            if (!change.live) continue;
            const std::string name = vm.toStr(change.key, "environment name");
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const std::string& entry) { return entry.compare(0, name.size() + 1, name + "=") == 0; }),
                          entries.end());
            if (!change.value.isNull()) entries.push_back(name + "=" + vm.stringify(change.value));
        }
    }
    std::vector<char*> envp;
    for (std::string& entry : entries) envp.push_back(entry.data());
    envp.push_back(nullptr);

    pid_t pid = 0;
    const int failure = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (failure != 0) throw ioError(vm, "cannot start '" + spawn.command[0] + "': " + systemMessage(failure));

    std::array<std::shared_ptr<Stream>, 4> made;
    made[0] = std::make_shared<Stream>(Stream::Kind::Process);
    made[0]->pid = pid;
    for (int k = 0; k < 3; k++) {
        if (parent[k] < 0) continue;
        made[k + 1] = std::make_shared<Stream>(Stream::Kind::Pipe);
        made[k + 1]->fd = fds.release(parent[k]);
    }
    return made;  // the child's ends close here, in this program
}

// Waits for a process to end, `limit` ms at most (-1 for as long as it takes): whether it
// ended, its exit code then kept (128 and the signal's number for one a signal ended, as a
// shell says).
bool waitProcess(Stream& s, int64_t limit) {
    if (s.simulated != nullptr) {
        int code = 0;
        if (!waitSimulated(*s.simulated, limit, code)) return false;
        s.exitCode = code;
        s.exited = true;
        return true;
    }
    const auto started = std::chrono::steady_clock::now();
    int pause = 1;
    for (;;) {
        {
            std::lock_guard<std::mutex> hold(s.waitMutex);
            if (s.exited.load()) return true;
            int status = 0;
            const pid_t done = ::waitpid(s.pid, &status, WNOHANG);
            if (done == s.pid) {
                s.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
                s.exited = true;
                return true;
            }
            if (done < 0 && errno != EINTR) {
                s.exitCode = -1;  // not this program's to wait for any more
                s.exited = true;
                return true;
            }
        }
        if (s.closed.load()) return false;
        if (limit >= 0 && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(limit)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(pause));
        pause = std::min(pause * 2, 10);
    }
}

// Only one not yet seen to end: once it has been waited for, its number may be another's.
void killProcess(Stream& s) {
    if (s.simulated != nullptr) return endSimulated(*s.simulated, 1, true);
    std::lock_guard<std::mutex> hold(s.waitMutex);
    if (!s.exited.load()) ::kill(s.pid, SIGKILL);
}

int64_t currentProcessId() { return ::getpid(); }

void startNetworking(VM&) {}

// A socket closed in what this program starts.
int newSocket(int family, int type, int protocol) {
    const int fd = ::socket(family, type, protocol);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

// A TCP connection, tried for at most `timeout` ms where that is not negative.
void connectTo(const std::string& host, int64_t port, int64_t timeout, Outcome& out) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || found == nullptr) {
        out.error = "cannot find host '" + host + "'";
        return;
    }
    int lastError = 0;
    bool timedOut = false;
    for (const addrinfo* address = found; address != nullptr && out.made == nullptr; address = address->ai_next) {
        const int fd = newSocket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        bool connected;
        if (timeout < 0) {
            connected = ::connect(fd, address->ai_addr, address->ai_addrlen) == 0;
            if (!connected) lastError = errno;
        } else {
            // With a time limit: start connecting without waiting, then wait that long.
            const int flags = ::fcntl(fd, F_GETFL);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            connected = ::connect(fd, address->ai_addr, address->ai_addrlen) == 0;
            if (!connected && errno == EINPROGRESS) {
                pollfd waiting{fd, POLLOUT, 0};
                const int ready = ::poll(&waiting, 1, static_cast<int>(std::min<int64_t>(timeout, 2000000000)));
                int failure = 0;
                socklen_t size = sizeof(failure);
                if (ready > 0) ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &size);
                connected = ready > 0 && failure == 0;
                timedOut = ready == 0;
                lastError = ready > 0 ? failure : errno;
            } else if (!connected) {
                lastError = errno;
            }
            ::fcntl(fd, F_SETFL, flags);
        }
        if (!connected) {
            ::close(fd);
            continue;
        }
        out.made = std::make_shared<Stream>(Stream::Kind::Socket);
        out.made->fd = fd;
    }
    ::freeaddrinfo(found);
    if (out.made == nullptr)
        out.error = "cannot connect to " + host + ":" + std::to_string(port) + ": " + (timedOut ? "timed out" : systemMessage(lastError));
}

std::shared_ptr<Stream> listenOn(VM& vm, int64_t port, const std::string& address, int backlog) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<uint16_t>(port));
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!address.empty() && ::inet_pton(AF_INET, (address == "localhost" ? "127.0.0.1" : address).c_str(), &local.sin_addr) != 1)
        throw ioError(vm, "'" + address + "' is not an address to listen on");
    const int fd = newSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const int reuse = 1;  // a port just let go of may be taken again at once, as on Windows
    if (fd >= 0) ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 || ::listen(fd, backlog) != 0) {
        const std::string why = systemMessage(errno);
        if (fd >= 0) ::close(fd);
        throw ioError(vm, "cannot listen on port " + std::to_string(port) + ": " + why);
    }
    auto listener = std::make_shared<Stream>(Stream::Kind::Listener);
    listener->fd = fd;
    return listener;
}

void acceptOn(Stream& s, Outcome& out) {
    Use use(s);
    if (!waitReadable(s, -1, out)) return;
    const int connection = ::accept(s.fd, nullptr, nullptr);
    if (connection < 0) {
        out.error = s.closed.load() ? "the socket is closed" : "cannot accept a connection: " + systemMessage(errno);
        return;
    }
    ::fcntl(connection, F_SETFD, FD_CLOEXEC);
    out.made = std::make_shared<Stream>(Stream::Kind::Socket);
    out.made->fd = connection;
}

int portOf(const sockaddr_storage& address) {
    return ntohs(address.ss_family == AF_INET6 ? reinterpret_cast<const sockaddr_in6&>(address).sin6_port : reinterpret_cast<const sockaddr_in&>(address).sin_port);
}

// The local port, and the remote address and port, of a socket; false for one it cannot say.
bool localPort(Stream& s, int& port) {
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    if (::getsockname(s.fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    port = portOf(address);
    return true;
}
bool remoteEnd(Stream& s, std::string& host, int& port) {
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    if (::getpeername(s.fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
    char text[64] = {};
    const void* raw = address.ss_family == AF_INET6 ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in6&>(address).sin6_addr)
                                                    : static_cast<const void*>(&reinterpret_cast<const sockaddr_in&>(address).sin_addr);
    ::inet_ntop(address.ss_family, raw, text, sizeof(text));
    host = text;
    port = portOf(address);
    return true;
}

void setReadTimeout(Stream& s, int64_t limit) { s.readTimeout = static_cast<int>(std::min<int64_t>(limit, 2000000000)); }

void shutdownOutput(Stream& s) { ::shutdown(s.fd, SHUT_WR); }

#endif

std::vector<std::string> selfCommand;

}  // namespace

void setSelfCommand(std::vector<std::string> command) { selfCommand = std::move(command); }

// What a sandbox has no form of yet (M6.6.2): refused there, never done on the system.
void refuseInSandbox(VM& vm, const std::string& what) {
    if (vm.currentHost().isSandbox()) throw vm.errorOf("IOException", what + " is not available in a sandbox yet");
}

void registerIo(VM& vm) {
#ifndef _WIN32
    // A write to a pipe or a socket whose other end has gone is an IOException, not the end of
    // the program.
    ::signal(SIGPIPE, SIG_IGN);
#endif
    ObjClass* io = vm.alloc<ObjClass>(vm.intern("io"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        io->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("io.") + name, minArgs, maxArgs, fn)));
    };

    // ---- Streams ----

    // open(path, mode): "r" to read, "w" to write a new file, "a" to add to the end of one.
    def("open", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const std::string path = vm.toStr(args[0], "file path");
        const std::string mode = vm.toStr(args[1], "mode");
        auto out = std::make_shared<Outcome>();
        std::shared_ptr<Host> host = vm.currentHostShared();
        return vm.blocking([=] {
                               if (!host->isSandbox()) {
                                   openFile(path, mode, *out);
                                   return;
                               }
                               // A sandbox's file (M6.6.2), through its channel.
                               std::shared_ptr<Channel> channel = host->openFile(path, mode, out->error);
                               if (channel == nullptr) return;
                               out->made = std::make_shared<Stream>(Stream::Kind::File);
                               out->made->channel = std::move(channel);
                           },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::integer(keep(out->made));
                           });
    });

    // Up to `max` bytes, at least one; null at the end.
    def("read", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const int64_t wanted = vm.toInt(args[1], "number of bytes");
        if (wanted < 1) throw vm.runtimeError(Thrown::IllegalArgumentException, "read() takes how many bytes to read at most, which is at least 1");
        const size_t max = static_cast<size_t>(std::min<int64_t>(wanted, 1 << 30));
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                if (s->position == s->buffer.size() && !fill(*s, *out)) {
                    out->end = out->error.empty();
                    return;
                }
                const size_t count = std::min(max, s->buffer.size() - s->position);
                out->data.assign(s->buffer, s->position, count);
                s->position += count;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return out->end ? Value::null() : bytesValue(vm, out->data);
            });
    });

    // Everything up to the end, as bytes.
    def("readAll", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                while (fill(*s, *out)) {}
                if (!out->error.empty()) return;
                out->data.assign(s->buffer, s->position, std::string::npos);
                s->buffer.clear();
                s->position = 0;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return bytesValue(vm, out->data);
            });
    });

    // Everything up to the end, as text in the character set (D127).
    def("readText", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const Charset cs = charsetOf(vm, argc > 1 ? args[1] : Value::null());
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                Charset order = cs;
                if (!readingOrder(*s, order, *out)) return;
                while (fill(*s, *out)) {}
                if (!out->error.empty()) return;
                out->data = decodeText(std::string_view(s->buffer).substr(s->position), order);
                s->buffer.clear();
                s->position = 0;
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return vm.str(std::move(out->data));
            });
    });

    // The next line, without its line end, in the character set (D127); null at the end.
    def("readLine", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const Charset cs = charsetOf(vm, argc > 1 ? args[1] : Value::null());
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                std::lock_guard<std::mutex> hold(s->readMutex);
                Charset order = cs;
                if (!readingOrder(*s, order, *out)) return;
                size_t searched = 0;  // how much of what is waiting has been looked through
                const size_t unit = isUtf16(order) ? 2 : 1;
                for (;;) {
                    size_t endSize = 1;
                    const size_t at = lineEnd(s->buffer, s->position + searched, order, endSize);
                    if (at != std::string::npos) {
                        out->data.assign(s->buffer, s->position, at - s->position);
                        s->position = at + endSize;
                        break;
                    }
                    searched = (s->buffer.size() - s->position) / unit * unit;
                    if (!fill(*s, *out)) {
                        if (!out->error.empty()) return;
                        // The end: what is left is the last line, if anything is.
                        if (s->position == s->buffer.size()) {
                            out->end = true;
                            return;
                        }
                        out->data.assign(s->buffer, s->position, std::string::npos);
                        s->position = s->buffer.size();
                        break;
                    }
                }
                out->data = decodeText(withoutCarriageReturn(std::move(out->data), order), order);
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return out->end ? Value::null() : vm.str(out->data);
            });
    });

    // Text (in the character set, D127) or bytes. Returns when all of it has been handed to
    // the system.
    def("write", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        std::string data;
        // In Charset.utf16, a byte order mark before the stream's first text, none after (G53).
        bool marked = false;
        if (args[1].isObjType(ObjType::Bytes)) {
            data.assign(asBytes(args[1])->data.begin(), asBytes(args[1])->data.end());
        } else {
            const Charset cs = charsetOf(vm, argc > 2 ? args[2] : Value::null());
            marked = cs == Charset::Utf16;
            data = encodeText(vm.stringify(args[1]), marked ? Charset::Utf16Be : cs);
        }
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [s, out, marked, data = std::move(data)] {
                std::lock_guard<std::mutex> hold(s->writeMutex);
                if (marked && !s->markWritten) {
                    s->markWritten = true;
                    if (!rawWrite(*s, "\xFE\xFF", 2, out->error)) return;
                }
                rawWrite(*s, data.data(), data.size(), out->error);
            },
            [out](VM& vm) -> Value {
                if (!out->error.empty()) throw ioError(vm, *out);
                return Value::null();
            });
    });

    def("close", 1, 1, [](VM& vm, int, Value* args) -> Value {
        std::shared_ptr<Stream> s = findStream(args[0], true);
        if (s == nullptr) return Value::null();
        // Closing may have to wait for a thread that is reading it to notice.
        return vm.blocking([s] { s->close(); }, [](VM&) -> Value { return Value::null(); });
    });

    def("isOpen", 1, 1, [](VM&, int, Value* args) -> Value {
        const std::shared_ptr<Stream> s = findStream(args[0]);
        return Value::boolean(s != nullptr && !s->closed.load());
    });

    // ---- Processes ----

    // spawn(command, directory, input, output, error, environment): starts a program. Gives
    // [process, its input, its output, its error output], the last three null unless "pipe".
    def("spawn", 6, 6, [](VM& vm, int, Value* args) -> Value {
        refuseInSandbox(vm, "starting a program");
        if (!args[0].isObjType(ObjType::List) || asList(args[0])->size() == 0)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "a process is started from a list: the program, then its arguments");
        Spawn spawn;
        for (Value part : vm.listCopy(asList(args[0]))) spawn.command.push_back(vm.stringify(part));
        if (!args[1].isNull()) spawn.directory = vm.toStr(args[1], "directory");
        spawn.modes[0] = vm.toStr(args[2], "input stream");
        spawn.modes[1] = vm.toStr(args[3], "output stream");
        spawn.modes[2] = vm.toStr(args[4], "error stream");
        spawn.environment = args[5];
        const std::array<std::shared_ptr<Stream>, 4> made = startProcess(vm, spawn);

        ObjList* result = newList(vm);
        TempRoot keepResult(vm, Value::object(result));
        for (const std::shared_ptr<Stream>& stream : made) result->values().push_back(stream == nullptr ? Value::null() : Value::integer(keep(stream)));
        return Value::object(result);
    });

    // Waits for a process to end: its exit code, or null if `timeout` ms ran out first.
    def("processWait", 1, 2, [](VM& vm, int argc, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        const int64_t limit = argc > 1 && !args[1].isNull() ? static_cast<int64_t>(std::clamp<double>(vm.toNumber(args[1], "time limit"), 0, 4e9)) : -1;
        auto out = std::make_shared<Outcome>();
        return vm.blocking(
            [=] {
                if (!waitProcess(*s, limit)) {
                    out->end = true;  // still running
                    return;
                }
                out->number = s->exitCode.load();
            },
            [out](VM&) -> Value { return out->end ? Value::null() : Value::integer(out->number); });
    });

    // collect(process, output, error, charset): reads what a process writes to its output and
    // its error output, each to its end, at the same time (so that neither fills up while the
    // other is read), and waits for it to end. Gives [exit code, output, error], the text in
    // the character set; either stream may be null, giving "".
    // collect(process, output, errors, charset, timeout?): what the program writes, read to its end,
    // and its exit code; with a timeout in milliseconds, the program is killed when it runs
    // longer, and a TimeoutException thrown (8.8h).
    def("collect", 4, 5, [](VM& vm, int argc, Value* args) -> Value {
        auto process = streamOf(vm, args[0]);
        std::shared_ptr<Stream> pipes[2] = {args[1].isNull() ? nullptr : streamOf(vm, args[1]), args[2].isNull() ? nullptr : streamOf(vm, args[2])};
        const Charset cs = charsetOf(vm, args[3]);
        const int64_t limit = argc > 4 && !args[4].isNull() ? std::max<int64_t>(vm.toInt(args[4], "timeout"), 0) : -1;
        auto out = std::make_shared<Outcome>();
        auto texts = std::make_shared<std::array<std::string, 2>>();
        auto killed = std::make_shared<std::atomic<bool>>(false);
        return vm.blocking(
            [=] {
                // A watch that kills the program when the time is up, which ends its pipes too.
                std::mutex watchMutex;
                std::condition_variable watchWake;
                bool finished = false;
                std::thread watch;
                if (limit >= 0 && process->simulated != nullptr)
                    // A sandbox's program: a time limit is up at once on one that waits for ever.
                    watch = std::thread([&, sim = process->simulated] {
                        int code = 0;
                        if (!waitSimulated(*sim, limit, code)) {
                            killed->store(true);
                            endSimulated(*sim, 1, true);
                        }
                    });
                else if (limit >= 0)
                    watch = std::thread([&] {
                        std::unique_lock<std::mutex> hold(watchMutex);
                        if (!watchWake.wait_for(hold, std::chrono::milliseconds(limit), [&] { return finished; }) && !process->exited.load()) {
                            killed->store(true);
                            killProcess(*process);
                        }
                    });
                std::string errors[2];
                auto drain = [&](int k) {
                    if (pipes[k] == nullptr) return;
                    Stream& s = *pipes[k];
                    std::lock_guard<std::mutex> hold(s.readMutex);
                    Outcome mine;
                    while (fill(s, mine)) {}
                    errors[k] = mine.error;
                    (*texts)[k] = decodeText(std::string_view(s.buffer).substr(s.position), cs);
                    s.buffer.clear();
                    s.position = 0;
                };
                std::thread second(drain, 1);
                drain(0);
                second.join();
                out->error = !errors[0].empty() ? errors[0] : errors[1];
                waitProcess(*process, -1);
                out->number = process->exitCode.load();
                if (watch.joinable()) {
                    {
                        std::lock_guard<std::mutex> hold(watchMutex);
                        finished = true;
                    }
                    watchWake.notify_all();
                    watch.join();
                }
            },
            [out, texts, killed, limit](VM& vm) -> Value {
                if (killed->load())
                    throw vm.errorOf("TimeoutException", "the program ran longer than " + std::to_string(limit) + " ms, and was stopped");
                if (!out->error.empty()) throw ioError(vm, *out);
                ObjList* result = newList(vm);
                TempRoot keepResult(vm, Value::object(result));
                result->values().push_back(Value::integer(out->number));
                for (std::string& text : *texts) result->values().push_back(vm.str(std::move(text)));
                return Value::object(result);
            });
    });

    def("processAlive", 1, 1, [](VM&, int, Value* args) -> Value {
        const std::shared_ptr<Stream> s = findStream(args[0]);
        if (s == nullptr || s->exited.load() || s->closed.load()) return Value::boolean(false);
        return Value::boolean(!waitProcess(*s, 0));
    });

    def("processKill", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        if (!s->exited.load() && !s->closed.load()) killProcess(*s);
        return Value::null();
    });

    def("processId", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto s = streamOf(vm, args[0]);
        return Value::integer(s->simulated != nullptr ? s->simulated->pid : static_cast<int64_t>(s->pid));
    });

    // ---- A sandbox's programs (M6.6.2 step 4); only in one ----

    // A pipe: [the reading end, the writing end], each a stream.
    def("sandboxPipe", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (!vm.currentHost().isSandbox()) throw vm.runtimeError(Thrown::IllegalStateException, "a sandbox's pipe is made in a sandbox");
        auto buffer = std::make_shared<PipeBuffer>();
        auto reading = std::make_shared<Stream>(Stream::Kind::Pipe);
        reading->channel = std::make_shared<PipeEnd>(buffer, true);
        auto writing = std::make_shared<Stream>(Stream::Kind::Pipe);
        writing->channel = std::make_shared<PipeEnd>(buffer, false);
        return Value::object(newList(vm, {Value::integer(keep(reading)), Value::integer(keep(writing))}));
    });
    // A program of the sandbox's, as a Process stream: its end told by sandboxEnded.
    def("sandboxProcess", 0, 0, [](VM& vm, int, Value*) -> Value {
        if (!vm.currentHost().isSandbox()) throw vm.runtimeError(Thrown::IllegalStateException, "a sandbox's program is started in a sandbox");
        static std::atomic<int64_t> nextPid{40000};
        auto made = std::make_shared<Stream>(Stream::Kind::Process);
        made->simulated = std::make_shared<SimulatedProcess>();
        made->simulated->pid = nextPid++;
        made->simulated->host = vm.currentHostShared();
        return Value::integer(keep(made));
    });
    // One of the program's own ends of its pipes, closed when it is killed.
    def("sandboxOwn", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const auto process = streamOf(vm, args[0]);
        const auto end = streamOf(vm, args[1]);
        if (process->simulated != nullptr && end->channel != nullptr) {
            std::lock_guard<std::mutex> hold(process->simulated->mutex);
            process->simulated->programEnds.push_back(end->channel);
        }
        return Value::null();
    });
    // The program has ended, with its exit code.
    def("sandboxEnded", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const auto process = streamOf(vm, args[0]);
        if (process->simulated != nullptr) endSimulated(*process->simulated, static_cast<int>(vm.toInt(args[1], "exit code")), false);
        return Value::null();
    });
    // The program waits for ever: until it is killed.
    def("sandboxHang", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto process = streamOf(vm, args[0]);
        auto sim = process->simulated;
        if (sim == nullptr) return Value::null();
        {
            std::lock_guard<std::mutex> hold(sim->mutex);
            sim->hanging = true;
            sim->changed.notify_all();
        }
        return vm.blocking([sim] {
            std::unique_lock<std::mutex> hold(sim->mutex);
            sim->changed.wait(hold, [&] { return sim->ended; });
        }, [](VM&) -> Value { return Value::null(); });
    });

    def("currentPid", 0, 0, [](VM&, int, Value*) -> Value { return Value::integer(currentProcessId()); });

    // What started this program: the path of its executable and, when astra is running a
    // script, the script. Starting that list again starts this program again.
    def("selfCommand", 0, 0, [](VM& vm, int, Value*) -> Value {
        ObjList* list = newList(vm);
        TempRoot keepList(vm, Value::object(list));
        for (const std::string& part : selfCommand) list->values().push_back(vm.str(part));
        return Value::object(list);
    });

    // ---- Sockets ----

    // connect(host, port, timeout): a TCP connection, tried for at most `timeout` ms if given.
    def("connect", 2, 3, [](VM& vm, int argc, Value* args) -> Value {
        refuseInSandbox(vm, "connecting to another computer");
        startNetworking(vm);
        const std::string host = vm.toStr(args[0], "host");
        const int64_t port = vm.toInt(args[1], "port");
        const int64_t timeout = argc > 2 && !args[2].isNull() ? std::max<int64_t>(vm.toInt(args[2], "time limit"), 0) : -1;
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] { connectTo(host, port, timeout, *out); },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::integer(keep(out->made));
                           });
    });

    // listen(port, address, backlog): a socket that takes connections. Port 0 lets the system
    // choose one; with no address it listens on every one this machine has.
    def("listen", 1, 3, [](VM& vm, int argc, Value* args) -> Value {
        refuseInSandbox(vm, "listening on a port");
        startNetworking(vm);
        const int64_t port = vm.toInt(args[0], "port");
        const std::string address = argc > 1 && !args[1].isNull() ? vm.toStr(args[1], "address") : std::string();
        const int backlog = argc > 2 && !args[2].isNull() ? static_cast<int>(vm.toInt(args[2], "backlog")) : 50;
        return Value::integer(keep(listenOn(vm, port, address, backlog)));
    });

    // Waits for a connection and gives it.
    def("accept", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        auto out = std::make_shared<Outcome>();
        return vm.blocking([=] {
                               if (s->listener == nullptr) return acceptOn(*s, *out);
                               // A sandbox's port: the next connection made to it (step 5).
                               SandboxListener& l = *s->listener;
                               std::unique_lock<std::mutex> hold(l.mutex);
                               l.changed.wait(hold, [&] { return !l.waiting.empty() || l.closed; });
                               if (l.closed) {
                                   out->error = "the socket is closed";
                                   return;
                               }
                               out->made = l.waiting.front();
                               l.waiting.pop_front();
                           },
                           [out](VM& vm) -> Value {
                               if (!out->error.empty()) throw ioError(vm, *out);
                               return Value::integer(keep(out->made));
                           });
    });

    // [local port, remote address, remote port] of a socket; the last two null for one that listens.
    def("socketInfo", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto s = streamOf(vm, args[0]);
        ObjList* list = newList(vm);
        TempRoot keepList(vm, Value::object(list));
        // A sandbox's socket or port (step 5).
        if (auto end = std::dynamic_pointer_cast<SocketEnd>(s->channel)) {
            list->values().push_back(Value::integer(end->localPort));
            list->values().push_back(vm.str(end->remoteHost));
            list->values().push_back(Value::integer(end->remotePort));
            return Value::object(list);
        }
        if (s->listener != nullptr) {
            list->values().push_back(Value::integer(s->listener->port));
            list->values().push_back(Value::null());
            list->values().push_back(Value::null());
            return Value::object(list);
        }
        int port = 0;
        list->values().push_back(localPort(*s, port) ? Value::integer(port) : Value::null());
        std::string host;
        if (s->kind == Stream::Kind::Socket && remoteEnd(*s, host, port)) {
            list->values().push_back(vm.str(host));
            list->values().push_back(Value::integer(port));
        } else {
            list->values().push_back(Value::null());
            list->values().push_back(Value::null());
        }
        return Value::object(list);
    });

    // How long a read on a socket waits before giving up, in ms; 0 for as long as it takes.
    def("socketTimeout", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const auto s = streamOf(vm, args[0]);
        const int64_t limit = std::max<int64_t>(vm.toInt(args[1], "time limit"), 0);
        if (s->channel != nullptr) s->channel->setReadTimeout(limit);
        else setReadTimeout(*s, limit);
        return Value::null();
    });

    // No more will be written on this socket: the other side reads the end.
    def("shutdownOutput", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto s = streamOf(vm, args[0]);
        if (s->channel != nullptr) s->channel->shutdownOutput();
        else shutdownOutput(*s);
        return Value::null();
    });

    // ---- A sandbox's network (M6.6.2 step 5); only in one ----

    // A connection to `host`:`port` in a sandbox: [this program's end, the far side's end].
    def("sandboxSocketPair", 2, 2, [](VM& vm, int, Value* args) -> Value {
        if (!vm.currentHost().isSandbox()) throw vm.runtimeError(Thrown::IllegalStateException, "a sandbox's connection is made in a sandbox");
        static std::atomic<int> nextPort{50000};
        auto link = std::make_shared<SocketLink>();
        link->host = vm.currentHostShared();
        const std::string host = vm.toStr(args[0], "host");
        const int port = static_cast<int>(vm.toInt(args[1], "port"));
        const int local = nextPort++;
        auto ours = std::make_shared<Stream>(Stream::Kind::Socket);
        ours->channel = std::make_shared<SocketEnd>(link, 0, host, port, local);
        auto theirs = std::make_shared<Stream>(Stream::Kind::Socket);
        theirs->channel = std::make_shared<SocketEnd>(link, 1, "10.0.0.2", local, port);
        return Value::object(newList(vm, {Value::integer(keep(ours)), Value::integer(keep(theirs))}));
    });
    // A port a program listens on in a sandbox: a Listener stream.
    def("sandboxListen", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (!vm.currentHost().isSandbox()) throw vm.runtimeError(Thrown::IllegalStateException, "a sandbox's port is made in a sandbox");
        auto made = std::make_shared<Stream>(Stream::Kind::Listener);
        made->listener = std::make_shared<SandboxListener>();
        made->listener->port = static_cast<int>(vm.toInt(args[0], "port"));
        return Value::integer(keep(made));
    });
    // A connection made to a port the program listens on: the end of what connects; the other is
    // the next accept()'s.
    def("sandboxDial", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const auto listening = streamOf(vm, args[0]);
        if (listening->listener == nullptr) throw vm.runtimeError(Thrown::IllegalArgumentException, "not a sandbox's port");
        SandboxListener& l = *listening->listener;
        auto link = std::make_shared<SocketLink>();
        link->host = vm.currentHostShared();
        static std::atomic<int> nextPort{60000};
        const int from = nextPort++;
        auto ours = std::make_shared<Stream>(Stream::Kind::Socket);
        ours->channel = std::make_shared<SocketEnd>(link, 0, "127.0.0.1", l.port, from);
        auto served = std::make_shared<Stream>(Stream::Kind::Socket);
        served->channel = std::make_shared<SocketEnd>(link, 1, "10.0.0.2", from, l.port);
        {
            std::lock_guard<std::mutex> hold(l.mutex);
            if (l.closed) throw vm.errorOf("IOException", "cannot connect to port " + std::to_string(l.port) + ": the connection was refused");
            l.waiting.push_back(served);
            l.changed.notify_all();
        }
        return Value::integer(keep(ours));
    });
    // The connection is reset: what either end does with it next fails.
    def("sandboxReset", 1, 1, [](VM& vm, int, Value* args) -> Value {
        if (auto end = std::dynamic_pointer_cast<SocketEnd>(streamOf(vm, args[0])->channel)) end->resetLink();
        return Value::null();
    });
    // This end waits for ever, until the connection is closed or reset: a read of the other's
    // with a time limit is up at once.
    def("sandboxSocketHang", 1, 1, [](VM& vm, int, Value* args) -> Value {
        auto end = std::dynamic_pointer_cast<SocketEnd>(streamOf(vm, args[0])->channel);
        if (end == nullptr) return Value::null();
        return vm.blocking([end] { end->hang(); }, [](VM&) -> Value { return Value::null(); });
    });

    vm.defineGlobal("__io", Value::object(io));
}

}  // namespace astra
