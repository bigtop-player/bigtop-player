/// @file Timecode.h
/// @brief Timecode arithmetic and the Art-Net ArtTimeCode packet encoder.
///
/// Lighting consoles follow the music via Art-Net timecode. This file holds the
/// pure parts: computing the timecode at a playback position and building the
/// 19-byte packet. Sending it over UDP is done elsewhere (network sender).
/// No JUCE, no OS headers, no allocation, no exceptions.
#pragma once

#include <array>
#include <cstdint>

#include "bigtop/Show.h"

namespace bigtop {

/// @brief Whether a frame rate can be carried by ArtTimeCode.
/// @param fps Frames per second.
/// @return True for 24, 25 and 30 (the rates ArtTimeCode defines here).
/// @note Any thread; no allocation.
bool isSupportedFps(int fps) noexcept;

/// @brief Timecode reached after a start value plus elapsed playback time.
/// @param start Timecode at playback position 0.
/// @param seconds Elapsed playback time in seconds; negative, NaN, infinite
///        values (and values whose product with fps overflows) are treated
///        as 0.
/// @param fps Frames per second.
/// @pre fps satisfies isSupportedFps(). If fps <= 0 the function returns
///      start unchanged; other unsupported values give unspecified (but safe)
///      results.
/// @pre start fields are non-negative; negative fields are not supported.
/// @note start is normalised by carry: f >= fps and h >= 24 are accepted and
///       folded into the 24-hour result.
/// @return start + floor(seconds * fps) frames, wrapped at 24 hours (so
///         23:59:59:xx plus one second gives 00:00:00:xx).
/// @note Any thread; no allocation, never throws. A tiny epsilon (1e-9 frames)
///       absorbs floating-point error so 84.48 s at 25 fps lands exactly on
///       frame 2112 instead of 2111.
Timecode timecodeAt(Timecode start, double seconds, int fps) noexcept;

/// @brief The 19 bytes of an ArtTimeCode packet.
using ArtTimeCodePacket = std::array<uint8_t, 19>;

/// @brief Builds an ArtTimeCode packet.
///
/// Layout: bytes 0-7 "Art-Net\0", 8-9 opcode 0x9700 low byte first, 10-11
/// protocol version 0,14, 12-13 zero (filler), 14 frames, 15 seconds,
/// 16 minutes, 17 hours, 18 type (0 = 24 fps, 1 = 25 fps, 3 = 30 fps).
/// @param tc Timecode to send; fields are copied as-is (not range-checked).
/// @param fps Frames per second.
/// @return The packet, ready to send as one UDP datagram (port 6454).
/// @warning Callers must check isSupportedFps() first: an unsupported fps is
///          encoded as type 1 (25 fps) rather than failing.
/// @note Any thread; no allocation, never throws.
ArtTimeCodePacket encodeArtTimeCode(Timecode tc, int fps) noexcept;

}  // namespace bigtop
