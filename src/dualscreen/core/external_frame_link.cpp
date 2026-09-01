#include "core/external_frame_link.hpp"

#include <cstring>

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ds {
namespace {

// Message framing on the socket: a kind and a length, then that many bytes.
// The same shape the network protocol uses, for the same reason -- a stream
// carries no boundaries of its own.
constexpr std::size_t kHeaderBytes = 5;

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

uint16_t read_u16(const uint8_t* data) {
    return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8));
}

// The pad, as 17 bytes. Written out here rather than memcpy'd from the struct
// so that the two ends agree on the bytes rather than on a compiler's padding.
constexpr std::size_t kInputBytes = 17;

void put_input(std::vector<uint8_t>& out, const ExternalInput& input) {
    put_u32(out, input.buttons);
    put_u16(out, static_cast<uint16_t>(input.left_x));
    put_u16(out, static_cast<uint16_t>(input.left_y));
    put_u16(out, static_cast<uint16_t>(input.right_x));
    put_u16(out, static_cast<uint16_t>(input.right_y));
    out.push_back(input.touching ? 1u : 0u);
    put_u16(out, input.touch_x);
    put_u16(out, input.touch_y);
}

ExternalInput read_input(const uint8_t* data) {
    ExternalInput input;
    input.buttons = read_u32(data);
    input.left_x = static_cast<int16_t>(read_u16(data + 4));
    input.left_y = static_cast<int16_t>(read_u16(data + 6));
    input.right_x = static_cast<int16_t>(read_u16(data + 8));
    input.right_y = static_cast<int16_t>(read_u16(data + 10));
    input.touching = data[12] != 0;
    input.touch_x = read_u16(data + 13);
    input.touch_y = read_u16(data + 15);
    return input;
}

