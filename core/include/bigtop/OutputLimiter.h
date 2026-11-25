/// @file OutputLimiter.h
/// @brief The lookahead brickwall limiter on the final stereo mix, just before
///        the sound card. It protects the PA: inaudible while the mix stays
///        below the ceiling, and the output never goes above it.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace bigtop {

/// @brief Linked-stereo lookahead peak limiter with a hard ceiling.
///
/// The signal is delayed by latencyFrames() so the gain can start moving down
/// before a peak reaches the output; the gain falls linearly to exactly what the
/// peak needs and recovers exponentially afterwards. Both channels always get
/// the same gain, so the stereo image never shifts. While nothing exceeds the
/// ceiling the gain is exactly 1, so the signal passes bit for bit (only delayed).
///
/// @note Threading: construct and reset() on the control thread (or while audio
///       is stopped); process() on the audio thread only; gainReductionDb() from
///       any thread.
class OutputLimiter {
public:
    /// @brief Builds a limiter and allocates all its storage.
    /// @param sampleRate Audio rate in Hz; must be > 0 (asserted).
    /// @param ceilingDb Highest output peak, in dBFS (default -0.3 dBFS).
    /// @param lookaheadSeconds How far ahead peaks are seen, and so the added
    ///        latency, in seconds; negative values are treated as 0.
    /// @param releaseSeconds Time constant of the gain recovery, in seconds; must be > 0 (asserted).
    /// @note Allocates; call on the control thread, never on the audio thread.
    explicit OutputLimiter(double sampleRate, double ceilingDb = -0.3,
                           double lookaheadSeconds = 0.0015, double releaseSeconds = 0.1);

    /// @brief Limits one block of stereo audio in place.
    /// @param left Left channel, @p frames samples; overwritten with the output.
    /// @param right Right channel, @p frames samples; overwritten with the output.
    /// @param frames Number of frames; <= 0 does nothing.
    /// @note Audio thread only. No allocation, no locks, never throws. The output
    ///       is the input delayed by latencyFrames(); any block size gives the
    ///       same output. Non-finite input samples come out as 0, and every output
    ///       sample is clamped to +/- the ceiling as a final guard.
    void process(float* left, float* right, int frames) noexcept;

    /// @brief The delay the limiter adds, in frames: llround(lookaheadSeconds * sampleRate).
    /// @return Latency in frames, >= 0.
    int latencyFrames() const noexcept { return delay_; }

    /// @brief How much the limiter is turning the level down right now.
    /// @return Gain reduction in dB at the end of the last process() block:
    ///         0 when not limiting, positive otherwise.
    /// @note Any thread (atomic). Meant for a meter or a "limiting" light.
    float gainReductionDb() const noexcept { return reductionDb_.load(std::memory_order_relaxed); }

    /// @brief Clears the delay line and the gain state, as if just constructed.
    /// @note Not concurrent with process(): call while audio is stopped.
    void reset() noexcept;

private:
    int delay_;           ///< @brief Lookahead, in frames.
    float ceiling_;       ///< @brief Ceiling as linear gain.
    double releaseCoef_;  ///< @brief Per-frame factor on the remaining distance to the target during release.

    std::vector<float> delayL_, delayR_;  ///< @brief Delay lines, delay_ frames each.
    int delayPos_ = 0;                    ///< @brief Oldest slot in the delay lines (read, then overwritten).

    /// @brief Monotonic queue for the sliding-window minimum of required gains
    ///        over the last delay_ + 1 frames (a ring of delay_ + 1 entries).
    std::vector<float> winGain_;
    std::vector<int64_t> winFrame_;  ///< @brief Frame number each winGain_ entry came from.
    int winHead_ = 0;                ///< @brief Ring index of the front (the window minimum).
    int winSize_ = 0;                ///< @brief Entries in the queue.
    int64_t frame_ = 0;              ///< @brief Frames processed since construction or reset().

    double gain_ = 1.0;  ///< @brief Gain applied to the sample leaving the delay line.
    double step_ = 0.0;  ///< @brief Per-frame decrease of the current downward ramp; 0 when not ramping.
    std::atomic<float> reductionDb_{0.0f};  ///< @brief Published gain reduction, dB.
    // The audio thread writes it every block: it must never fall back to a lock.
    static_assert(std::atomic<float>::is_always_lock_free);
};

}  // namespace bigtop
