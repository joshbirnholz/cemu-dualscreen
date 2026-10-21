#include "audio/audio_stretch.hpp"

#include <algorithm>
#include <cmath>

namespace ds {

double audio_stretch_for(double speed, std::size_t queued_frames, std::size_t aimed_frames) {
    // As long as the game is slow by, and then nudged by how far the queue is
    // from where it is aimed. Unlike the rate correction this nudge is free to
    // be large: stretching moves tempo, not pitch, and a game already running
    // slow is not going to be heard as a few percent slower still -- while the
    // measured speed is only good to a percent or two, which on its own would
    // leave the queue to drift.
    const double base = 1.0 / std::max(speed, 1.0 / kAudioMaxStretch);
    const double aimed = static_cast<double>(std::max<std::size_t>(aimed_frames, 1));
    const double nudge =
        std::clamp(1.0 + 0.5 * (aimed - static_cast<double>(queued_frames)) / aimed, 0.85, 1.25);
    return std::clamp(base * nudge, 0.9, kAudioMaxStretch);
}

void AudioSourceSpeed::reset() { pushes_.clear(); }

void AudioSourceSpeed::note(uint64_t now_ms, std::size_t frames) {
    if (!pushes_.empty() &&
        now_ms - pushes_.back().at_ms >= static_cast<uint64_t>(kAudioStreamStoppedMs)) {
        pushes_.clear();
    }
    pushes_.push_back({now_ms, frames});
    // Keep one push from before the window began: what is measured is the
    // frames since it, over the time since it.
    std::size_t first = 0;
    while (first + 1 < pushes_.size() &&
           now_ms - pushes_[first + 1].at_ms >= static_cast<uint64_t>(kAudioSpeedWindowMs)) {
        ++first;
    }
    pushes_.erase(pushes_.begin(), pushes_.begin() + static_cast<std::ptrdiff_t>(first));
}

double AudioSourceSpeed::speed(double sample_rate) const {
    if (pushes_.size() < 2 || sample_rate <= 0.0) {
        return 1.0;
    }
    const uint64_t span_ms = pushes_.back().at_ms - pushes_.front().at_ms;
    if (span_ms < static_cast<uint64_t>(kAudioSpeedWindowMs)) {
        return 1.0;
    }
    std::size_t frames = 0;
    for (std::size_t i = 1; i < pushes_.size(); ++i) {
        frames += pushes_[i].frames;
    }
    return static_cast<double>(frames) * 1000.0 / static_cast<double>(span_ms) / sample_rate;
}

namespace {

// The rising half of the raised-cosine crossfade; the falling half is one
// minus it, so the two always sum to one.
float stretch_rise(std::size_t i, std::size_t hop) {
    const double s = std::sin(1.5707963267948966 * (static_cast<double>(i) + 0.5) /
                              static_cast<double>(hop));
    return static_cast<float>(s * s);
}

int16_t to_sample(float value) {
    return static_cast<int16_t>(std::lround(std::clamp(static_cast<double>(value), -32768.0, 32767.0)));
}

}  // namespace

void AudioStretcher::reset(double sample_rate) {
    const double rate = sample_rate > 0.0 ? sample_rate : 48000.0;
    hop_ = std::max<std::size_t>(64, static_cast<std::size_t>(rate * kAudioStretchHopMs / 1000.0));
    search_ = static_cast<std::size_t>(rate * kAudioStretchSearchMs / 1000.0);
    input_.clear();
    offset_ = 0;
    position_ = 0.0;
    previous_ = 0;
    pending_.clear();
    started_ = false;
}

void AudioStretcher::process(const int16_t* interleaved_stereo, std::size_t frames, double stretch,
                             std::vector<int16_t>& out) {
    if (interleaved_stereo == nullptr || frames == 0) {
        return;
    }
    if (hop_ == 0) {
        reset(48000.0);
    }
    input_.insert(input_.end(), interleaved_stereo, interleaved_stereo + frames * 2);
    stretch = std::clamp(stretch, 0.5, kAudioMaxStretch);

    if (!started_) {
        // Begun as though it had been running at a stretch of one all along,
        // which would have played the input straight through: the first half
        // window goes out as it is, and the second is what the next window
        // overlaps. So starting is no more an edge than finishing is.
        if (input_.size() / 2 < hop_ * 2) {
            return;
        }
        for (std::size_t i = 0; i < hop_ * 2; ++i) {
            // hop_ frames, which is twice that many samples.
            out.push_back(to_sample(input_[i]));
        }
        pending_.assign(hop_ * 2, 0.0f);
        for (std::size_t i = 0; i < hop_; ++i) {
            const float fall = 1.0f - stretch_rise(i, hop_);
            pending_[i * 2] = input_[(hop_ + i) * 2] * fall;
            pending_[i * 2 + 1] = input_[(hop_ + i) * 2 + 1] * fall;
        }
        previous_ = offset_;
        position_ = static_cast<double>(offset_);
        started_ = true;
    }

    for (;;) {
        // Read in whole frames from here on: offset_ + held is the end of what
        // has arrived.
        const std::size_t held = input_.size() / 2;
        const std::size_t end = offset_ + held;
        position_ += static_cast<double>(hop_) / stretch;
        const auto nominal = static_cast<std::size_t>(std::max(0.0, std::floor(position_)));
        // Room to search past the nominal place and take a whole window from
        // the furthest candidate, and to read what would have followed the
        // previous window.
        const std::size_t needed =
            std::max(nominal + search_ + hop_ * 2, previous_ + hop_ * 2);
        if (needed > end) {
            position_ -= static_cast<double>(hop_) / stretch;
            break;
        }
        step(out);
    }
}

void AudioStretcher::step(std::vector<int16_t>& out) {
    const std::size_t nominal = static_cast<std::size_t>(std::max(0.0, std::floor(position_)));
    const auto at = [this](std::size_t frame, int channel) {
        return input_[(frame - offset_) * 2 + static_cast<std::size_t>(channel)];
    };
    const auto mono = [&](std::size_t frame) { return at(frame, 0) + at(frame, 1); };

    // What would naturally have followed the previous window, and the place
    // near `nominal` whose start looks most like it.
    const std::size_t natural = previous_ + hop_;
    const std::size_t lowest = std::max(offset_, nominal > search_ ? nominal - search_ : 0);
    const std::size_t highest = nominal + search_;
    std::size_t chosen = std::clamp(nominal, lowest, highest);
    double best = -2.0;
    for (std::size_t candidate = lowest; candidate <= highest; ++candidate) {
        double together = 0.0;
        double energy = 0.0;
        double reference = 0.0;
        // Every other frame: the shape is all that is being matched, and it
        // halves the cost of the one part of this that costs anything.
        for (std::size_t i = 0; i < hop_; i += 2) {
            const double a = mono(natural + i);
            const double b = mono(candidate + i);
            together += a * b;
            energy += b * b;
            reference += a * a;
        }
        const double score =
            energy > 0.0 && reference > 0.0 ? together / std::sqrt(energy * reference) : 0.0;
        // Ties go to the nominal place, so silence keeps the timing honest.
        const double nearness =
            1e-6 * std::abs(static_cast<double>(candidate) - static_cast<double>(nominal));
        if (score - nearness > best) {
            best = score - nearness;
            chosen = candidate;
        }
    }

    for (std::size_t i = 0; i < hop_; ++i) {
        const float rise = stretch_rise(i, hop_);
        for (int channel = 0; channel < 2; ++channel) {
            out.push_back(to_sample(pending_[i * 2 + channel] + at(chosen + i, channel) * rise));
            pending_[i * 2 + channel] = at(chosen + hop_ + i, channel) * (1.0f - rise);
        }
    }
    previous_ = chosen;

    // Nothing before the earliest frame a later step can still read is needed
    // again: the next search starts no earlier than a search before the
    // position, and the next natural continuation is past previous_.
    const std::size_t next_nominal = static_cast<std::size_t>(std::max(0.0, std::floor(position_)));
    const std::size_t keep_from =
        std::min(previous_ + hop_, next_nominal > search_ ? next_nominal - search_ : 0);
    if (keep_from > offset_) {
        const std::size_t drop = std::min(keep_from - offset_, input_.size() / 2);
        input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(drop * 2));
        offset_ += drop;
    }
}

void AudioStretcher::finish(std::vector<int16_t>& out) {
    if (!started_) {
        // Never had enough to start: all of it is still here, untouched.
        for (const float sample : input_) {
            out.push_back(to_sample(sample));
        }
    } else {
        // The pending half is the input from previous_ + hop_ on, faded out;
        // the same input faded in completes it, since the two fades sum to
        // one -- and from there on the input is simply played.
        const std::size_t from = previous_ + hop_;
        const std::size_t end = offset_ + input_.size() / 2;
        for (std::size_t frame = from; frame < end; ++frame) {
            const std::size_t i = frame - from;
            for (int channel = 0; channel < 2; ++channel) {
                const float value = input_[(frame - offset_) * 2 + static_cast<std::size_t>(channel)];
                out.push_back(i < hop_ ? to_sample(pending_[i * 2 + channel] +
                                                   value * stretch_rise(i, hop_))
                                       : to_sample(value));
            }
        }
    }
    input_.clear();
    pending_.clear();
    offset_ = 0;
    position_ = 0.0;
    previous_ = 0;
    started_ = false;
}

}  // namespace ds
