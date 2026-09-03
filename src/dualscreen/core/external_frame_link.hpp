#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "layout/screen_id.hpp"

// The handover between this app and a program that plays a console this app
// does not run -- Cemu, for a Wii U. See docs/research-notes.md for why a Wii U
// is played that way rather than in a core.
//
// Two channels, because the two halves have opposite shapes:
//
//   Pixels go through shared memory. A television frame and a GamePad frame
//   together are about five megabytes, sixty times a second; a socket would
//   copy every byte of that twice for nothing.
//
//   Everything else goes through a Unix socket -- the handshake, audio, resets,
//   and the end of the session. Audio is a couple of hundred kilobytes a second
//   and does not deserve its own machinery, and a socket has the one property
//   shared memory does not: when the program at the other end dies, this end
//   finds out immediately rather than waiting for frames that never come.
//
// This app is the consumer and it goes first: it makes the memory, it listens,
// and it hands the producer a file descriptor for that memory over the socket.
// So there is no path to agree on, nothing left behind in /dev/shm when either
// side crashes, and no chance of two sessions finding each other's buffers.
//
// Both roles are here on purpose. A protocol described twice is a protocol with
// two subtly different implementations, and the second one is going to live in
// a fork of somebody else's emulator.
namespace ds {

// What a producer says about itself when it connects. The consumer already
// knows which console this is; what it cannot know is how big the pictures will
// be, since an emulator may be rendering above native.
struct ExternalStreamInfo {
    int top_width = 0;
    int top_height = 0;
    int bottom_width = 0;
    int bottom_height = 0;
    int audio_sample_rate = 0;
    // How fast the producer intends to run, for pacing. Zero means it does not
    // know, and the console's own rate is assumed.
    double frames_per_second = 0.0;
};

// One screen's pixels. A copy rather than a pointer into the shared memory: the
// producer is free to overwrite a slot the moment it has published a newer one,
// and a caller that held a pointer across a frame would be reading whatever it
// had become.
struct ExternalFrame {
    std::vector<uint32_t> pixels;
    int width = 0;
    int height = 0;
    uint64_t serial = 0;

    bool valid() const {
        return width > 0 && height > 0 &&
               pixels.size() >= static_cast<std::size_t>(width) * height;
    }
};

// What the two ends say to each other. Deliberately small: everything here is
// either something the producer knows and this app cannot, or something a
// player asked for that only the producer can do.
enum class ExternalControl : uint8_t {
    Hello = 1,     // producer -> consumer, an ExternalStreamInfo
    Frame = 2,     // producer -> consumer, "screen S, slot N, serial X"
    Audio = 3,     // producer -> consumer, interleaved stereo int16
    Reset = 4,     // consumer -> producer, the player chose Reset Game
    Shutdown = 5,  // either way, "this session is over"
    Input = 6,     // consumer -> producer, an ExternalInput
    // consumer -> producer, "capture and send me this screen (or neither), at
    // no more than this size" -- sendable at any point in a session, not just
    // at connect, since what a shell wants can change as devices come and go.
    // Absent means the producer's own screen(s) are its own business again:
    // nothing captured, nothing streamed, exactly like running without a
    // shell attached at all.
    RequestScreen = 7,
    // producer -> consumer, "the GamePad's motor is on/off". Sent on every
    // change, never on a timer -- see ExternalFrameProducer::send_rumble().
    // What the pattern behind it looks like once a real device plays a Wii U
    // through this link (a bit sampled at up to 60Hz -- see VPADControlMotor)
    // is the producer's own business; this link only ever carries the on/off
    // edges, which is all a companion device standing in for the GamePad's
    // one motor can act on anyway.
    Rumble = 8,
};

// A GamePad, in its own terms rather than this app's. What crosses this link is
// a Wii U's controls; an emulator on the far end has no interest in how this
// app happens to represent a DS internally, and translating once here is better
// than exporting that shape to somebody else's tree.
//
// The order is the wire format, so entries are only ever added at the end.
enum class ExternalButton : uint8_t {
    A, B, X, Y,
    L, R, Zl, Zr,
    Plus, Minus,
    Up, Down, Left, Right,
    LeftStick, RightStick,
};

// One moment's worth of it. Sent every frame rather than as changes, because a
// pad's state is small and a missed edge is a button that sticks down.
struct ExternalInput {
    uint32_t buttons = 0;  // one bit per ExternalButton
    // Both sticks, in libretro's range: [-32767, 32767], y positive downwards.
    int16_t left_x = 0;
    int16_t left_y = 0;
    int16_t right_x = 0;
    int16_t right_y = 0;
    // The GamePad's own touch screen, in its own pixels rather than in
    // whatever window the player is actually touching.
    bool touching = false;
    uint16_t touch_x = 0;
    uint16_t touch_y = 0;