void frame_message(ExternalControl kind, const std::vector<uint8_t>& body,
                   std::vector<uint8_t>& out) {
    out.clear();
    out.push_back(static_cast<uint8_t>(kind));
    put_u32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

// Waits for the socket to accept more bytes, the write-side twin of
// wait_readable below. Bounded for the same reason wait_readable is: a
// consumer that has genuinely stopped draining should fail this send rather
// than hang whoever called it forever.
bool wait_writable(int fd, int milliseconds) {
    pollfd waiting{};
    waiting.fd = fd;
    waiting.events = POLLOUT;
    int ready = 0;
    do {
        ready = ::poll(&waiting, 1, milliseconds);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

bool write_all(int fd, const uint8_t* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t wrote = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (wrote > 0) {
            sent += static_cast<std::size_t>(wrote);
            continue;
        }
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The kernel's send buffer is full, not gone -- wait for room
            // rather than spinning a whole CPU core retrying send() until
            // there is some, which is what this did before. That spin was
            // self-defeating on a single machine running both ends of the
            // link: it stole CPU time from the very process that would have
            // drained the socket and freed the buffer, which made the
            // backlog worse rather than riding it out, and did so while
            // holding DualscreenSession's own lock -- which is why a single
            // slow moment here was able to stall the render thread too.
            if (!wait_writable(fd, 100)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

// The producer's twin of write_all for Frame and Audio: messages that arrive
// continuously, where the next one coming right behind makes a dropped one
// unnoticeable, but waiting for one blocks whichever thread is producing the
// game's picture and sound. That thread is Cemu's own render loop, so a slow
// consumer must never be able to hold it up -- the whole reason the shared
// memory this same call announces was designed to never wait, back in
// VulkanRendererDualscreen.cpp. A blocking send() here would have undone
// that even after the busy-spin above was fixed to a proper wait, since a
// bounded wait is still a wait sitting on the same thread.
//
// Only a message that has not sent any bytes yet is safe to drop outright:
// these are framed with a length prefix, so abandoning one partway through
// would leave the consumer's parser expecting bytes that are never coming,
// corrupting every message after it for the rest of the session. A write
// already in progress is therefore still finished, bounded the same way
// write_all is, rather than left to tear the stream.
//
// `out_sent`, when given, says whether this actually reached the wire --
// distinct from the return value, which only reports whether the connection
// is still alive. A caller with its own "only send what changed" state (see
// send_input()) needs this to tell a genuine delivery from a drop; otherwise
// a dropped update would be mistaken for a delivered one and never retried.
bool write_best_effort(int fd, const uint8_t* data, std::size_t size, bool* out_sent = nullptr) {
    if (out_sent != nullptr) *out_sent = false;
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t wrote = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (wrote > 0) {
            sent += static_cast<std::size_t>(wrote);
            continue;
        }
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (sent == 0) {
                return true;  // nothing committed to the stream yet: drop it and move on
            }
            if (!wait_writable(fd, 100)) {
                return false;
            }
            continue;
        }
        return false;
    }
    if (out_sent != nullptr) *out_sent = true;
    return true;
}

// Reads whatever is there without waiting. False means the far end has gone.
bool drain_socket(int fd, std::vector<uint8_t>& inbox, bool& closed) {
    closed = false;
    uint8_t buffer[16384];
    while (true) {
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (got > 0) {
            inbox.insert(inbox.end(), buffer, buffer + got);
            continue;
        }
        if (got == 0) {
            closed = true;
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
}

bool take_message(std::vector<uint8_t>& inbox, ExternalControl& kind,
                  std::vector<uint8_t>& body) {
    if (inbox.size() < kHeaderBytes) {
        return false;
    }
    const uint32_t length = read_u32(inbox.data() + 1);
    if (inbox.size() < kHeaderBytes + length) {
        return false;
    }
    kind = static_cast<ExternalControl>(inbox[0]);
    body.assign(inbox.begin() + kHeaderBytes, inbox.begin() + kHeaderBytes + length);
    inbox.erase(inbox.begin(), inbox.begin() + kHeaderBytes + length);
    return true;
}

// The shared memory travels as a file descriptor rather than a name. Nothing is
// left in the filesystem for a crash to strand, and two sessions on one machine
// cannot find each other's buffers.
bool send_fd(int socket_fd, int payload_fd) {
    char body = 'M';
    iovec io{};
    io.iov_base = &body;
    io.iov_len = 1;

    char control[CMSG_SPACE(sizeof(int))] = {0};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &payload_fd, sizeof(int));

    return ::sendmsg(socket_fd, &message, MSG_NOSIGNAL) == 1;
}

// Waits, but not for ever: a consumer that died between listening and accepting
// would otherwise leave the producer hanging on a handshake nobody will finish.
bool wait_readable(int fd, int milliseconds) {
    pollfd waiting{};
    waiting.fd = fd;
    waiting.events = POLLIN;
    int ready = 0;
    do {
        ready = ::poll(&waiting, 1, milliseconds);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

bool receive_fd(int socket_fd, int& out_fd) {
    if (!wait_readable(socket_fd, 5000)) {
        return false;
    }
    char body = 0;
    iovec io{};
    io.iov_base = &body;
    io.iov_len = 1;

    char control[CMSG_SPACE(sizeof(int))] = {0};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    ssize_t got = 0;
    do {
        got = ::recvmsg(socket_fd, &message, 0);
    } while (got < 0 && errno == EINTR);
    if (got != 1) {
        return false;
    }
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
            std::memcpy(&out_fd, CMSG_DATA(header), sizeof(int));
            return true;
        }
    }
    return false;
}

bool bind_unix(int fd, const std::string& path, std::string* error) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(address.sun_path)) {
        if (error != nullptr) *error = "that socket path is too long: " + path;
        return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
}

}  // namespace

// --- consumer ---------------------------------------------------------------

ExternalFrameLink::~ExternalFrameLink() { close(); }

std::string ExternalFrameLink::default_socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] != '\0') {
        return std::string(runtime) + "/dualscreen-external.sock";
    }
    return "/tmp/dualscreen-external-" + std::to_string(::getuid()) + ".sock";
}

