#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <utility>
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
    // What else the producer can do -- kExternalFeature* bits. A trailing
    // word in the Hello, so a consumer that predates it reads none.
    uint32_t features = 0;
};

// The producer can draw an overlay the consumer hands it over its own window,
// and says when its own menu key is pressed: ExternalControl::Overlay and
// ExternalControl::MenuKey. A Wii U's Cemu, so this app's quick menu appears
// over the game in Cemu's window rather than behind it in this one.
inline constexpr uint32_t kExternalFeatureOverlay = 1u;

// The producer can pause the game while this app's quick menu is up, the way a
// Wii U pauses for its HOME Menu: ExternalControl::Pause, answered with
// ExternalControl::PauseState. The game stops itself when it is ready to, or
// says it cannot right now, so the answer is not immediate.
inline constexpr uint32_t kExternalFeaturePause = 2u;

// Where a game is, as a producer that can pause it says.
enum class ExternalPauseState : uint8_t {
    Running = 0,  // going again, after a pause or a refusal
    Paused = 1,   // stopped, and nothing it shows will change until it goes on
    Refused = 2,  // it cannot stop now -- a Wii U game in the middle of saving
    // It never listens for the HOME Menu at all, so it cannot be asked: it
    // goes on, and the request is not kept.
    NotListening = 3,
};

// Microseconds on a clock both ends of the link read the same: the system's
// monotonic clock, which on one machine is one clock whichever process asks.
uint64_t external_frame_clock_us();

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
    // producer -> consumer, "screen S, slot N, serial X", then optionally
    // "captured at T" (external_frame_clock_us()). A consumer that predates T
    // reads only what it knows; one that finds no T times nothing.
    Frame = 2,
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
    // consumer -> producer, one of the game's other players' pads: a player
    // number (0 for Player 1) and whether anything is feeding it, then the
    // same bytes Input carries. Latest-state like Input, and for the same
    // reason. See ExternalPlayers.
    PlayerInput = 9,
    // producer -> consumer, "Player N's motor is on/off", on the edges like
    // Rumble.
    PlayerRumble = 10,
    // consumer -> producer, "draw overlay slot N (serial X) over your window",
    // or "draw nothing" -- see ExternalOverlayArea. Only to a producer whose
    // Hello said kExternalFeatureOverlay. Its first byte is 0 for nothing, 1
    // for a menu, which takes the keys, clicks and pads while it is up, and 2
    // for a notice, which only shows: the game goes on hearing them. A
    // producer that predates 2 reads it as 1.
    Overlay = 11,
    // producer -> consumer, "the player pressed the menu key in my window" --
    // Tab or F1, the keys that open this app's own menu. Its window is the one
    // with the keyboard while a game plays in it, so only it can hear them.
    MenuKey = 12,
    // consumer -> producer, "show the mouse pointer over your window, or
    // never do" -- this app's Pointer setting. A producer that predates it
    // ignores it and goes on as it was.
    Pointer = 13,
    // producer -> consumer, "the overlay was tapped": where the click or
    // finger went down and where it came up, each as a fraction of the
    // overlay's width and height in 1/65535ths. A click on the menu is the
    // menu's, not the game's, so the producer keeps it from the game.
    OverlayTap = 14,
    // consumer -> producer, "pause the game" (1) or "let it go on" (0). Only to
    // a producer whose Hello said kExternalFeaturePause. A pause asked for is
    // not a pause yet: the producer answers with PauseState once the game has
    // stopped, or refused.
    Pause = 15,
    // producer -> consumer, an ExternalPauseState, each time it changes.
    PauseState = 16,
    // producer -> consumer, "this menu key went down" (1) or "up" (0) in my
    // window, while the consumer's overlay is up: an ExternalMenuKey. The
    // producer's window has the keyboard then, and only the consumer knows
    // what its menu does with a key. Ups as well as downs, so a held arrow
    // can repeat on the consumer's side as one held in its own window does.
    MenuKeyboard = 17,
};

