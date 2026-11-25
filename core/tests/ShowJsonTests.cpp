#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "bigtop/Engine.h"  // kMaxChanges
#include "bigtop/ShowJson.h"

using namespace bigtop;

// A step with a sound fades others out by the default fade; one without keeps them.
TEST_CASE("makeStep applies the spec defaults") {
    Settings s;
    CHECK(makeStep(1, s, Sound{.file = 1}).others == Others{ChangeKind::FadeOut, 2.0});
    CHECK(makeStep(2, s, std::nullopt).others == Others{ChangeKind::Keep, 0.0});
}
// Every field type (optionals, timecode, changes, pads) must come back equal after toJson/fromJson.
TEST_CASE("A show survives a JSON round trip unchanged") {
    Show show; show.files = {{1, "audio/a.wav", -3.5}};
    Step st = makeStep(7, show.settings, Sound{.file = 1, .levelDb = -6, .fadeInSeconds = 5,
        .endSeconds = 90.0, .loop = true, .startDelaySeconds = 4, .timecodeStart = Timecode{1,0,0,0}, .ontimeCue = "OPEN"});
    st.changes = {{3, ChangeKind::Level, -15.0, 3.0}}; show.steps = {st}; show.lastStepId = 7;  // loader raises it to the max step id
    show.pads[2] = {"Fanfare", 1, -2.0};
    CHECK(fromJson(toJson(show)) == show);
}
// The version marker is what lets future readers recognise this format.
TEST_CASE("JSON text contains formatVersion 1") {
    CHECK(nlohmann::json::parse(toJson(Show{}))["formatVersion"] == 1);
}
TEST_CASE("Bad show files give ShowFormatError, never a crash") {   // corrupt input must throw our error, not crash or leak nlohmann types
    CHECK_THROWS_AS(fromJson("not json"), ShowFormatError);
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":2})"), ShowFormatError);          // newer than we know
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"steps":"oops"})"), ShowFormatError);
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"steps":[{"id":0}]})"), ShowFormatError);   // id 0
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"files":[{"id":1,"path":"a","trimDb":null}]})"), ShowFormatError);
}
// Forward compatibility: an old reader tolerates sparse files and extra keys.
TEST_CASE("Missing optional fields take defaults and unknown fields are ignored") {
    Show s = fromJson(R"({"formatVersion":1,"future":true,"steps":[{"id":1,"name":"x"}]})");
    CHECK(s.steps[0].others == Others{}); CHECK(s.settings.timecodeFps == 25);
}
// Further rejection rules: missing version, duplicate ids, Others "level", overflow to infinity, bad timecode.
TEST_CASE("More bad show files") {
    CHECK_THROWS_AS(fromJson(R"({"steps":[]})"), ShowFormatError);  // no formatVersion
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"steps":[{"id":1},{"id":1}]})"), ShowFormatError);
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"files":[{"id":1,"path":"a"},{"id":1,"path":"b"}]})"), ShowFormatError);
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"steps":[{"id":1,"others":{"kind":"level"}}]})"), ShowFormatError);
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"settings":{"defaultFadeSeconds":1e999}})"), ShowFormatError);
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"steps":[{"id":1,"sound":{"file":1,"timecodeStart":"bad"}}]})"), ShowFormatError);
}

// Ids in the upper half would collide with the engine's pad keys.
TEST_CASE("Step ids and change targets above kMaxStepId are rejected") {
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"steps":[{"id":2147483648}]})"), ShowFormatError);
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"steps":[{"id":1,"changes":[{"target":2147483648,"kind":"fadeOut"}]}]})"),
                    ShowFormatError);
    CHECK(fromJson(R"({"formatVersion":1,"steps":[{"id":2147483647}]})").steps[0].id == kMaxStepId);
}
// Timecode code divides by timecodeFps, so only the real SMPTE rates may load.
TEST_CASE("timecodeFps must be 24, 25 or 30") {
    for (int bad : {0, -25, 29})
        CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"settings":{"timecodeFps":)" + std::to_string(bad) + "}}"), ShowFormatError);
    for (int ok : {24, 25, 30})
        CHECK(fromJson(R"({"formatVersion":1,"settings":{"timecodeFps":)" + std::to_string(ok) + "}}").settings.timecodeFps == ok);
}