bool ExternalFrameLink::listen(const std::string& socket_path, std::string* error) {
    close();
    socket_path_ = socket_path;

    memory_fd_ = ::memfd_create("dualscreen-frames", MFD_CLOEXEC);
    if (memory_fd_ < 0) {
        if (error != nullptr) *error = std::string("no shared memory: ") + std::strerror(errno);
        return false;
    }
    const std::size_t bytes = ExternalSharedHeader::total_bytes();
    if (::ftruncate(memory_fd_, static_cast<off_t>(bytes)) != 0) {
        if (error != nullptr) {
            *error = std::string("could not size the shared memory: ") + std::strerror(errno);
        }
        close();
        return false;
    }
    memory_ = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memory_fd_, 0);
    if (memory_ == MAP_FAILED) {
        memory_ = nullptr;
        if (error != nullptr) {
            *error = std::string("could not map the shared memory: ") + std::strerror(errno);
        }
        close();
        return false;
    }
    // Placement-new rather than memset: the atomics in here have to be
    // constructed, and a zeroed buffer is only accidentally a valid one.
    new (memory_) ExternalSharedHeader();
    header()->magic.store(ExternalSharedHeader::kMagic, std::memory_order_relaxed);
    header()->version.store(ExternalSharedHeader::kVersion, std::memory_order_release);

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) {
        if (error != nullptr) *error = std::string("no socket: ") + std::strerror(errno);
        close();
        return false;
    }
    // A path left behind by a crash is not a reason to refuse to start.
    ::unlink(socket_path_.c_str());
    if (!bind_unix(listen_fd_, socket_path_, error)) {
        if (error != nullptr && error->empty()) {
            *error = "could not claim " + socket_path_ + ": " + std::strerror(errno);
        }
        close();
        return false;
    }
    if (::listen(listen_fd_, 1) != 0) {
        if (error != nullptr) *error = std::string("could not listen: ") + std::strerror(errno);
        close();
        return false;
    }
    return true;
}

void ExternalFrameLink::close() {
    if (peer_fd_ >= 0) {
        ::close(peer_fd_);
        peer_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (memory_ != nullptr) {
        ::munmap(memory_, ExternalSharedHeader::total_bytes());
        memory_ = nullptr;
    }
    if (memory_fd_ >= 0) {
        ::close(memory_fd_);
        memory_fd_ = -1;
    }
    if (!socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
        socket_path_.clear();
    }
    have_info_ = false;
    info_ = ExternalStreamInfo();
    inbox_.clear();
    audio_[0].clear();
    audio_[1].clear();
    pending_[0] = Pending();
    pending_[1] = Pending();
    // A pad is only sent when it differs from the last one sent, so what was
    // sent to a game that has gone has to be forgotten with it -- otherwise the
    // next game's first pad is silently withheld for matching a session it was
    // never part of.
    sent_input_ = ExternalInput();
    have_sent_input_ = false;
}

bool ExternalFrameLink::accept_peer(std::string* error) {
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return true;  // nobody yet, which is not a failure
        }
        if (error != nullptr) *error = std::string("could not accept: ") + std::strerror(errno);
        return false;
    }
    peer_fd_ = fd;
    // The memory goes over first, before anything is said about it, so a
    // producer never has to handle a frame announcement for memory it cannot
    // see yet.
    if (!send_fd(peer_fd_, memory_fd_)) {
        if (error != nullptr) *error = "could not hand over the shared memory";
        ::close(peer_fd_);
        peer_fd_ = -1;
        return false;
    }
    return true;
}

bool ExternalFrameLink::poll(std::string* error) {
    if (listen_fd_ < 0) {
        if (error != nullptr) *error = "nothing is listening";
        return false;
    }
    if (peer_fd_ < 0) {
        return accept_peer(error);
    }
    return read_messages(error);
}

bool ExternalFrameLink::read_messages(std::string* error) {
    bool closed = false;
    if (!drain_socket(peer_fd_, inbox_, closed)) {
        if (error != nullptr) *error = std::string("the game's link failed: ") + std::strerror(errno);
        return false;
    }

    ExternalControl kind = ExternalControl::Hello;
    std::vector<uint8_t> body;
    while (take_message(inbox_, kind, body)) {
        if (!handle_message(kind, body, error)) {
            return false;
        }
    }
    if (closed) {
        // The ordinary way a session ends: the game was closed.
        if (error != nullptr) *error = "the game closed";
        return false;
    }
    return true;
}