    bool pressed(ExternalButton button) const {
        return (buttons & (1u << static_cast<unsigned>(button))) != 0u;
    }
    void set(ExternalButton button, bool down) {
        const uint32_t mask = 1u << static_cast<unsigned>(button);
        buttons = down ? (buttons | mask) : (buttons & ~mask);
    }
    bool operator==(const ExternalInput& other) const {
        return buttons == other.buttons && left_x == other.left_x && left_y == other.left_y &&
               right_x == other.right_x && right_y == other.right_y &&
               touching == other.touching && touch_x == other.touch_x &&
               touch_y == other.touch_y;
    }
    bool operator!=(const ExternalInput& other) const { return !(*this == other); }
};

// The size to reduce a `width` x `height` picture to so it fits inside
// `max_width` x `max_height` without changing its shape. Never enlarges: a
// consumer with a display bigger than the picture is not a reason to invent
// detail that was never rendered. Zero (or negative) for either cap means no
// cap at all, and the size comes back unchanged.
//
// Here, in the header both ends share, because both ends have to agree on it:
// the producer resizes to this before publishing, and the consumer's own cap
// then has nothing left to do. Two copies of this arithmetic that rounded
// differently would mean a picture arriving a few pixels off what was asked
// for, and the consumer paying for a scale it thought it had avoided.
inline void fit_within(int width, int height, int max_width, int max_height, int& out_width,
                       int& out_height) {
    out_width = width;
    out_height = height;
    if (width <= 0 || height <= 0 || max_width <= 0 || max_height <= 0) {
        return;
    }
    const double scale = std::min(static_cast<double>(max_width) / width,
                                  static_cast<double>(max_height) / height);
    if (scale >= 1.0) {
        return;
    }
    // Landing on a multiple of 16, with a floor, for the same reasons the
    // consumer's own encode-size cap does (see compute_capped_encode_size in
    // host_session.cpp, whose constants these are): 4:2:0 needs even
    // dimensions, a software H.264 encode of a small non-macroblock-aligned
    // width has been seen to corrupt memory outright, and 64 is below
    // anything worth encoding but above a degenerate size. Matching that
    // function exactly is the point -- it is what makes it a no-op on a
    // picture already reduced here, rather than a second scale for the sake
    // of a few rows.
    constexpr int kMinDimension = 64;
    constexpr int kAlignment = 16;
    out_width = std::max(kMinDimension, static_cast<int>(width * scale) / kAlignment * kAlignment);
    out_height =
        std::max(kMinDimension, static_cast<int>(height * scale) / kAlignment * kAlignment);
}

// How the shared memory is arranged. Both sides compute this from the same
// numbers rather than sending it, so there is one description of the layout
// instead of two that could disagree.
struct ExternalSlotState {
    // Odd while the producer is writing this slot, even when it is settled --
    // a reader that sees an odd number, or a different one either side of its
    // copy, knows it read a frame being overwritten and drops it. Cheaper than
    // any lock and correct without the producer ever waiting for a reader.
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> serial{0};
    std::atomic<uint32_t> width{0};
    std::atomic<uint32_t> height{0};
};

struct ExternalSharedHeader {
    static constexpr uint32_t kMagic = 0x4C58'5344;  // "DSXL"
    static constexpr uint32_t kVersion = 1;
    // Three per screen: one being written, one being read, one spare. Two would
    // do until a reader was slow, which is exactly when it matters.
    static constexpr int kSlotsPerScreen = 3;
    // The most a Wii U screen can be asked to be. Cemu renders above native --
    // a graphics pack's own resolution presets go up to 4K (observed directly:
    // Bayonetta 2's "Extreme" preset renders at 3840x2160, and silently never
    // reached this link at the old 1920x1080 ceiling) -- and the memory is
    // made once rather than remade whenever a running game changes its mind.
    static constexpr int kMaxWidth = 3840;
    static constexpr int kMaxHeight = 2160;

    std::atomic<uint32_t> magic{0};
    std::atomic<uint32_t> version{0};
    ExternalSlotState slots[2][kSlotsPerScreen];

