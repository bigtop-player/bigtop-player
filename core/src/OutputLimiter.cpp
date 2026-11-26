// OutputLimiter.cpp: the look-ahead peak limiter on the final output.
#include "bigtop/OutputLimiter.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "bigtop/Gain.h"

namespace bigtop {

OutputLimiter::OutputLimiter(double sampleRate, double ceilingDb, double lookaheadSeconds, double releaseSeconds)
    // A negative lookahead would mean a negative-sized delay line: treat it as none.
    : delay_(static_cast<int>(std::llround(std::max(0.0, lookaheadSeconds) * sampleRate))),
      ceiling_(static_cast<float>(dbToGain(ceilingDb))),
      releaseCoef_(std::exp(-1.0 / (releaseSeconds * sampleRate))),
      delayL_(static_cast<size_t>(delay_)), delayR_(static_cast<size_t>(delay_)),
      winGain_(static_cast<size_t>(delay_) + 1), winFrame_(static_cast<size_t>(delay_) + 1) {
    assert(sampleRate > 0.0 && releaseSeconds > 0.0);
}

void OutputLimiter::reset() noexcept {
    std::fill(delayL_.begin(), delayL_.end(), 0.0f);
    std::fill(delayR_.begin(), delayR_.end(), 0.0f);
    delayPos_ = winHead_ = winSize_ = 0;
    frame_ = 0;
    gain_ = 1.0;
    step_ = 0.0;
    reductionDb_.store(0.0f, std::memory_order_relaxed);
    peakReductionDb_.store(0.0f, std::memory_order_relaxed);
}

void OutputLimiter::process(float* left, float* right, int frames) noexcept {
    const int cap = delay_ + 1;  // window: the frame leaving now plus the delay_ still inside
    double lowestGain = gain_;   // the block's deepest reduction, for takePeakReductionDb()
    for (int i = 0; i < frames; ++i) {
        float l = std::isfinite(left[i]) ? left[i] : 0.0f;
        float r = std::isfinite(right[i]) ? right[i] : 0.0f;

        // Linked stereo: one gain from the louder channel.
        const float peak = std::max(std::abs(l), std::abs(r));
        const float need = peak > ceiling_ ? ceiling_ / peak : 1.0f;

        // Sliding-window minimum: first drop the front if its frame has left
        // the window (before the push, so the ring of cap slots never holds
        // more than cap entries), then drop entries that can never be the
        // minimum again (not smaller than the newcomer), then push it.
        if (winSize_ > 0 && winFrame_[static_cast<size_t>(winHead_)] <= frame_ - cap) { winHead_ = (winHead_ + 1) % cap; --winSize_; }
        while (winSize_ > 0 && winGain_[static_cast<size_t>((winHead_ + winSize_ - 1) % cap)] >= need) --winSize_;
        const size_t back = static_cast<size_t>((winHead_ + winSize_) % cap);
        winGain_[back] = need; winFrame_[back] = frame_; ++winSize_;
        const double target = winGain_[static_cast<size_t>(winHead_)];

        // A newcomer needing less gain than now leaves the delay line after
        // cap more gain updates, so the ramp must be at least this steep. Taking
        // the max keeps every earlier, closer deadline met as well.
        if (need < gain_) step_ = std::max(step_, (gain_ - need) / cap);
        if (gain_ > target) {
            gain_ = std::max(gain_ - step_, target);
        } else {
            // Reached (or above) everything pending: stop ramping, release
            // exponentially toward the window minimum, snapping to it at the end
            // so an idle limiter has gain exactly 1 and is bit-transparent.
            step_ = 0.0;
            gain_ = target - (target - gain_) * releaseCoef_;
            if (target - gain_ < 1e-6) gain_ = target;
        }

        // Swap the new sample into the delay line and take out the oldest.
        if (delay_ > 0) {
            std::swap(l, delayL_[static_cast<size_t>(delayPos_)]);
            std::swap(r, delayR_[static_cast<size_t>(delayPos_)]);
            delayPos_ = (delayPos_ + 1) % delay_;
        }
        // Final guard: rounding at the end of a ramp must never pass the ceiling.
        lowestGain = std::min(lowestGain, gain_);
        const float g = static_cast<float>(gain_);
        left[i] = std::clamp(l * g, -ceiling_, ceiling_);
        right[i] = std::clamp(r * g, -ceiling_, ceiling_);
        ++frame_;
    }
    // max with 0: at unity -gainToDb gives -0.0, which should read as plain 0.
    reductionDb_.store(std::max(0.0f, static_cast<float>(-gainToDb(gain_))), std::memory_order_relaxed);
    const float blockPeakDb = std::max(0.0f, static_cast<float>(-gainToDb(lowestGain)));
    if (blockPeakDb > peakReductionDb_.load(std::memory_order_relaxed))
        peakReductionDb_.store(blockPeakDb, std::memory_order_relaxed);
}

}  // namespace bigtop
