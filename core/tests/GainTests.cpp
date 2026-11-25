#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "bigtop/Gain.h"

using Catch::Approx;
using namespace bigtop;

// Unity, half amplitude, and the -100 dB silence floor in both directions.
TEST_CASE("dB conversion") {
    CHECK(dbToGain(0.0) == Approx(1.0));
    CHECK(dbToGain(-6.0206) == Approx(0.5).epsilon(1e-4));
    CHECK(dbToGain(-100.0) == 0.0);      // at or below -100 dB is silence
    CHECK(gainToDb(0.5) == Approx(-6.0206).epsilon(1e-4));
    CHECK(gainToDb(0.0) == -100.0);
}

TEST_CASE("Fader jumps when frames is zero") {
    Fader f(0.0f); f.fadeTo(1.0f, 0);
    CHECK(f.next() == 1.0f); CHECK_FALSE(f.moving());
}
// Midpoint of a rising fade is sin(pi/4) ~ 0.7071, and the end is exactly the target.
TEST_CASE("Fader rises with equal-power shape and lands exactly") {
    Fader f(0.0f); f.fadeTo(1.0f, 1000);
    CHECK(f.next() == 0.0f);
    for (int i = 1; i < 500; ++i) f.next();
    CHECK(f.next() == Approx(0.70711f).margin(1e-3));   // frame 500
    for (int i = 501; i < 1000; ++i) f.next();
    CHECK(f.next() == 1.0f); CHECK_FALSE(f.moving());
}
// A cross-fade pair must sum to constant power (a^2 + b^2 = 1) at every frame.
TEST_CASE("Rising and falling faders keep constant power") {
    Fader in(0.0f), out(1.0f); in.fadeTo(1.0f, 1000); out.fadeTo(0.0f, 1000);
    for (int i = 0; i < 1000; ++i) { float a = in.next(), b = out.next();
        CHECK(a*a + b*b == Approx(1.0f).margin(1e-3)); }
}
// Retargeting mid-fade begins at the current gain, so there is no audible jump.
TEST_CASE("A new fadeTo starts from the current gain") {
    Fader f(1.0f); f.fadeTo(0.0f, 1000);
    for (int i = 0; i < 500; ++i) f.next();
    float mid = f.current(); f.fadeTo(1.0f, 0);
    CHECK(mid == Approx(0.70711f).margin(2e-3)); CHECK(f.next() == 1.0f);
}