    static std::size_t slot_bytes() {
        return static_cast<std::size_t>(kMaxWidth) * kMaxHeight * sizeof(uint32_t);
    }
    static std::size_t pixels_offset() {
        // Rounded up so every slot starts on a page, which is what makes a
        // partially-written slot a partially-written picture rather than a
        // question about what a torn write to a header looks like.
        constexpr std::size_t kPage = 4096;
        return ((sizeof(ExternalSharedHeader) + kPage - 1) / kPage) * kPage;
    }
    static std::size_t total_bytes() {
        return pixels_offset() + slot_bytes() * kSlotsPerScreen * 2;
    }
    static std::size_t slot_offset(int screen_index, int slot) {
        return pixels_offset() +
               slot_bytes() * (static_cast<std::size_t>(screen_index) * kSlotsPerScreen + slot);
    }
    static int screen_index(ScreenId screen) { return screen == ScreenId::Top ? 0 : 1; }
};

// The consumer: this app, waiting for something to play.
class ExternalFrameLink {
public:
    ~ExternalFrameLink();

    // Makes the shared memory and starts listening. `socket_path` is unlinked
    // first: a path left behind by a crash would otherwise make this fail for
    // a reason nobody could see.
    bool listen(const std::string& socket_path, std::string* error);
    void close();
    bool listening() const { return listen_fd_ >= 0; }

    // True once a producer has connected and said what it is going to send.
    bool connected() const { return peer_fd_ >= 0 && have_info_; }
    const ExternalStreamInfo& info() const { return info_; }

    // Takes whatever has arrived: a connection, a handshake, frames, audio.
    // False when the session has ended, with the reason in `error` -- which
    // includes the producer exiting, since that is what "the game closed" looks
    // like from here.
    bool poll(std::string* error);

    // The newest frame published for this screen since the last call, if any.
    bool take_frame(ScreenId screen, ExternalFrame& out);
    // Audio sent for this screen since the last call, interleaved stereo.
    // Drained rather than peeked: it is played once. Tagged by screen for the
    // same reason frames are: a Wii U's TV and GamePad each have their own
    // independent audio, routed to wherever the player asked for that
    // screen's sound to go -- mixing them into one stream here would make
    // that choice impossible to honour on the far side.
    void take_audio(ScreenId screen, std::vector<int16_t>& out);
    // Asks the producer to restart the game. Nothing happens here; only the
    // producer knows what restarting means.
    void request_reset();
    // Hands the producer the pad, which on this side is both players' pads
    // merged. Sent only when it differs from what was sent last, so a session
    // where nothing is being pressed costs nothing. Never blocks the caller:
    // this shares a poll loop with the audio this link hands onward, so a
    // pad update waiting on a full send buffer would otherwise be heard as an
    // audio cutout timed to whenever the pad moved. A send that cannot go out
    // immediately is simply retried the next time the pad state is checked,
    // same as a value that has not changed yet.
    void send_input(const ExternalInput& input);
    // Tells the producer which single screen, if any, this end actually
    // wants captured right now. A producer with a native display of its own
    // (a Wii U's Cemu, showing whichever screen this does not ask for in its
    // own window) uses this to avoid paying for a screen nobody is watching
    // through the link at all -- so this is worth sending every time what is
    // wanted changes, including down to nothing when there is nowhere local
    // to show it.
    // `max_width`/`max_height` cap what is wanted: a producer rendering above
    // the size the far end can actually show (a Wii U emulator with a 4K
    // graphics pack feeding a 1280x800 handheld) should reduce the picture
    // itself, where it already has the frame on a GPU, rather than handing
    // over sixteen times the pixels for this end to copy across a socket and
    // then scale down on a CPU. Both zero means "whatever size you have",
    // which is what a consumer that does not know the far end's display size
    // yet asks for. See fit_within() for the arithmetic both ends agree on.
    void request_screen(std::optional<ScreenId> screen, int max_width = 0, int max_height = 0);

    // The GamePad motor's state as of the last thing the producer sent, and
    // whether anything new has arrived since the last call -- a one-shot,
    // same shape as HostSession::take_video_error(), because what matters to
    // a caller forwarding this onward is the edges, not the level: reading it
    // as a level would mean redelivering "on" every poll for as long as it
    // stays on, and a caller with its own "only send what changed" state
    // (see HostSession::publish_rumble()) would have to reinvent this to
    // undo that. False until a producer has said anything at all.
    bool take_rumble(bool& out_active);

    // Where a producer connects when nobody said otherwise. Under the user's
    // runtime directory rather than /tmp, so two people on one machine do not
    // collide and nothing survives a reboot.
    static std::string default_socket_path();

private:
    struct Pending {
        int slot = -1;
        uint64_t serial = 0;
    };

