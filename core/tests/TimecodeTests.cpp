// Tests for timecode arithmetic and the ArtTimeCode packet encoder.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#include "bigtop/Timecode.h"

using namespace bigtop;

namespace {
// Test-side decoder: validates header, opcode and version, then returns the
// timecode and fps. Independent of the encoder so the round trip means something.
std::optional<std::pair<Timecode, int>> decodeArtTimeCode(const ArtTimeCodePacket& p) {
    if (std::memcmp(p.data(), "Art-Net\0", 8) != 0) return std::nullopt;
    if (p[8] != 0x00 || p[9] != 0x97) return std::nullopt;
    if (p[10] != 0 || p[11] != 14) return std::nullopt;
    int fps = 0;
    switch (p[18]) {
        case 0: fps = 24; break;
        case 1: fps = 25; break;
        case 3: fps = 30; break;
        default: return std::nullopt;
    }
    return std::make_pair(Timecode{p[17], p[16], p[15], p[14]}, fps);
}
}  // namespace

TEST_CASE("timecodeAt adds elapsed time to the start value") {
    CHECK(timecodeAt({1,0,0,0}, 84.48, 25) == Timecode{1,1,24,12});      // 01:01:24:12
    CHECK(timecodeAt({0,0,0,0}, 0.999, 25) == Timecode{0,0,0,24});
    CHECK(timecodeAt({23,59,59,0}, 1.0, 25) == Timecode{0,0,0,0});      // wraps
    CHECK(timecodeAt({0,0,0,10}, 0.5, 30) == Timecode{0,0,0,25});
}
TEST_CASE("timecodeAt treats negative seconds as zero") {
    CHECK(timecodeAt({1,2,3,4}, -5.0, 25) == Timecode{1,2,3,4});
}
TEST_CASE("timecodeAt treats non-finite time as zero elapsed") {
    const double inf = std::numeric_limits<double>::infinity();
    const Timecode s{1,2,3,4};
    CHECK(timecodeAt(s, inf, 25) == s);
    CHECK(timecodeAt(s, -inf, 25) == s);
    CHECK(timecodeAt(s, std::numeric_limits<double>::quiet_NaN(), 25) == s);
    CHECK(timecodeAt(s, 1e308, 25) == s);  // finite, but seconds * fps overflows
}
TEST_CASE("timecodeAt returns start unchanged for non-positive fps") {
    CHECK(timecodeAt({1,2,3,4}, 10.0, 0) == Timecode{1,2,3,4});
    CHECK(timecodeAt({1,2,3,4}, 10.0, -25) == Timecode{1,2,3,4});
}
TEST_CASE("Encoded packets decode to the same timecode") {
    for (int fps : {24, 25, 30}) {
        auto d = decodeArtTimeCode(encodeArtTimeCode({1,2,3,4}, fps));
        REQUIRE(d); CHECK(d->first == Timecode{1,2,3,4}); CHECK(d->second == fps);
    }
}
TEST_CASE("Packet bytes match the Art-Net layout") {
    auto p = encodeArtTimeCode({17,16,15,14}, 25);
    CHECK(std::memcmp(p.data(), "Art-Net\0", 8) == 0);
    CHECK(p[10] == 0); CHECK(p[12] == 0); CHECK(p[13] == 0);
    CHECK(p[8] == 0x00); CHECK(p[9] == 0x97); CHECK(p[11] == 14);
    CHECK(p[14] == 14); CHECK(p[15] == 15); CHECK(p[16] == 16); CHECK(p[17] == 17); CHECK(p[18] == 1);
}
TEST_CASE("Unsupported fps is flagged and encodes as 25 fps") {
    CHECK(isSupportedFps(24)); CHECK(isSupportedFps(25)); CHECK(isSupportedFps(30));
    CHECK_FALSE(isSupportedFps(29)); CHECK_FALSE(isSupportedFps(0));
    CHECK(encodeArtTimeCode({0,0,0,0}, 60)[18] == 1);
}
