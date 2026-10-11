#include "core/external_frame_link.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif

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

// A float goes as its own IEEE-754 bits, little-endian like the integers
// above. Exact rather than scaled into a fixed-point integer: what these
// carry is a raw sensor reading whose useful range is the sensor's business,
// and both ends of this link are built by the same compiler family for the
// same machine anyway.
void put_f32(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "a float is not four bytes here");
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

float read_f32(const uint8_t* data) {
    const uint32_t bits = read_u32(data);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The pad, as 46 bytes: 17 of buttons, sticks and touch, then 29 of how the
// thing is being held (see ExternalMotion). Written out here rather than
// memcpy'd from the struct so that the two ends agree on the bytes rather
// than on a compiler's padding.
constexpr std::size_t kInputBytes = 46;

void put_input(std::vector<uint8_t>& out, const ExternalInput& input) {
    put_u32(out, input.buttons);
    put_u16(out, static_cast<uint16_t>(input.left_x));
    put_u16(out, static_cast<uint16_t>(input.left_y));
    put_u16(out, static_cast<uint16_t>(input.right_x));
    put_u16(out, static_cast<uint16_t>(input.right_y));
    out.push_back(input.touching ? 1u : 0u);
    put_u16(out, input.touch_x);
    put_u16(out, input.touch_y);
    out.push_back(input.motion.present ? 1u : 0u);
    put_f32(out, input.motion.gyro_x);
    put_f32(out, input.motion.gyro_y);
    put_f32(out, input.motion.gyro_z);
    put_f32(out, input.motion.accel_x);
    put_f32(out, input.motion.accel_y);
    put_f32(out, input.motion.accel_z);
    put_u32(out, input.motion.sampled_us);
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
    input.motion.present = data[17] != 0;
    input.motion.gyro_x = read_f32(data + 18);
    input.motion.gyro_y = read_f32(data + 22);
    input.motion.gyro_z = read_f32(data + 26);
    input.motion.accel_x = read_f32(data + 30);
    input.motion.accel_y = read_f32(data + 34);
    input.motion.accel_z = read_f32(data + 38);
    input.motion.sampled_us = read_u32(data + 42);
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

#if defined(__ANDROID__) && __ANDROID_API__ < 30
    // Bionic only wraps it from Android 11; the kernel has had it far longer.
    memory_fd_ = static_cast<int>(::syscall(__NR_memfd_create, "dualscreen-frames", MFD_CLOEXEC));
#else
    memory_fd_ = ::memfd_create("dualscreen-frames", MFD_CLOEXEC);
#endif
    if (memory_fd_ < 0) {
        if (error != nullptr) *error = std::string("no shared memory: ") + std::strerror(errno);
        return false;
    }
    // Room for the overlay after the frames: see ExternalOverlayArea. Never
    // touched by a producer that cannot draw one, so it costs nothing there.
    const std::size_t bytes = ExternalOverlayArea::end();
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
    new (overlay_area()) ExternalOverlayArea();
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
        ::munmap(memory_, ExternalOverlayArea::end());
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
    // Same reasoning for what screen and size were last asked for: a request
    // that only matched the producer that has gone would leave the next one
    // never told what to capture at all.
    sent_screen_.reset();
    sent_max_width_ = -1;
    sent_max_height_ = -1;
    rumble_active_ = false;
    have_new_rumble_ = false;
    // Every player, for the same reason as the GamePad's pad above.
    for (int player = 0; player < kExternalPlayers; ++player) {
        sent_player_input_[player] = ExternalInput();
        sent_player_connected_[player] = false;
        have_sent_player_[player] = false;
    }
    player_rumble_.clear();
    // An overlay is shown by one producer; the next starts with none.
    next_overlay_slot_ = 0;
    overlay_slot_ = -1;
    overlay_shown_ = false;
    overlay_unsent_ = false;
    menu_key_ = false;
    sent_pointer_shown_.reset();
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
    // A fresh producer has been told nothing yet, whatever the last one was
    // told -- see close() for why these are tracked at all.
    sent_screen_.reset();
    sent_max_width_ = -1;
    sent_max_height_ = -1;
    next_overlay_slot_ = 0;
    overlay_slot_ = -1;
    overlay_shown_ = false;
    overlay_unsent_ = false;
    menu_key_ = false;
    sent_pointer_shown_.reset();
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
    if (!read_messages(error)) {
        return false;
    }
    send_overlay_state();
    return true;
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
            // A trailing word a producer built before it does not send.
            info_.features = body.size() >= 28 ? read_u32(body.data() + 24) : 0u;
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
            uint64_t captured_us = 0;
            if (body.size() >= 21) {
                captured_us = static_cast<uint64_t>(read_u32(body.data() + 13)) |
                              (static_cast<uint64_t>(read_u32(body.data() + 17)) << 32);
            }
            // Only the newest is kept: this is a live picture and an older
            // frame has nothing to offer once a newer one exists.
            FrameCounts& counts = counts_[screen_index];
            ++counts.announced;
            if (pending_[screen_index].slot >= 0) {
                ++counts.missed;  // the last one, never taken
            }
            const uint64_t now_us = external_frame_clock_us();
            if (captured_us != 0 && captured_us <= now_us) {
                ++counts.timed_announced;
                counts.announced_age_us += now_us - captured_us;
            }
            if (serial >= pending_[screen_index].serial) {
                pending_[screen_index].slot = slot;
                pending_[screen_index].serial = serial;
                pending_[screen_index].captured_us = captured_us;
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
        case ExternalControl::Rumble: {
            if (body.empty()) {
                return true;
            }
            rumble_active_ = body[0] != 0u;
            have_new_rumble_ = true;
            return true;
        }
        case ExternalControl::PlayerRumble: {
            if (body.size() < 2 || body[0] >= kExternalPlayers) {
                return true;
            }
            player_rumble_.emplace_back(static_cast<int>(body[0]), body[1] != 0u);
            // Bounded, like audio: a caller that never takes these must not
            // make them grow for ever. The newest are the ones that matter.
            if (player_rumble_.size() > 64) {
                player_rumble_.erase(player_rumble_.begin(),
                                     player_rumble_.end() - 64);
            }
            return true;
        }
        case ExternalControl::MenuKey:
            menu_key_ = true;
            return true;
        case ExternalControl::Shutdown:
            if (error != nullptr) *error = "the game closed";
            return false;
        case ExternalControl::Reset:
        case ExternalControl::Input:
        case ExternalControl::PlayerInput:
        case ExternalControl::RequestScreen:
        case ExternalControl::Overlay:
        case ExternalControl::Pointer:
            // Ours to send, not to receive.
            return true;
    }
    return true;
}

bool ExternalFrameLink::wait_for_message(int timeout_ms) const {
    const int fd = peer_fd_ >= 0 ? peer_fd_ : listen_fd_;
    if (fd < 0) {
        return false;
    }
    pollfd waiting {};
    waiting.fd = fd;
    waiting.events = POLLIN;
    return ::poll(&waiting, 1, timeout_ms) > 0;
}

ExternalFrameLink::FrameCounts ExternalFrameLink::take_frame_counts(ScreenId screen) {
    const int index = ExternalSharedHeader::screen_index(screen);
    const FrameCounts counts = counts_[index];
    counts_[index] = FrameCounts();
    return counts;
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
    const uint64_t captured_us = pending_[index].captured_us;
    const uint64_t now_us = external_frame_clock_us();
    if (captured_us != 0 && captured_us <= now_us) {
        FrameCounts& counts = counts_[index];
        const uint64_t age_us = now_us - captured_us;
        ++counts.timed_taken;
        counts.taken_age_us += age_us;
        counts.taken_age_max_us = std::max(counts.taken_age_max_us, age_us);
    }

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

bool ExternalFrameLink::take_rumble(bool& out_active) {
    if (!have_new_rumble_) {
        return false;
    }
    have_new_rumble_ = false;
    out_active = rumble_active_;
    return true;
}

bool ExternalFrameLink::take_player_rumble(int& out_player, bool& out_active) {
    if (player_rumble_.empty()) {
        return false;
    }
    out_player = player_rumble_.front().first;
    out_active = player_rumble_.front().second;
    player_rumble_.erase(player_rumble_.begin());
    return true;
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

bool ExternalFrameLink::publish_overlay(const uint8_t* rgba, int width, int height) {
    if (!overlay_supported() || memory_ == nullptr || rgba == nullptr || width <= 0 ||
        height <= 0 || width > ExternalOverlayArea::kMaxWidth ||
        height > ExternalOverlayArea::kMaxHeight) {
        return false;
    }
    // Never the slot last announced, which the producer may be copying now.
    const int slot = next_overlay_slot_;
    next_overlay_slot_ = (slot + 1) % ExternalOverlayArea::kSlots;

    ExternalSlotState& state = overlay_area()->slots[slot];
    const uint64_t sequence = state.sequence.load(std::memory_order_relaxed);
    state.sequence.store(sequence + 1, std::memory_order_release);
    std::memcpy(static_cast<uint8_t*>(memory_) + ExternalOverlayArea::slot_offset(slot), rgba,
                static_cast<std::size_t>(width) * height * 4);
    ++overlay_serial_;
    state.width.store(static_cast<uint32_t>(width), std::memory_order_relaxed);
    state.height.store(static_cast<uint32_t>(height), std::memory_order_relaxed);
    state.serial.store(overlay_serial_, std::memory_order_relaxed);
    state.sequence.store(sequence + 2, std::memory_order_release);

    overlay_slot_ = slot;
    overlay_shown_ = true;
    overlay_unsent_ = true;
    send_overlay_state();
    return true;
}

void ExternalFrameLink::hide_overlay() {
    if (!overlay_shown_) {
        return;
    }
    overlay_shown_ = false;
    overlay_unsent_ = true;
    send_overlay_state();
}

// Never blocks, like send_input(), and for its reason; only the newest state
// matters, so one that could not go is simply tried again by the next poll().
void ExternalFrameLink::send_overlay_state() {
    if (!overlay_unsent_) {
        return;
    }
    if (!overlay_supported()) {
        overlay_unsent_ = false;
        return;
    }
    std::vector<uint8_t> body;
    body.push_back(overlay_shown_ ? 1u : 0u);
    body.push_back(static_cast<uint8_t>(std::max(0, overlay_slot_)));
    put_u32(body, static_cast<uint32_t>(overlay_serial_ & 0xFFFFFFFFu));
    put_u32(body, static_cast<uint32_t>((overlay_serial_ >> 32) & 0xFFFFFFFFu));
    bool sent = false;
    send_control_best_effort(ExternalControl::Overlay, body, sent);
    if (sent) {
        overlay_unsent_ = false;
    }
}

void ExternalFrameLink::set_pointer_shown(bool shown) {
    if (sent_pointer_shown_ == shown) {
        return;
    }
    std::vector<uint8_t> body;
    body.push_back(shown ? 1u : 0u);
    bool sent = false;
    send_control_best_effort(ExternalControl::Pointer, body, sent);
    if (sent) {
        sent_pointer_shown_ = shown;
    }
}

bool ExternalFrameLink::take_menu_key() {
    const bool pressed = menu_key_;
    menu_key_ = false;
    return pressed;
}

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

void ExternalFrameLink::send_player_input(int player, bool connected, const ExternalInput& input) {
    if (player < 0 || player >= kExternalPlayers) {
        return;
    }
    // A player nothing feeds has no pad worth describing; what matters is
    // only that it is gone, so its state is compared as empty.
    const ExternalInput& state = connected ? input : ExternalInput();
    if (have_sent_player_[player] && sent_player_connected_[player] == connected &&
        sent_player_input_[player] == state) {
        return;
    }
    std::vector<uint8_t> body;
    body.reserve(2 + kInputBytes);
    body.push_back(static_cast<uint8_t>(player));
    body.push_back(connected ? 1u : 0u);
    put_input(body, state);
    bool sent = false;
    send_control_best_effort(ExternalControl::PlayerInput, body, sent);
    // Recorded only once it is really on the wire -- see send_input().
    if (sent) {
        sent_player_input_[player] = state;
        sent_player_connected_[player] = connected;
        have_sent_player_[player] = true;
    }
}

void ExternalFrameLink::request_screen(std::optional<ScreenId> screen, int max_width,
                                       int max_height, std::optional<ScreenId> shown) {
    // Clamped rather than trusted: these come from a companion device's own
    // reported window size, and a negative or absurd one must not turn into a
    // capture size the producer then tries to allocate for.
    max_width = std::max(0, std::min(max_width, ExternalSharedHeader::kMaxWidth));
    max_height = std::max(0, std::min(max_height, ExternalSharedHeader::kMaxHeight));
    if (screen == sent_screen_ && max_width == sent_max_width_ &&
        max_height == sent_max_height_ && have_sent_shown_ && shown == sent_shown_) {
        return;  // Nothing has changed; see sent_screen_.
    }
    std::vector<uint8_t> body;
    // 0/1 for a screen, 2 for none -- ScreenId has no "none" of its own (see
    // screen_id.hpp), so that has to be spelled out here rather than cast.
    if (!screen.has_value()) {
        body.push_back(2u);
    } else {
        body.push_back(*screen == ScreenId::Top ? 0u : 1u);
    }
    // Appended after the byte that used to be the whole message, so a
    // producer built before this existed reads the screen and ignores the
    // rest rather than failing to parse it at all.
    const uint16_t width = static_cast<uint16_t>(max_width);
    const uint16_t height = static_cast<uint16_t>(max_height);
    body.push_back(static_cast<uint8_t>(width & 0xFFu));
    body.push_back(static_cast<uint8_t>((width >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(height & 0xFFu));
    body.push_back(static_cast<uint8_t>((height >> 8) & 0xFFu));
    // Appended in its turn, the same way and for the same reason: a producer
    // built before this existed stops reading after the size and falls back
    // to choosing its own screen, which is what it did anyway.
    body.push_back(!shown.has_value() ? 2u : (*shown == ScreenId::Top ? 0u : 1u));
    if (!send_control(ExternalControl::RequestScreen, body)) {
        return;  // Not recorded as sent, so the next call tries again.
    }
    sent_screen_ = screen;
    sent_max_width_ = max_width;
    sent_max_height_ = max_height;
    sent_shown_ = shown;
    have_sent_shown_ = true;
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
    // The overlay too when the consumer made room for it -- an older one's
    // memory ends with the frames, and is mapped exactly as it always was.
    struct stat described {};
    const bool roomy = ::fstat(memory_fd_, &described) == 0 && described.st_size > 0 &&
                       static_cast<std::size_t>(described.st_size) >= ExternalOverlayArea::end();
    memory_bytes_ = roomy ? ExternalOverlayArea::end() : ExternalSharedHeader::total_bytes();
    memory_ = ::mmap(nullptr, memory_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, memory_fd_, 0);
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
    // An overlay is only offered when there is memory to carry it.
    const uint32_t features =
        overlay_available() ? info.features : (info.features & ~kExternalFeatureOverlay);
    put_u32(body, features);
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
        ::munmap(memory_, memory_bytes_);
        memory_ = nullptr;
    }
    memory_bytes_ = 0;
    if (memory_fd_ >= 0) {
        ::close(memory_fd_);
        memory_fd_ = -1;
    }
    next_slot_[0] = 0;
    next_slot_[1] = 0;
    overlay_visible_ = false;
    overlay_slot_ = -1;
    overlay_serial_ = 0;
    overlay_taken_serial_ = 0;
    pointer_shown_ = true;
    inbox_.clear();
    // Same reason as the other end: whether a pad has ever arrived is a fact
    // about one session, and a fork reads it to decide whether to keep using
    // its own controllers.
    input_ = ExternalInput();
    have_input_ = false;
    // Same reasoning again: a motor that was on for a session now over is not
    // a fact about the next one, and a fresh connection must be free to say
    // "on" again from nothing rather than have that be a no-op because this
    // remembered it from before.
    sent_rumble_ = false;
    have_sent_rumble_ = false;
    // And every player, on both counts.
    for (int player = 0; player < kExternalPlayers; ++player) {
        player_input_[player] = ExternalInput();
        player_connected_[player] = false;
        sent_player_rumble_[player] = false;
        have_sent_player_rumble_[player] = false;
    }
}

bool ExternalFrameProducer::publish(ScreenId screen, uint64_t serial, const uint32_t* pixels,
                                    int width, int height, int stride_pixels,
                                    std::string* error, uint64_t captured_us) {
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
    if (captured_us == 0) {
        captured_us = external_frame_clock_us();
    }
    put_u32(body, static_cast<uint32_t>(captured_us & 0xFFFFFFFFu));
    put_u32(body, static_cast<uint32_t>((captured_us >> 32) & 0xFFFFFFFFu));
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

void ExternalFrameProducer::send_rumble(bool active) {
    if (fd_ < 0) {
        return;
    }
    if (have_sent_rumble_ && active == sent_rumble_) {
        return;
    }
    std::vector<uint8_t> body;
    body.push_back(active ? 1u : 0u);
    std::vector<uint8_t> message;
    frame_message(ExternalControl::Rumble, body, message);
    // Recorded as sent only if it actually reached the wire. The check at the
    // top of this function is what makes that matter: this is diffed against
    // what was last delivered, so recording a value that was dropped means
    // every later call with that same value returns early and the edge never
    // arrives at all -- the motor stays stuck on whatever it was, for the rest
    // of the session. Leaving the recorded state alone instead means the next
    // call with the same value simply tries again.
    bool sent = false;
    write_best_effort(fd_, message.data(), message.size(), &sent);
    if (!sent) {
        return;
    }
    sent_rumble_ = active;
    have_sent_rumble_ = true;
}

void ExternalFrameProducer::send_player_rumble(int player, bool active) {
    if (fd_ < 0 || player < 0 || player >= kExternalPlayers) {
        return;
    }
    if (have_sent_player_rumble_[player] && active == sent_player_rumble_[player]) {
        return;
    }
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(player));
    body.push_back(active ? 1u : 0u);
    std::vector<uint8_t> message;
    frame_message(ExternalControl::PlayerRumble, body, message);
    // Recorded only once it is on the wire, for the reason send_rumble()
    // gives: a dropped edge recorded as sent would never be sent again.
    bool sent = false;
    write_best_effort(fd_, message.data(), message.size(), &sent);
    if (!sent) {
        return;
    }
    sent_player_rumble_[player] = active;
    have_sent_player_rumble_[player] = true;
}

void ExternalFrameProducer::send_menu_key() {
    if (fd_ < 0) {
        return;
    }
    // A key the player pressed once, so it waits for room rather than being
    // dropped -- briefly, and only ever on a keypress.
    std::vector<uint8_t> message;
    frame_message(ExternalControl::MenuKey, {}, message);
    write_all(fd_, message.data(), message.size());
}

bool ExternalFrameProducer::take_overlay(std::vector<uint8_t>& rgba, int& width, int& height) {
    if (memory_ == nullptr || !overlay_available() || !overlay_visible_ || overlay_slot_ < 0 ||
        overlay_serial_ == overlay_taken_serial_) {
        return false;
    }
    const auto* area = reinterpret_cast<const ExternalOverlayArea*>(
        static_cast<const uint8_t*>(memory_) + ExternalOverlayArea::offset());
    const ExternalSlotState& state = area->slots[overlay_slot_];
    const uint64_t before = state.sequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0) {
        return false;  // being written: a newer announcement is on its way
    }
    const int w = static_cast<int>(state.width.load(std::memory_order_relaxed));
    const int h = static_cast<int>(state.height.load(std::memory_order_relaxed));
    if (w <= 0 || h <= 0 || w > ExternalOverlayArea::kMaxWidth ||
        h > ExternalOverlayArea::kMaxHeight) {
        return false;
    }
    const uint8_t* source =
        static_cast<const uint8_t*>(memory_) + ExternalOverlayArea::slot_offset(overlay_slot_);
    rgba.assign(source, source + static_cast<std::size_t>(w) * h * 4);
    if (state.sequence.load(std::memory_order_acquire) != before) {
        return false;
    }
    overlay_taken_serial_ = overlay_serial_;
    width = w;
    height = h;
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
        } else if (kind == ExternalControl::PlayerInput) {
            if (body.size() >= 2 + kInputBytes && body[0] < kExternalPlayers) {
                const int player = body[0];
                player_connected_[player] = body[1] != 0u;
                player_input_[player] = read_input(body.data() + 2);
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
                // Optional tail -- a consumer built before the size existed
                // sends only the screen byte, which reads as "no preference".
                if (body.size() >= 5) {
                    requested_max_width_ = static_cast<int>(static_cast<uint16_t>(
                        body[1] | (static_cast<uint16_t>(body[2]) << 8)));
                    requested_max_height_ = static_cast<int>(static_cast<uint16_t>(
                        body[3] | (static_cast<uint16_t>(body[4]) << 8)));
                } else {
                    requested_max_width_ = 0;
                    requested_max_height_ = 0;
                }
                // And the tail after that, which older consumers do not send
                // at all -- absent reads as "no opinion", leaving whatever
                // this producer was showing alone.
                if (body.size() >= 6 && body[5] == 0u) {
                    shown_screen_ = ScreenId::Top;
                } else if (body.size() >= 6 && body[5] == 1u) {
                    shown_screen_ = ScreenId::Bottom;
                } else {
                    shown_screen_.reset();
                }
            }
        } else if (kind == ExternalControl::Pointer) {
            if (!body.empty()) {
                pointer_shown_ = body[0] != 0u;
            }
        } else if (kind == ExternalControl::Overlay) {
            if (body.size() >= 10 && overlay_available() &&
                body[1] < ExternalOverlayArea::kSlots) {
                overlay_visible_ = body[0] != 0u;
                overlay_slot_ = body[1];
                overlay_serial_ = static_cast<uint64_t>(read_u32(body.data() + 2)) |
                                  (static_cast<uint64_t>(read_u32(body.data() + 6)) << 32);
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
bool ExternalFrameLink::wait_for_message(int) const { return false; }
ExternalFrameLink::FrameCounts ExternalFrameLink::take_frame_counts(ScreenId) {
    return FrameCounts();
}
void ExternalFrameLink::take_audio(ScreenId, std::vector<int16_t>& out) { out.clear(); }
void ExternalFrameLink::request_reset() {}
void ExternalFrameLink::send_input(const ExternalInput&) {}
void ExternalFrameLink::request_screen(std::optional<ScreenId>, int, int,
                                       std::optional<ScreenId>) {}
bool ExternalFrameLink::take_rumble(bool&) { return false; }
void ExternalFrameLink::send_player_input(int, bool, const ExternalInput&) {}
bool ExternalFrameLink::take_player_rumble(int&, bool&) { return false; }
bool ExternalFrameLink::accept_peer(std::string*) { return false; }
bool ExternalFrameLink::read_messages(std::string*) { return false; }
bool ExternalFrameLink::handle_message(ExternalControl, const std::vector<uint8_t>&, std::string*) {
    return false;
}
bool ExternalFrameLink::send_control(ExternalControl, const std::vector<uint8_t>&) { return false; }
bool ExternalFrameLink::publish_overlay(const uint8_t*, int, int) { return false; }
void ExternalFrameLink::hide_overlay() {}
void ExternalFrameLink::send_overlay_state() {}
bool ExternalFrameLink::take_menu_key() { return false; }
void ExternalFrameLink::set_pointer_shown(bool) {}

ExternalFrameProducer::~ExternalFrameProducer() = default;
void ExternalFrameProducer::close() {}
bool ExternalFrameProducer::connect(const std::string&, const ExternalStreamInfo&,
                                    std::string* error) {
    if (error != nullptr) *error = "this platform has no way to hand frames to another program";
    return false;
}
bool ExternalFrameProducer::publish(ScreenId, uint64_t, const uint32_t*, int, int, int,
                                    std::string*, uint64_t) {
    return false;
}
bool ExternalFrameProducer::send_audio(ScreenId, const int16_t*, std::size_t, std::string*) {
    return false;
}
void ExternalFrameProducer::send_rumble(bool) {}
void ExternalFrameProducer::send_player_rumble(int, bool) {}
void ExternalFrameProducer::send_menu_key() {}
bool ExternalFrameProducer::take_overlay(std::vector<uint8_t>&, int&, int&) { return false; }
bool ExternalFrameProducer::poll(bool& out_reset_requested, std::string*) {
    out_reset_requested = false;
    return false;
}

}  // namespace ds

#endif

namespace ds {

uint64_t external_frame_clock_us() {
    // steady_clock is CLOCK_MONOTONIC wherever the link exists, which every
    // process on the machine reads alike.
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

}  // namespace ds