bool ExternalFrameLink::handle_message(ExternalControl kind, const std::vector<uint8_t>& body,
                                       std::string* error) {
    switch (kind) {
        case ExternalControl::Hello: {
            if (body.size() < 24) {
                if (error != nullptr) *error = "the game introduced itself with nonsense";
                return false;
            }
            info_.top_width = static_cast<int>(read_u32(body.data()));
            info_.top_height = static_cast<int>(read_u32(body.data() + 4));
            info_.bottom_width = static_cast<int>(read_u32(body.data() + 8));
            info_.bottom_height = static_cast<int>(read_u32(body.data() + 12));
            info_.audio_sample_rate = static_cast<int>(read_u32(body.data() + 16));
            info_.frames_per_second = static_cast<double>(read_u32(body.data() + 20)) / 1000.0;
            const bool sane = info_.top_width > 0 && info_.top_height > 0 &&
                              info_.bottom_width > 0 && info_.bottom_height > 0 &&
                              info_.top_width <= ExternalSharedHeader::kMaxWidth &&
                              info_.top_height <= ExternalSharedHeader::kMaxHeight &&
                              info_.bottom_width <= ExternalSharedHeader::kMaxWidth &&
                              info_.bottom_height <= ExternalSharedHeader::kMaxHeight;
            if (!sane) {
                if (error != nullptr) {
                    *error = "the game offered screens this cannot carry: " +
                             std::to_string(info_.top_width) + "x" +
                             std::to_string(info_.top_height) + " and " +
                             std::to_string(info_.bottom_width) + "x" +
                             std::to_string(info_.bottom_height);
                }
                return false;
            }
            have_info_ = true;
            return true;
        }
        case ExternalControl::Frame: {
            if (body.size() < 13) {
                return true;  // nothing usable, and not worth ending a session over
            }
            const int screen_index = body[0] == 0 ? 0 : 1;
            const int slot = static_cast<int>(read_u32(body.data() + 1));
            if (slot < 0 || slot >= ExternalSharedHeader::kSlotsPerScreen) {
                return true;
            }
            const uint64_t serial = static_cast<uint64_t>(read_u32(body.data() + 5)) |
                                    (static_cast<uint64_t>(read_u32(body.data() + 9)) << 32);
            // Only the newest is kept: this is a live picture and an older
            // frame has nothing to offer once a newer one exists.
            if (serial >= pending_[screen_index].serial) {
                pending_[screen_index].slot = slot;
                pending_[screen_index].serial = serial;
            }
            return true;
        }
        case ExternalControl::Audio: {
            if (body.empty()) {
                return true;
            }
            const int index = body[0] == 0 ? 0 : 1;
            const std::size_t samples = (body.size() - 1) / sizeof(int16_t);
            const auto* data = reinterpret_cast<const int16_t*>(body.data() + 1);
            auto& audio = audio_[index];
            audio.insert(audio.end(), data, data + samples);
            // A consumer that stopped taking audio must not make this grow
            // without bound. A second of it is far more than any jitter.
            const std::size_t cap =
                static_cast<std::size_t>(std::max(8000, info_.audio_sample_rate)) * 2;
            if (audio.size() > cap) {
                audio.erase(audio.begin(), audio.end() - static_cast<std::ptrdiff_t>(cap));
            }
            return true;
        }
        case ExternalControl::Shutdown:
            if (error != nullptr) *error = "the game closed";
            return false;
        case ExternalControl::Reset:
        case ExternalControl::Input:
        case ExternalControl::RequestScreen:
            // Ours to send, not to receive.
            return true;
    }
    return true;
}

bool ExternalFrameLink::take_frame(ScreenId screen, ExternalFrame& out) {
    out.pixels.clear();
    out.width = 0;
    out.height = 0;
    const int index = ExternalSharedHeader::screen_index(screen);
    if (memory_ == nullptr || pending_[index].slot < 0) {
        return false;
    }
    const int slot = pending_[index].slot;
    pending_[index].slot = -1;  // taken; nothing new until the next announcement

    ExternalSlotState& state = header()->slots[index][slot];
    // Twice around at most. A producer overwriting the slot we want is a lost
    // frame rather than something to wait for: by the time it settles there is
    // a newer one anyway.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint64_t before = state.sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;  // being written right now
        }
        const int width = static_cast<int>(state.width.load(std::memory_order_relaxed));
        const int height = static_cast<int>(state.height.load(std::memory_order_relaxed));
        if (width <= 0 || height <= 0 || width > ExternalSharedHeader::kMaxWidth ||
            height > ExternalSharedHeader::kMaxHeight) {
            return false;
        }
        const auto* source = reinterpret_cast<const uint32_t*>(
            static_cast<uint8_t*>(memory_) + ExternalSharedHeader::slot_offset(index, slot));
        out.pixels.assign(source, source + static_cast<std::size_t>(width) * height);

        const uint64_t after = state.sequence.load(std::memory_order_acquire);
        if (before == after) {
            out.width = width;
            out.height = height;
            out.serial = state.serial.load(std::memory_order_relaxed);
            return true;
        }
    }
    out.pixels.clear();
    return false;
}