TEST_CASE("sourceHash and lastStepId survive a round trip; old files load") {
    Show show; show.files = {{1, "audio/a.flac", 0.0, "ab12"}}; show.lastStepId = 9;
    CHECK(fromJson(toJson(show)) == show);
    CHECK(fromJson(R"({"formatVersion":1,"steps":[{"id":4}]})").lastStepId == 4);
}
TEST_CASE("More than 16 changes in a step is a format error") {
    std::string changes; for (int i = 0; i < 17; ++i) changes += std::string(i ? "," : "") + R"({"target":1,"kind":"keep"})";
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"steps":[{"id":2,"changes":[)" + changes + "]}]}"), ShowFormatError);
}

// Pad grid, import version, tighter limits. All optional, so files written before them still load.
TEST_CASE("Pad grid, 24 pads and importVersion survive a round trip") {
    Show show; show.settings.padRows = 6; show.settings.padCols = 4; show.pads.resize(kMaxPads);
    show.pads[23] = {"Last", 1, -1.0};
    show.files = {{1, "audio/a.flac", 0.0, "ab", 2}};
    const Show back = fromJson(toJson(show));
    CHECK(back == show); CHECK(back.pads.size() == 24); CHECK(back.files[0].importVersion == 2);
}
TEST_CASE("25 pads are rejected") {
    std::string pads; for (int i = 0; i < 25; ++i) pads += std::string(i ? "," : "") + R"({"name":"p"})";
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"pads":[)" + pads + "]}"), ShowFormatError);
}
TEST_CASE("A file without importVersion loads as 1") {
    CHECK(fromJson(R"({"formatVersion":1,"files":[{"id":1,"path":"a"}]})").files[0].importVersion == 1);
}
TEST_CASE("lastStepId above kMaxStepId is rejected") {
    CHECK_THROWS_AS(fromJson(R"({"formatVersion":1,"lastStepId":2147483648})"), ShowFormatError);
    CHECK(fromJson(R"({"formatVersion":1,"lastStepId":2147483647})").lastStepId == kMaxStepId);
}
TEST_CASE("exactly 16 changes load, 17 are rejected") {
    auto with = [](int n) {
        std::string c; for (int i = 0; i < n; ++i) c += std::string(i ? "," : "") + R"({"target":1,"kind":"keep"})";
        return R"({"formatVersion":1,"steps":[{"id":2,"changes":[)" + c + "]}]}";
    };
    CHECK(fromJson(with(kMaxChanges)).steps[0].changes.size() == static_cast<size_t>(kMaxChanges));
    CHECK_THROWS_AS(fromJson(with(kMaxChanges + 1)), ShowFormatError);
}
TEST_CASE("padRows 7 or padCols 0 is rejected; so is a grid above 24 cells") {
    for (const char* bad : {R"("padRows":7)", R"("padCols":0)", R"("padRows":6,"padCols":5)"})
        CHECK_THROWS_AS(fromJson(std::string(R"({"formatVersion":1,"settings":{)") + bad + "}}"), ShowFormatError);
}
TEST_CASE("A show.json without pads or import versions loads with the new defaults") {
    const Show s = fromJson(R"({"formatVersion":1,"pads":[{"name":"a"},{},{},{},{},{},{},{}]})");
    CHECK(s.pads.size() == 8); CHECK(s.settings.padRows == 4); CHECK(s.settings.padCols == 2);
}

TEST_CASE("sourcePath survives a round trip; a file without it loads empty and writes no key") {
    Show show; show.files = {{1, "audio/a.flac", 0.0, "ab", 2, "sources/ab.mp3"}, {2, "audio/b.flac"}};
    const Show back = fromJson(toJson(show));
    CHECK(back == show); CHECK(back.files[0].sourcePath == "sources/ab.mp3"); CHECK(back.files[1].sourcePath.empty());
    CHECK(toJson(show).find("sourcePath") == toJson(show).rfind("sourcePath"));  // written once, for file 1 only
}
