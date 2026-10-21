#pragma once

// Slowing a game's sound down with the game, when it cannot keep up -- see
// kAudioStretchBelowSpeed.
//
// Standalone on purpose: nothing here needs SDL or the rest of this project,
// because Cemu vendors these two files byte for byte to do the same for the
// screen it plays itself (cemu-dualscreen, src/dualscreen/audio/), the way it
// vendors the frame link. Keep it that way; the external_link_stands_alone
// test compiles them alone.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ds {

// A wait between pushes this long is the stream stopping -- a menu up on the
// host, the game paused -- not the link holding it back or the game running
// slow. AudioOutput buys no cover for it: a stall this long is past anything
// its ceiling could cover anyway.
inline constexpr int kAudioStreamStoppedMs = 500;

// The game running slow -- an emulator that cannot keep up for a while -- is
// the third way the two clocks part, and the one none of AudioOutput's drift
// correction, trimming or cover is for. A
// console at eighty percent speed makes eight-tenths of a second of sound a
// second, so the queue runs dry every frame or two: a dry spell, a fade back
// in, another dry spell, a dozen times a second. That is not heard as gaps, it
// is heard as the sound itself being distorted -- a buzz on every note for as
// long as the slowdown lasts. Cover cannot help, since there is never anything
// to fill it with, and the rate correction is held to a percent on purpose.
//
// So the sound is slowed down with the game instead, the way Citra's and
// Dolphin's "audio stretching" do it: stretched in time with the pitch left
// where it was (see AudioStretcher), so a game at eighty percent sounds like
// a game at eighty percent rather than like a broken speaker.
//
// Measured over this much wall clock, which is long enough that a frame or
// two arriving a few milliseconds early or late moves the reading by a
// percent or two, and short enough that a slowdown is caught within about two
// of these.
inline constexpr int kAudioSpeedWindowMs = 500;
// Stretching starts once the source falls under this fraction of real time,
// and stops once it is back above the second. Apart, so a source hovering
// near one does not flip between the two. The gap between the first and full
// speed is ordinary drift, and left to the rate correction and the cover.
inline constexpr double kAudioStretchBelowSpeed = 0.94;
inline constexpr double kAudioStretchUntilSpeed = 0.98;
// The most a block is ever stretched by. A game at a quarter speed is not one
// anyone is going to be playing for its sound, and WSOLA much past this is
// more echo than music.
inline constexpr double kAudioMaxStretch = 4.0;
// AudioStretcher's half-window, and how far either side of where a window
// would be taken from it looks for a better place. A window of 30ms holds a
// few cycles of anything but the lowest notes, which is what the search needs
// to line them up by; the search covers one cycle of anything above about
// 60Hz.
inline constexpr int kAudioStretchHopMs = 15;
inline constexpr int kAudioStretchSearchMs = 8;

// How fast a source is sending audio, as a fraction of real time: frames
// handed in over the last kAudioSpeedWindowMs of wall clock, against the rate
// they claim to be at. 1 for a source keeping up, 0.8 for a game running at
// eighty percent speed.
//
// A wait between pushes of kAudioStreamStoppedMs or more is the stream
// stopping -- a menu up, the game paused -- not the game running slowly, and
// starts the measurement again rather than reading as a speed near zero.
class AudioSourceSpeed {
public:
    void reset();
    void note(uint64_t now_ms, std::size_t frames);
    // 1 until a window's worth has been seen.
    double speed(double sample_rate) const;

private:
    struct Push {
        uint64_t at_ms;
        std::size_t frames;
    };
    std::vector<Push> pushes_;
};

// How much to stretch by while a source runs at `speed` of real time: enough
// to make up the difference, nudged by how far the queue is from where it is
// aimed, so an estimate of the speed a percent or two out does not leave it to
// drift dry or deep. Between 0.9 and kAudioMaxStretch.
double audio_stretch_for(double speed, std::size_t queued_frames, std::size_t aimed_frames);

// Time-stretching for stereo int16 that leaves the pitch alone: WSOLA, the
// same idea audio_splice() uses to take a few milliseconds out, run the other
// way to put time in.
//
// The output is built from overlapping windows of the input, each half a
// window on from the last; to make the sound longer, each is taken from less
// than half a window further on in the input. Taken exactly there, the
// overlapping windows would be out of step with each other and the result a
// sound and its own echo, so each is moved to wherever within
// kAudioStretchSearchMs it best lines up with the audio that would naturally
// have followed the previous one. The overlap is a raised-cosine crossfade
// whose two halves sum to one, so a stretch of exactly one is the input again.
//
// It holds back a window and a search's worth of input to look ahead into.
// finish() hands that back unstretched, continuing seamlessly from where the
// stretched output left off, for when the stretching stops.
class AudioStretcher {
public:
    // Resets, and sizes the windows for audio at `sample_rate`.
    void reset(double sample_rate);
    // Appends `frames` of input stretched to about `stretch` times as long.
    // Above 1 makes it longer; below 1 is allowed too, and shortens it.
    void process(const int16_t* interleaved_stereo, std::size_t frames, double stretch,
                 std::vector<int16_t>& out);
    // Appends everything still held, unstretched, and starts over.
    void finish(std::vector<int16_t>& out);
    // Whether anything is held back, which finish() would hand over.
    bool holding() const { return started_ || !input_.empty(); }

private:
    void step(std::vector<int16_t>& out);

    std::size_t hop_ = 0;
    std::size_t search_ = 0;
    // Input not yet passed over, as floats; input_[0] is frame offset_ of
    // the stream since reset().
    std::vector<float> input_;
    std::size_t offset_ = 0;
    // Where the next window is taken from, before the search moves it.
    double position_ = 0.0;
    // Where the last one was taken from, and the half of it still to be
    // overlapped by the next, already faded out.
    std::size_t previous_ = 0;
    std::vector<float> pending_;
    bool started_ = false;
};

}  // namespace ds
