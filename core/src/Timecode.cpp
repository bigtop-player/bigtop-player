// Implementation of Timecode.h. See the header for the contracts.
#include "bigtop/Timecode.h"

#include <cmath>

namespace bigtop {

bool isSupportedFps(int fps) noexcept { return fps == 24 || fps == 25 || fps == 30; }

Timecode timecodeAt(Timecode start, double seconds, int fps) noexcept {
    // Non-positive fps would divide by zero below; there is no sensible answer,
    // so hand back the start value (this runs live and must not crash).
    if (fps <= 0) return start;
    // Work in whole frames (int64) so wrapping is exact modular arithmetic.
    const int64_t dayFrames = int64_t{24} * 3600 * fps;
    const int64_t startFrames =
        ((int64_t{start.h} * 60 + start.m) * 60 + start.s) * fps + start.f;
    // Negative time (e.g. before playback starts) counts as 0. The epsilon
    // guards against values like 84.48 * 25 = 2111.9999999999995.
    // NaN, +-inf and products that overflow to inf would make fmod return NaN
    // and the int64 cast undefined, so they count as 0 elapsed too.
    const double product = seconds * fps;
    const double elapsed =
        (std::isfinite(product) && product > 0.0) ? std::floor(product + 1e-9) : 0.0;
    // Reduce the elapsed frames before adding so the sum cannot overflow.
    int64_t total = (startFrames + static_cast<int64_t>(std::fmod(elapsed, static_cast<double>(dayFrames)))) % dayFrames;
    Timecode tc;
    tc.f = static_cast<int>(total % fps);
    total /= fps;
    tc.s = static_cast<int>(total % 60);
    total /= 60;
    tc.m = static_cast<int>(total % 60);
    tc.h = static_cast<int>(total / 60);
    return tc;
}

ArtTimeCodePacket encodeArtTimeCode(Timecode tc, int fps) noexcept {
    ArtTimeCodePacket p{};  // zero-initialised: bytes 12-13 stay 0
    const char id[8] = {'A', 'r', 't', '-', 'N', 'e', 't', '\0'};
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(id[i]);
    p[8] = 0x00;  // opcode 0x9700, low byte first
    p[9] = 0x97;
    p[10] = 0;    // protocol version 14, high byte first
    p[11] = 14;
    p[14] = static_cast<uint8_t>(tc.f);
    p[15] = static_cast<uint8_t>(tc.s);
    p[16] = static_cast<uint8_t>(tc.m);
    p[17] = static_cast<uint8_t>(tc.h);
    p[18] = fps == 24 ? 0 : fps == 30 ? 3 : 1;  // anything else -> 25 fps
    return p;
}

}  // namespace bigtop