void ExternalFrameLink::take_audio(ScreenId screen, std::vector<int16_t>& out) {
    auto& audio = audio_[ExternalSharedHeader::screen_index(screen)];
    out.swap(audio);
    audio.clear();
}

bool ExternalFrameLink::send_control(ExternalControl kind, const std::vector<uint8_t>& body) {
    if (peer_fd_ < 0) {
        return false;
    }
    std::vector<uint8_t> message;
    frame_message(kind, body, message);
    return write_all(peer_fd_, message.data(), message.size());
}

// send_input()'s own twin of write_best_effort, for the same reason that one
// exists: this app's poll loop is the one thread that both calls this (every
// frame the pad has moved) and pumps the audio taken off this same link out
// to a speaker or onward to a companion (see ExternalScreenSource's audio
// callback). A pad update blocking on a full send buffer would stall that
// audio right along with it -- heard as a cutout timed to whenever a stick or
// button actually moved, which is exactly what a blocking write_all() here
// used to risk. Unlike Frame and Audio, a dropped pad update cannot just be
// left dropped: `out_sent` is what lets send_input() below tell a real
// delivery from a drop, so a drop is retried rather than mistaken for sent.
bool ExternalFrameLink::send_control_best_effort(ExternalControl kind,
                                                  const std::vector<uint8_t>& body,
                                                  bool& out_sent) {
    out_sent = false;
    if (peer_fd_ < 0) {
        return false;
    }
    std::vector<uint8_t> message;
    frame_message(kind, body, message);
    return write_best_effort(peer_fd_, message.data(), message.size(), &out_sent);
}

void ExternalFrameLink::request_reset() { send_control(ExternalControl::Reset, {}); }

void ExternalFrameLink::send_input(const ExternalInput& input) {
    // Only when it changed. A pad that is not being touched is the common case
    // and it would otherwise cost a message every frame for nothing.
    if (have_sent_input_ && input == sent_input_) {
        return;
    }
    std::vector<uint8_t> body;
    body.reserve(kInputBytes);
    put_input(body, input);
    bool sent = false;
    send_control_best_effort(ExternalControl::Input, body, sent);
    // Only marked sent when it actually reached the wire -- a drop leaves
    // sent_input_ stale, so the unchanged `input` above still reads as
    // different next call and gets tried again rather than lost until the
    // pad happens to move again.
    if (sent) {
        sent_input_ = input;
        have_sent_input_ = true;
    }
}

void ExternalFrameLink::request_screen(std::optional<ScreenId> screen) {
    std::vector<uint8_t> body;
    // 0/1 for a screen, 2 for none -- ScreenId has no "none" of its own (see
    // screen_id.hpp), so that has to be spelled out here rather than cast.
    if (!screen.has_value()) {
        body.push_back(2u);
    } else {
        body.push_back(*screen == ScreenId::Top ? 0u : 1u);
    }
    send_control(ExternalControl::RequestScreen, body);
}

// --- producer ---------------------------------------------------------------

ExternalFrameProducer::~ExternalFrameProducer() { close(); }

