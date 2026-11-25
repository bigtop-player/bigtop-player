/// @file Gain.h
/// @brief Decibel/linear gain conversion and the Fader used for every level
///        change in the engine (fade in, fade out, level changes between steps).
///
/// Pure arithmetic: no JUCE, no allocation, no locks, so everything here is
/// safe to call from the audio thread.
#pragma once

#include <cmath>
#include <cstdint>

namespace bigtop {

/// @brief Level in dB at or below which a signal is treated as silence.
///
/// Also the value gainToDb() returns for zero gain, so "silent" has one
/// representation in both directions and we never produce -infinity.
constexpr double kSilenceDb = -100.0;

/// @brief Converts decibels to linear gain (0 dB -> 1.0, -6.02 dB -> ~0.5).
/// @param db Level in decibels. Anything at or below kSilenceDb yields exactly 0.0.
/// @return Linear gain, >= 0.
/// @note Audio thread safe: no allocation, no locks.
double dbToGain(double db);

/// @brief Converts linear gain to decibels.
/// @param gain Linear gain. Gain <= 0 returns kSilenceDb.
/// @return Level in dB, never below kSilenceDb (smaller results are clamped to
///         it), so dbToGain(gainToDb(g)) never misbehaves.
/// @note Audio thread safe: no allocation, no locks.
double gainToDb(double gain);

/// @brief A gain ramp (linear gain, 0..1+) from one value to another over a
///        number of audio frames, using an equal-power curve.
///
/// Why equal-power: a rising and a falling fade of equal length use sin and cos
/// of the same angle, so their squares sum to 1 (sin^2 + cos^2). Cross-fading
/// two sounds therefore keeps constant perceived loudness, with no dip in the
/// middle as a straight linear cross-fade would give.
///
/// @note Threading: all members are noexcept and allocation-free; next() is
///       meant to be called once per audio frame on the audio thread. fadeTo()
///       is also cheap and may be called from the audio thread when a command
///       is applied.
class Fader {
public:
    /// @brief Starts steady at a given gain.
    /// @param gain Initial linear gain; default 0 = silent.
    explicit Fader(float gain = 0.0f) noexcept : from_(gain), to_(gain) {}

    /// @brief Begins a fade to a target gain, starting from wherever the fader
    ///        is right now (so retargeting mid-fade never jumps).
    /// @param targetGain Linear gain to reach.
    /// @param frames Fade length in audio frames; <= 0 jumps to the target
    ///        immediately (the next call returns it).
    /// @note Audio thread safe.
    void fadeTo(float targetGain, int64_t frames) noexcept {
        from_ = current();
        to_ = targetGain;
        frames_ = frames > 0 ? frames : 0;
        elapsed_ = 0;
    }

    /// @brief Returns the gain for this frame and advances one frame.
    /// @return Linear gain. For a fade of N frames, calls 1..N return ramp
    ///         values; the call after the final ramp frame (the N+1th) returns
    ///         the target exactly.
    /// @note Call exactly once per audio frame, on the audio thread.
    float next() noexcept {
        const float g = current();
        if (elapsed_ < frames_) ++elapsed_;
        return g;
    }

    /// @brief Multiplies both ends of the fade (or the steady gain) by a factor,
    ///        keeping the fade's progress, so current() is scaled exactly and
    ///        the rest of the curve keeps its shape.
    /// @param factor [in] Linear factor, >= 0.
    /// @param ceiling [in] Neither end is raised above this linear gain; when
    ///        the cap bites, current() is no longer exactly scaled.
    /// @note Audio thread safe.
    void scale(float factor, float ceiling) noexcept {
        from_ = std::fmin(from_ * factor, ceiling);
        to_ = std::fmin(to_ * factor, ceiling);
    }

    /// @brief Gain at the current position, without advancing.
    /// @return Linear gain.
    float current() const noexcept {
        // Finished (or zero-length) fade: return the target exactly, avoiding
        // float rounding from the sin/cos at the end of the ramp.
        if (elapsed_ >= frames_) return to_;
        // a runs 0 .. pi/2 (1.5707963 = pi/2) across the fade.
        const float a = 1.5707963f * static_cast<float>(elapsed_) / static_cast<float>(frames_);
        // Rising uses sin (fast start, flattening at the end); falling uses cos
        // (slow start, fast end), the mirror image, so a matching pair keeps
        // sin^2 + cos^2 = 1.
        return to_ >= from_ ? from_ + (to_ - from_) * std::sin(a)
                            : to_ + (from_ - to_) * std::cos(a);
    }

    /// @brief The gain this fader is heading for (or sitting at).
    /// @return Linear gain.
    float target() const noexcept { return to_; }

    /// @brief Whether a fade is still in progress.
    /// @return True while a fade has not yet reached its target.
    bool moving() const noexcept { return elapsed_ < frames_; }

private:
    float from_;  ///< @brief Linear gain at the start of the fade.
    float to_;    ///< @brief Linear gain at the end of the fade.
    int64_t frames_ = 0;   ///< @brief Fade length, in audio frames.
    int64_t elapsed_ = 0;  ///< @brief Fade progress, in audio frames.
};

}  // namespace bigtop