// The keys a menu answers, by what they mean to it rather than any keyboard's
// codes, so neither end needs the other's. The order is the wire format, so
// entries are only ever added at the end.
enum class ExternalMenuKey : uint8_t {
    Up, Down, Left, Right,
    PageUp, PageDown,
    Confirm,     // Enter, Space
    Back,        // Escape, Backspace
    Filter,      // F2
    SwapLayout,  // F3
    Settings,    // F4
    Reset,       // F5
    Quit,        // F6
    Disconnect,  // F7
    Count
};

// How many players the link carries besides the GamePad, which is not one of
// them: a Wii U's GamePad is its own controller, and its other controllers --
// Pro Controllers, Wii Remotes -- are the players. Eight because that is how
// many controllers Cemu can map at once; which physical pad feeds which player
// is decided on the consumer's side, where every controller on every device
// can be seen, and the producer only ever sees Player 1 to Player 8.
inline constexpr int kExternalPlayers = 8;

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

// How a GamePad is being held and moved, as read off whatever device is
// actually standing in for it.
//
// Raw, and in SDL's units and axes: gyro in radians per second, acceleration
// in metres per second squared, right-handed with +x right, +y up and +z
// towards the player. Nothing is integrated here on purpose -- a producer
// consuming this has its own sensor fusion, tuned to what a real GamePad
// reports, and this link handing it a second opinion about which way is up
// would be worse than handing it nothing.
struct ExternalMotion {
    // False when the device standing in for the GamePad has no sensors, or
    // has not been asked to read them. Everything below means nothing then,
    // and a producer should fall back to whatever motion it has of its own.
    bool present = false;
    float gyro_x = 0.0f;
    float gyro_y = 0.0f;
    float gyro_z = 0.0f;
    float accel_x = 0.0f;
    float accel_y = 0.0f;
    float accel_z = 0.0f;
    // When it was sampled, by the reading device's own clock, in
    // microseconds. What a producer integrating these should measure the
    // interval between samples with, rather than its own arrival times: the
    // reading crossed a network to get here, and a sample delayed by a busy
    // moment on the wire is not a sample that took longer to happen.
    // Unsigned subtraction gives the right interval across the wrap this has
    // every seventy-one minutes.
    uint32_t sampled_us = 0;

    bool operator==(const ExternalMotion& other) const {
        return present == other.present && gyro_x == other.gyro_x && gyro_y == other.gyro_y &&
               gyro_z == other.gyro_z && accel_x == other.accel_x && accel_y == other.accel_y &&
               accel_z == other.accel_z && sampled_us == other.sampled_us;
    }
    bool operator!=(const ExternalMotion& other) const { return !(*this == other); }
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
    // How the thing is being held -- see ExternalMotion. Absent when whatever
    // is standing in for the GamePad has no sensors to read.
    ExternalMotion motion;

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
               touch_y == other.touch_y && motion == other.motion;
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

// The overlay: pictures going the other way, from this app to the producer, to
// be drawn over the producer's own window -- this app's quick menu over a Wii U
// game in Cemu's window. After everything ExternalSharedHeader lays out, so a
// producer built before it maps exactly what it always mapped and never sees
// it; one built after checks the memory is big enough before reading it, so
// an older consumer's smaller memory is simply an overlay it does not offer.
//
// Two slots, written by the consumer under the same odd/even sequence the
// frames use, so the producer copying one while the next is written into the
// other never tears. The pixels are RGBA8 in memory order with straight
// (unpremultiplied) alpha, tightly packed: what an OpenGL readback gives and
// what an ImGui texture takes.
struct ExternalOverlayArea {
    static constexpr int kSlots = 2;
    // A menu, not a game: drawn at the consumer's own display size up to
    // this, and scaled to the producer's window.
    static constexpr int kMaxWidth = 1920;
    static constexpr int kMaxHeight = 1080;

    ExternalSlotState slots[kSlots];