bool ExternalFrameProducer::connect(const std::string& socket_path,
                                    const ExternalStreamInfo& info, std::string* error) {
    close();
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        if (error != nullptr) *error = std::string("no socket: ") + std::strerror(errno);
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() + 1 > sizeof(address.sun_path)) {
        if (error != nullptr) *error = "that socket path is too long";
        close();
        return false;
    }
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error != nullptr) {
            *error = "nothing is listening on " + socket_path + ": " + std::strerror(errno);
        }
        close();
        return false;
    }
    if (!receive_fd(fd_, memory_fd_)) {
        if (error != nullptr) *error = "the far end did not hand over any shared memory";
        close();
        return false;
    }
    memory_ = ::mmap(nullptr, ExternalSharedHeader::total_bytes(), PROT_READ | PROT_WRITE,
                     MAP_SHARED, memory_fd_, 0);
    if (memory_ == MAP_FAILED) {
        memory_ = nullptr;
        if (error != nullptr) {
            *error = std::string("could not map the shared memory: ") + std::strerror(errno);
        }
        close();
        return false;
    }
    if (header()->magic.load(std::memory_order_acquire) != ExternalSharedHeader::kMagic ||
        header()->version.load(std::memory_order_acquire) != ExternalSharedHeader::kVersion) {
        if (error != nullptr) *error = "the far end is speaking a different version of this";
        close();
        return false;
    }

    std::vector<uint8_t> body;
    put_u32(body, static_cast<uint32_t>(info.top_width));
    put_u32(body, static_cast<uint32_t>(info.top_height));
    put_u32(body, static_cast<uint32_t>(info.bottom_width));
    put_u32(body, static_cast<uint32_t>(info.bottom_height));
    put_u32(body, static_cast<uint32_t>(info.audio_sample_rate));
    put_u32(body, static_cast<uint32_t>(info.frames_per_second * 1000.0 + 0.5));
    std::vector<uint8_t> message;
    frame_message(ExternalControl::Hello, body, message);
    if (!write_all(fd_, message.data(), message.size())) {
        if (error != nullptr) *error = "could not introduce itself";
        close();
        return false;
    }
    // Non-blocking only after the handshake, which is the one part that has to
    // happen in order.
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

void ExternalFrameProducer::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (memory_ != nullptr) {
        ::munmap(memory_, ExternalSharedHeader::total_bytes());
        memory_ = nullptr;
    }
    if (memory_fd_ >= 0) {
        ::close(memory_fd_);
        memory_fd_ = -1;
    }
    next_slot_[0] = 0;
    next_slot_[1] = 0;
    inbox_.clear();
    // Same reason as the other end: whether a pad has ever arrived is a fact
    // about one session, and a fork reads it to decide whether to keep using
    // its own controllers.
    input_ = ExternalInput();
    have_input_ = false;
}

bool ExternalFrameProducer::publish(ScreenId screen, uint64_t serial, const uint32_t* pixels,
                                    int width, int height, int stride_pixels,
                                    std::string* error) {
    if (fd_ < 0 || memory_ == nullptr || pixels == nullptr) {
        if (error != nullptr) *error = "not connected";
        return false;
    }
    if (width <= 0 || height <= 0 || width > ExternalSharedHeader::kMaxWidth ||
        height > ExternalSharedHeader::kMaxHeight) {
        if (error != nullptr) {
            *error = "a screen this size cannot be carried: " + std::to_string(width) + "x" +
                     std::to_string(height);
        }
        return false;
    }
    const int index = ExternalSharedHeader::screen_index(screen);
    const int slot = next_slot_[index];
    next_slot_[index] = (slot + 1) % ExternalSharedHeader::kSlotsPerScreen;

    ExternalSlotState& state = header()->slots[index][slot];
    // Odd while writing, even when settled. A reader that sees either an odd
    // number or a different one afterwards knows it caught this mid-write.
    const uint64_t sequence = state.sequence.load(std::memory_order_relaxed);
    state.sequence.store(sequence + 1, std::memory_order_release);

    auto* destination = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(memory_) +
                                                    ExternalSharedHeader::slot_offset(index, slot));
    if (stride_pixels == width) {
        std::memcpy(destination, pixels, static_cast<std::size_t>(width) * height * sizeof(uint32_t));
    } else {
        for (int row = 0; row < height; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * width,
                        pixels + static_cast<std::size_t>(row) * stride_pixels,
                        static_cast<std::size_t>(width) * sizeof(uint32_t));
        }
    }
    state.width.store(static_cast<uint32_t>(width), std::memory_order_relaxed);
    state.height.store(static_cast<uint32_t>(height), std::memory_order_relaxed);
    state.serial.store(serial, std::memory_order_relaxed);
    state.sequence.store(sequence + 2, std::memory_order_release);

    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(index));
    put_u32(body, static_cast<uint32_t>(slot));
    put_u32(body, static_cast<uint32_t>(serial & 0xFFFFFFFFu));
    put_u32(body, static_cast<uint32_t>((serial >> 32) & 0xFFFFFFFFu));
    std::vector<uint8_t> message;
    frame_message(ExternalControl::Frame, body, message);
    if (!write_best_effort(fd_, message.data(), message.size())) {
        if (error != nullptr) *error = "the far end has gone";
        return false;
    }
    return true;
}