    bool accept_peer(std::string* error);
    bool read_messages(std::string* error);
    bool handle_message(ExternalControl kind, const std::vector<uint8_t>& body,
                        std::string* error);
    bool send_control(ExternalControl kind, const std::vector<uint8_t>& body);
    // send_input()'s non-blocking twin -- see its definition for why sending
    // a pad update can never be allowed to wait on this link's socket.
    bool send_control_best_effort(ExternalControl kind, const std::vector<uint8_t>& body,
                                  bool& out_sent);
    ExternalSharedHeader* header() { return static_cast<ExternalSharedHeader*>(memory_); }

    int listen_fd_ = -1;
    int peer_fd_ = -1;
    int memory_fd_ = -1;
    void* memory_ = nullptr;
    std::string socket_path_;

    ExternalStreamInfo info_;
    bool have_info_ = false;
    std::vector<uint8_t> inbox_;
    std::vector<int16_t> audio_[2];
    Pending pending_[2];
    ExternalInput sent_input_;
    bool have_sent_input_ = false;
    // What request_screen() last sent, so a repeat of the same request costs
    // nothing -- the same "only what changed" shape send_input() uses.
    std::optional<ScreenId> sent_screen_;
    int sent_max_width_ = -1;
    int sent_max_height_ = -1;
    bool rumble_active_ = false;
    bool have_new_rumble_ = false;
};

// The producer: whatever is actually playing the game. Here so that the fork of
// Cemu that will use it is working from the same code as the fake one the tests
// drive, rather than from a description of it.
class ExternalFrameProducer {
public:
    ~ExternalFrameProducer();

    // Connects and says what is going to be sent. The shared memory arrives
    // over the socket as part of this.
    bool connect(const std::string& socket_path, const ExternalStreamInfo& info,
                 std::string* error);
    void close();
    bool connected() const { return fd_ >= 0; }

    // Writes one screen into the next free slot and announces it. `serial` ties
    // the two screens of one moment together, so a consumer never shows the
    // television from one frame beside the GamePad from another.
    bool publish(ScreenId screen, uint64_t serial, const uint32_t* pixels, int width, int height,
                 int stride_pixels, std::string* error);
    // Tagged by screen: a Wii U's TV and GamePad audio are independent
    // streams a player can route separately, so they cross the link as two.
    bool send_audio(ScreenId screen, const int16_t* interleaved_stereo, std::size_t frames,
                    std::string* error);
    // Tells the consumer whether the GamePad's motor is currently on. Sent
    // only when it changed since the last call, the same "only what's new"
    // shape send_input() and request_screen() already use -- a companion
    // device standing in for the GamePad has nothing to gain from being told
    // "still on" sixty times a second. Best-effort like publish()/send_audio()
    // above rather than blocking like request_reset(): this is called from
    // wherever the emulated pad's own rumble state changes, which on a real
    // fork is driven by the game and must never be able to stall it waiting
    // on a slow consumer.
    void send_rumble(bool active);

    // Whether the consumer has asked for a reset since the last call, and
    // whether it has gone away.
    bool poll(bool& out_reset_requested, std::string* error);

    // The pad as of the last thing the consumer sent. False until it has sent
    // anything at all, which is how a fork knows to keep reading its own pads
    // instead -- and stays true from then on, since a pad with nothing pressed
    // is still a pad.
    bool have_input() const { return have_input_; }
    const ExternalInput& input() const { return input_; }

    // Which single screen the consumer has most recently asked for, if any --
    // updated as of the last poll(). Absent means either nothing has been
    // asked for yet or the consumer explicitly asked for nothing.
    std::optional<ScreenId> requested_screen() const { return requested_screen_; }

    // The largest picture the consumer wants for that screen, as of the last
    // poll(). Both zero means it expressed no preference and the producer
    // should hand over whatever it has. A producer that can resize cheaply --
    // one with the frame already on a GPU -- should reduce to fit_within()
    // of these before publishing; one that cannot is free to ignore them,
    // since the consumer caps the picture again on its own side regardless.
    int requested_max_width() const { return requested_max_width_; }
    int requested_max_height() const { return requested_max_height_; }

private:
    ExternalSharedHeader* header() { return static_cast<ExternalSharedHeader*>(memory_); }

    int fd_ = -1;
    int memory_fd_ = -1;
    void* memory_ = nullptr;
    int next_slot_[2] = {0, 0};
    std::vector<uint8_t> inbox_;
    ExternalInput input_;
    bool have_input_ = false;
    std::optional<ScreenId> requested_screen_;
    int requested_max_width_ = 0;
    int requested_max_height_ = 0;
    bool sent_rumble_ = false;
    bool have_sent_rumble_ = false;
};

}  // namespace ds