    static std::size_t offset() { return ExternalSharedHeader::total_bytes(); }
    static std::size_t slot_bytes() { return static_cast<std::size_t>(kMaxWidth) * kMaxHeight * 4; }
    static std::size_t pixels_offset() {
        constexpr std::size_t kPage = 4096;
        return offset() + ((sizeof(ExternalOverlayArea) + kPage - 1) / kPage) * kPage;
    }
    static std::size_t slot_offset(int slot) {
        return pixels_offset() + slot_bytes() * static_cast<std::size_t>(slot);
    }
    // The whole of the memory once the overlay is in it.
    static std::size_t end() { return pixels_offset() + slot_bytes() * kSlots; }
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
    // Waits up to `timeout_ms` for the producer to say something -- a new
    // frame, most often -- so a loop can run at the game's pace rather than
    // at its own. True when there is something to read; at once when there
    // is nothing connected to wait on.
    bool wait_for_message(int timeout_ms) const;
    // Frames the producer announced for a screen since the last ask, and how
    // many of those were overwritten by the next before anything took them.
    // The game's own rate, and what reading it too slowly cost.
    // With a producer that says when it captured each frame, also how old the
    // frames were: when announced (the producer's own delay -- the GPU
    // finishing, the readback, the copy) and when taken (that plus this app
    // getting round to them).
    struct FrameCounts {
        unsigned announced = 0;
        unsigned missed = 0;
        unsigned timed_announced = 0;
        uint64_t announced_age_us = 0;  // summed over timed_announced
        unsigned timed_taken = 0;
        uint64_t taken_age_us = 0;  // summed over timed_taken
        uint64_t taken_age_max_us = 0;
    };
    FrameCounts take_frame_counts(ScreenId screen);
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
    //
    // `shown` is the other half of the same conversation, and a separate
    // question: which screen the producer should put in its own window. A
    // producer used to have to guess it -- the one not being captured -- and
    // that guess has no answer at all when nothing is being captured, which
    // is a device playing on its own. That case is the whole reason this is
    // said outright: a handheld with nobody else connected is a GamePad and
    // wants Cemu showing the GamePad, and a desktop is a television and wants
    // the television, and only this end knows which it is being. Absent
    // leaves the producer's own display choice alone, which is what a
    // consumer that has no opinion sends.
    void request_screen(std::optional<ScreenId> screen, int max_width = 0, int max_height = 0,
                        std::optional<ScreenId> shown = std::nullopt);

    // The GamePad motor's state as of the last thing the producer sent, and
    // whether anything new has arrived since the last call -- a one-shot,
    // same shape as HostSession::take_video_error(), because what matters to
    // a caller forwarding this onward is the edges, not the level: reading it
    // as a level would mean redelivering "on" every poll for as long as it
    // stays on, and a caller with its own "only send what changed" state
    // (see HostSession::publish_rumble()) would have to reinvent this to
    // undo that. False until a producer has said anything at all.
    bool take_rumble(bool& out_active);

    // Whether the producer draws what publish_overlay() hands it -- see
    // kExternalFeatureOverlay.
    bool overlay_supported() const { return connected() && (info_.features & kExternalFeatureOverlay) != 0u; }
    // Hands the producer a picture to draw over its own window: `rgba`, RGBA8
    // with straight alpha, `width` x `height` (no bigger than
    // ExternalOverlayArea's maximum). Shown until hide_overlay() or the next
    // publish. `takes_input` false is a notice the player only reads: the
    // game keeps its keys, clicks and pads. False when the producer cannot
    // draw one.
    bool publish_overlay(const uint8_t* rgba, int width, int height, bool takes_input = true);
    void hide_overlay();
    // Whether the producer's menu key was pressed since the last call.
    bool take_menu_key();
    // This app's Pointer setting, for the producer's own window -- see
    // ExternalControl::Pointer. Only a change is put on the wire.
    void set_pointer_shown(bool shown);
    // A tap on the overlay, oldest first: where it went down and came up, as
    // fractions of the overlay's width and height. False when there is none.
    bool take_overlay_tap(float& press_x, float& press_y, float& x, float& y);

    // Whether the producer can pause the game -- see kExternalFeaturePause.
    bool pause_supported() const { return connected() && (info_.features & kExternalFeaturePause) != 0u; }
    // Asks the producer to pause the game, or to let it go on. Only a change
    // is put on the wire. Nothing has paused until take_pause_state() says so.
    void request_pause(bool pause);
    // What the producer has said about the game since the last call, oldest
    // first. False when it has said nothing new.
    bool take_pause_state(ExternalPauseState& out);