bool ExternalFrameProducer::send_audio(ScreenId screen, const int16_t* interleaved_stereo,
                                       std::size_t frames, std::string* error) {
    if (fd_ < 0 || interleaved_stereo == nullptr || frames == 0) {
        return true;
    }
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(ExternalSharedHeader::screen_index(screen)));
    const auto* bytes = reinterpret_cast<const uint8_t*>(interleaved_stereo);
    body.insert(body.end(), bytes, bytes + frames * 2 * sizeof(int16_t));
    std::vector<uint8_t> message;
    frame_message(ExternalControl::Audio, body, message);
    if (!write_best_effort(fd_, message.data(), message.size())) {
        if (error != nullptr) *error = "the far end has gone";
        return false;
    }
    return true;
}

bool ExternalFrameProducer::poll(bool& out_reset_requested, std::string* error) {
    out_reset_requested = false;
    if (fd_ < 0) {
        if (error != nullptr) *error = "not connected";
        return false;
    }
    bool closed = false;
    if (!drain_socket(fd_, inbox_, closed)) {
        if (error != nullptr) *error = std::strerror(errno);
        return false;
    }
    ExternalControl kind = ExternalControl::Hello;
    std::vector<uint8_t> body;
    while (take_message(inbox_, kind, body)) {
        if (kind == ExternalControl::Reset) {
            out_reset_requested = true;
        } else if (kind == ExternalControl::Input) {
            if (body.size() >= kInputBytes) {
                input_ = read_input(body.data());
                have_input_ = true;
            }
        } else if (kind == ExternalControl::RequestScreen) {
            if (!body.empty()) {
                if (body[0] == 0u) {
                    requested_screen_ = ScreenId::Top;
                } else if (body[0] == 1u) {
                    requested_screen_ = ScreenId::Bottom;
                } else {
                    requested_screen_.reset();
                }
            }
        } else if (kind == ExternalControl::Shutdown) {
            if (error != nullptr) *error = "the far end ended the session";
            return false;
        }
    }
    if (closed) {
        if (error != nullptr) *error = "the far end has gone";
        return false;
    }
    return true;
}

}  // namespace ds

#else  // __linux__

namespace ds {

ExternalFrameLink::~ExternalFrameLink() = default;
void ExternalFrameLink::close() {}
std::string ExternalFrameLink::default_socket_path() { return std::string(); }

bool ExternalFrameLink::listen(const std::string&, std::string* error) {
    if (error != nullptr) *error = "this platform has no way to run a game beside this app";
    return false;
}
bool ExternalFrameLink::poll(std::string* error) {
    if (error != nullptr) *error = "nothing is listening";
    return false;
}
bool ExternalFrameLink::take_frame(ScreenId, ExternalFrame&) { return false; }
void ExternalFrameLink::take_audio(ScreenId, std::vector<int16_t>& out) { out.clear(); }
void ExternalFrameLink::request_reset() {}
void ExternalFrameLink::send_input(const ExternalInput&) {}
void ExternalFrameLink::request_screen(std::optional<ScreenId>) {}
bool ExternalFrameLink::accept_peer(std::string*) { return false; }
bool ExternalFrameLink::read_messages(std::string*) { return false; }
bool ExternalFrameLink::handle_message(ExternalControl, const std::vector<uint8_t>&, std::string*) {
    return false;
}
bool ExternalFrameLink::send_control(ExternalControl, const std::vector<uint8_t>&) { return false; }

ExternalFrameProducer::~ExternalFrameProducer() = default;
void ExternalFrameProducer::close() {}
bool ExternalFrameProducer::connect(const std::string&, const ExternalStreamInfo&,
                                    std::string* error) {
    if (error != nullptr) *error = "this platform has no way to hand frames to another program";
    return false;
}
bool ExternalFrameProducer::publish(ScreenId, uint64_t, const uint32_t*, int, int, int,
                                    std::string*) {
    return false;
}
bool ExternalFrameProducer::send_audio(ScreenId, const int16_t*, std::size_t, std::string*) {
    return false;
}
bool ExternalFrameProducer::poll(bool& out_reset_requested, std::string*) {
    out_reset_requested = false;
    return false;
}

}  // namespace ds

#endif