    // A menu key pressed or let go in the producer's window, oldest first --
    // see ExternalControl::MenuKeyboard. False when there is none.
    bool take_menu_keyboard(ExternalMenuKey& key, bool& down);

    // One player's pad, the same way send_input() sends the GamePad's -- only
    // what changed, never blocking. `connected` false means nothing is
    // feeding this player any more, which a producer should show as the
    // controller being unplugged rather than as one with nothing pressed.
    // `player` outside [0, kExternalPlayers) is ignored.
    void send_player_input(int player, bool connected, const ExternalInput& input);

    // The next change to a player's motor, oldest first. One-shot edges like
    // take_rumble(), but queued rather than latest-only: two players can
    // change in the same poll, and neither edge may be lost.
    bool take_player_rumble(int& out_player, bool& out_active);

    // Where a producer connects when nobody said otherwise. Under the user's
    // runtime directory rather than /tmp, so two people on one machine do not
    // collide and nothing survives a reboot.
    static std::string default_socket_path();

private:
    struct Pending {
        int slot = -1;
        uint64_t serial = 0;
        uint64_t captured_us = 0;  // 0 when the producer did not say
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
    FrameCounts counts_[2];
    ExternalInput sent_input_;
    bool have_sent_input_ = false;
    // What request_screen() last sent, so a repeat of the same request costs
    // nothing -- the same "only what changed" shape send_input() uses.
    std::optional<ScreenId> sent_screen_;
    int sent_max_width_ = -1;
    int sent_max_height_ = -1;
    // Deliberately not an optional: "not sent yet" has to be distinguishable
    // from "sent, meaning no opinion", or the first request never goes out.
    bool have_sent_shown_ = false;
    std::optional<ScreenId> sent_shown_;
    bool rumble_active_ = false;
    bool have_new_rumble_ = false;
    // What send_player_input() last got onto the wire, per player -- the same
    // "only what changed" bookkeeping as sent_input_.
    ExternalInput sent_player_input_[kExternalPlayers];
    bool sent_player_connected_[kExternalPlayers] = {};
    bool have_sent_player_[kExternalPlayers] = {};
    std::vector<std::pair<int, bool>> player_rumble_;
    // Where the next overlay goes, its serial, and whether one is up.
    int next_overlay_slot_ = 0;
    int overlay_slot_ = -1;
    uint64_t overlay_serial_ = 0;
    bool overlay_shown_ = false;
    bool overlay_takes_input_ = true;
    // Whether the producer has yet to hear the latest of those: an Overlay
    // message that found the socket full, tried again by the next poll().
    bool overlay_unsent_ = false;
    bool menu_key_ = false;
    void send_overlay_state();
    // The Pointer setting as last delivered; absent until one has been.
    std::optional<bool> sent_pointer_shown_;
    std::vector<std::array<float, 4>> overlay_taps_;
    // What request_pause() last got onto the wire; absent until it has.
    std::optional<bool> sent_pause_;
    std::vector<ExternalPauseState> pause_states_;
    std::vector<std::pair<ExternalMenuKey, bool>> menu_keyboard_;
    ExternalOverlayArea* overlay_area() {
        return reinterpret_cast<ExternalOverlayArea*>(static_cast<uint8_t*>(memory_) +
                                                      ExternalOverlayArea::offset());
    }
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
    // `captured_us` is when the frame was captured, by external_frame_clock_us(),
    // for the consumer to say how old frames are by the time it has them; 0
    // means now.
    bool publish(ScreenId screen, uint64_t serial, const uint32_t* pixels, int width, int height,
                 int stride_pixels, std::string* error, uint64_t captured_us = 0);
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
    // The same, for one of the players -- see kExternalPlayers.
    void send_player_rumble(int player, bool active);

    // Whether the consumer has asked for a reset since the last call, and
    // whether it has gone away.
    bool poll(bool& out_reset_requested, std::string* error);

    // The pad as of the last thing the consumer sent. False until it has sent
    // anything at all, which is how a fork knows to keep reading its own pads
    // instead -- and stays true from then on, since a pad with nothing pressed
    // is still a pad.
    bool have_input() const { return have_input_; }
    const ExternalInput& input() const { return input_; }

    // One player's pad as of the last poll(), and whether anything is feeding
    // that player at all. False for a player the consumer has never mentioned,
    // and for one it has since said nothing feeds -- both of which a producer
    // should present as a controller that is not plugged in.
    bool player_connected(int player) const {
        return player >= 0 && player < kExternalPlayers && player_connected_[player];
    }
    const ExternalInput& player_input(int player) const {
        static const ExternalInput kNothing;
        return (player >= 0 && player < kExternalPlayers) ? player_input_[player] : kNothing;
    }

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

    // Which screen the consumer would like this producer to show in its own
    // window, as of the last poll(). Absent means it has no opinion -- either
    // it is an older consumer that never had one, or it explicitly said so --
    // and the producer should leave its own display choice alone. See
    // request_screen(), whose `shown` this is.
    std::optional<ScreenId> shown_screen() const { return shown_screen_; }

    // Tells the consumer its menu key was pressed in this producer's window.
    void send_menu_key();
    // Whether the consumer wants an overlay drawn, as of the last poll(), and
    // whether this end can at all: the consumer's memory has room for one.
    bool overlay_available() const { return memory_bytes_ >= ExternalOverlayArea::end(); }
    bool overlay_visible() const { return overlay_visible_; }
    // Whether that overlay is a menu, which the keys, clicks and pads go to
    // while it is up, rather than a notice they pass under.
    bool overlay_takes_input() const { return overlay_visible_ && overlay_takes_input_; }
    // The overlay's pixels, when they have changed since the last call:
    // RGBA8, straight alpha, `width` x `height`. False when nothing new has
    // arrived, or what arrived was being rewritten as it was read -- the next
    // poll() brings the newer one.
    bool take_overlay(std::vector<uint8_t>& rgba, int& width, int& height);
    // Whether the consumer wants the mouse pointer shown over this window --
    // true until it has said otherwise.
    bool pointer_shown() const { return pointer_shown_; }
    // A tap on the overlay, as take_overlay_tap() hands it over at the other
    // end: fractions of the overlay's width and height, 0 to 1.
    void send_overlay_tap(float press_x, float press_y, float x, float y);

    // The consumer's requests to pause the game (true) or let it go on
    // (false) since the last call, oldest first. False when there are none.
    bool take_pause_request(bool& out_pause);
    // Tells the consumer where the game is now: paused once it has stopped,
    // refused when it cannot, running when it goes on.
    void send_pause_state(ExternalPauseState state);

    // A menu key going down or up in this producer's window, for the
    // consumer's menu over it -- see ExternalControl::MenuKeyboard.
    void send_menu_keyboard(ExternalMenuKey key, bool down);

private:
    ExternalSharedHeader* header() { return static_cast<ExternalSharedHeader*>(memory_); }

    int fd_ = -1;
    int memory_fd_ = -1;
    void* memory_ = nullptr;
    // How much of it is mapped: ExternalSharedHeader's, and the overlay's
    // after it when the consumer made room for one.
    std::size_t memory_bytes_ = 0;
    int next_slot_[2] = {0, 0};
    bool overlay_visible_ = false;
    bool overlay_takes_input_ = true;
    int overlay_slot_ = -1;
    uint64_t overlay_serial_ = 0;
    uint64_t overlay_taken_serial_ = 0;
    bool pointer_shown_ = true;
    std::vector<bool> pause_requests_;
    std::vector<uint8_t> inbox_;
    ExternalInput input_;
    bool have_input_ = false;
    std::optional<ScreenId> requested_screen_;
    int requested_max_width_ = 0;
    int requested_max_height_ = 0;
    std::optional<ScreenId> shown_screen_;
    bool sent_rumble_ = false;
    bool have_sent_rumble_ = false;
    ExternalInput player_input_[kExternalPlayers];
    bool player_connected_[kExternalPlayers] = {};
    bool sent_player_rumble_[kExternalPlayers] = {};
    bool have_sent_player_rumble_[kExternalPlayers] = {};
};

}  // namespace ds
