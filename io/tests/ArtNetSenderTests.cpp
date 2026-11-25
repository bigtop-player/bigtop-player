// ArtNetSenderTests.cpp: the sender against a fake socket and a fake snapshot,
// driven step by step with simulated time (no thread, no network).
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <optional>

#include "bigtop/io/ArtNetSender.h"

using namespace bigtop;
using namespace bigtop::io;

namespace {
// Test-side decoder, independent of the encoder (mirrors core/tests/TimecodeTests.cpp).
std::optional<Timecode> decode(const std::vector<uint8_t>& p, int* fps = nullptr) {
    if (p.size() != 19 || std::memcmp(p.data(), "Art-Net\0", 8) != 0) return std::nullopt;
    if (p[8] != 0x00 || p[9] != 0x97 || p[10] != 0 || p[11] != 14) return std::nullopt;
    if (fps != nullptr) *fps = p[18] == 0 ? 24 : p[18] == 1 ? 25 : 30;
    return Timecode{p[17], p[16], p[15], p[14]};
}

struct Packet {
    juce::String host;
    std::vector<uint8_t> bytes;
};

// Records what the sender does; shared with the test through a pointer.
struct FakeOut : UdpOut {
    std::vector<Packet> sent;
    std::vector<juce::String> binds;
    bool failSend = false;
    bool failBind = false;
    int sendAttempts = 0;
    bool bind(const juce::String& a) override {
        binds.push_back(a);
        return !failBind;
    }
    bool send(const juce::String& host, const uint8_t* d, int n) override {
        ++sendAttempts;
        if (failSend) return false;
        sent.push_back({host, std::vector<uint8_t>(d, d + n)});
        return true;
    }
};

// The "engine": a position in seconds the test moves, or no driver at all.
struct Fixture {
    FakeOut* out = nullptr;  // owned by the sender
    double seconds = 0.0;
    bool driving = true;
    bool paused = false;
    std::vector<NetAdapter> adapters;
    ArtNetSender sender;

    Fixture() : sender(makeSnapshot(), makeOut(), false, [this] { return adapters; }) {}

    std::function<Snapshot()> makeSnapshot() {
        return [this] {
            Snapshot s;
            if (driving) {
                s.timecodeKey = 1;
                s.timecodeFrame = static_cast<int64_t>(seconds * 48000.0);
            }
            s.paused = paused;
            return s;
        };
    }
    std::unique_ptr<UdpOut> makeOut() {
        auto o = std::make_unique<FakeOut>();
        out = o.get();
        return o;
    }
    // Advance simulated time by 1 ms steps, position following at real speed.
    void run(double secs, bool moving = true) {
        for (int i = 0; i < static_cast<int>(secs * 1000); ++i) {
            now += 1000;
            if (moving) seconds += 0.001;
            sender.step(now);
        }
    }
    ArtNetSender::Micros now = 1'000'000;
};
}  // namespace

TEST_CASE("Sends one packet per frame while a sound drives") {
    Fixture f;
    f.sender.configure("255.255.255.255", "", 25);
    f.run(1.0);
    CHECK(std::abs(static_cast<int>(f.out->sent.size()) - 25) <= 1);
    int fps = 0;
    for (size_t i = 1; i < f.out->sent.size(); ++i) {
        const auto a = decode(f.out->sent[i - 1].bytes), b = decode(f.out->sent[i].bytes, &fps);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(b->f == (a->f + 1) % 25);  // consecutive frames
    }
    CHECK(fps == 25);
    CHECK(f.out->sent[0].host == "255.255.255.255");
    CHECK(f.sender.status().state == ArtNetStatus::State::Sending);
}

TEST_CASE("Sends nothing without a driver") {
    Fixture f;
    f.driving = false;
    f.sender.configure("255.255.255.255", "", 25);
    f.run(0.5);
    CHECK(f.out->sent.empty());
    CHECK(f.sender.status().state == ArtNetStatus::State::Idle);
}

TEST_CASE("Follows a seek at once") {
    Fixture f;
    f.sender.configure("255.255.255.255", "", 25);
    f.run(0.2);
    f.seconds = 10.0;  // seek
    f.run(0.002);
    const auto tc = decode(f.out->sent.back().bytes);
    REQUIRE(tc);
    CHECK(*tc == Timecode{0, 0, 10, 0});
}

TEST_CASE("Resends the frozen frame while paused") {
    Fixture f;
    f.sender.configure("255.255.255.255", "", 25);
    f.run(0.5);
    f.paused = true;
    f.run(0.01);  // let the last moving frame settle
    const auto before = f.out->sent.size();
    f.run(1.0, false);  // position frozen
    CHECK(std::abs(static_cast<int>(f.out->sent.size() - before) - 25) <= 1);
    CHECK(decode(f.out->sent.back().bytes) == decode(f.out->sent[before].bytes));
}

TEST_CASE("An adapter binds and sends to its broadcast") {
    Fixture f;
    f.adapters = {{juce::IPAddress("192.168.1.20"), juce::IPAddress("192.168.1.255"), "Wi-Fi"}};
    f.sender.configure("10.0.0.255", "192.168.1.20", 25);
    f.run(0.1);
    REQUIRE_FALSE(f.out->sent.empty());
    CHECK(f.out->binds.front() == "192.168.1.20");
    CHECK(f.out->sent.front().host == "192.168.1.255");
}

TEST_CASE("A missing adapter falls back to Default and says so") {
    Fixture f;
    f.sender.configure("10.0.0.255", "192.168.9.9", 25);
    f.run(0.1);
    REQUIRE_FALSE(f.out->sent.empty());
    CHECK(f.out->binds.front() == "");
    CHECK(f.out->sent.front().host == "10.0.0.255");
    CHECK(f.sender.status().text == "Art-Net: adapter 192.168.9.9 missing, using default");
}

TEST_CASE("A failing send reports Error and recovers") {
    Fixture f;
    f.sender.configure("255.255.255.255", "", 25);
    f.out->failSend = true;
    f.run(0.2);
    CHECK(f.sender.status().state == ArtNetStatus::State::Error);
    CHECK(f.out->sent.empty());
    f.out->failSend = false;
    f.run(0.1);
    CHECK_FALSE(f.out->sent.empty());
    CHECK(f.sender.status().state == ArtNetStatus::State::Sending);
}

TEST_CASE("fps change applies at once") {
    Fixture f;
    f.sender.configure("255.255.255.255", "", 25);
    f.run(0.2);
    f.sender.configure("255.255.255.255", "", 30);
    f.run(0.002);
    int fps = 0;
    REQUIRE(decode(f.out->sent.back().bytes, &fps));
    CHECK(fps == 30);
}

TEST_CASE("NetAdapter label shows name, address and broadcast") {
    const NetAdapter a{juce::IPAddress("2.0.0.10"), juce::IPAddress("2.255.255.255"), "Ethernet 2"};
    CHECK(a.label() == juce::String("Ethernet 2 ") + juce::String::charToString(0x2014) + " 2.0.0.10 (broadcast 2.255.255.255)");
}

namespace {
constexpr uint32_t ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return (a << 24) | (b << 16) | (c << 8) | d; }
}  // namespace

TEST_CASE("ipv4Broadcast sets the host bits of the real prefix") {
    CHECK(ipv4Broadcast(ip(2, 0, 0, 10), 8) == ip(2, 255, 255, 255));
    CHECK(ipv4Broadcast(ip(192, 168, 1, 20), 24) == ip(192, 168, 1, 255));
    CHECK(ipv4Broadcast(ip(10, 1, 2, 3), 16) == ip(10, 1, 255, 255));
    CHECK(ipv4Broadcast(ip(10, 1, 2, 3), 32) == ip(10, 1, 2, 3));        // host route: no host bits
    CHECK(ipv4Broadcast(ip(10, 1, 2, 3), 0) == ip(255, 255, 255, 255));  // everything is host bits
}

TEST_CASE("isUsableAdapterAddress rejects what no console can sit behind") {
    CHECK(isUsableAdapterAddress(ip(192, 168, 1, 20)));
    CHECK(isUsableAdapterAddress(ip(2, 0, 0, 10)));
    CHECK_FALSE(isUsableAdapterAddress(ip(0, 0, 0, 0)));
    CHECK_FALSE(isUsableAdapterAddress(ip(127, 0, 0, 1)));
    CHECK_FALSE(isUsableAdapterAddress(ip(224, 0, 0, 1)));
    CHECK_FALSE(isUsableAdapterAddress(ip(239, 255, 255, 250)));
    CHECK_FALSE(isUsableAdapterAddress(ip(255, 255, 255, 255)));
}

TEST_CASE("selectAdapters drops link-local only when something better exists") {
    const std::vector<RawAdapter> mixed{{"Wi-Fi", ip(192, 168, 1, 20), 24},
                                        {"Bluetooth", ip(169, 254, 3, 4), 16},
                                        {"Loopback", ip(127, 0, 0, 1), 8},
                                        {"Multicast", ip(224, 0, 0, 251), 4}};
    const auto good = selectAdapters(mixed);
    REQUIRE(good.size() == 1);
    CHECK(good[0].name == "Wi-Fi");
    CHECK(good[0].broadcast.toString() == "192.168.1.255");

    const auto onlyLinkLocal = selectAdapters({mixed[1], mixed[2]});
    REQUIRE(onlyLinkLocal.size() == 1);
    CHECK(onlyLinkLocal[0].address.toString() == "169.254.3.4");
    CHECK(onlyLinkLocal[0].broadcast.toString() == "169.254.255.255");

    CHECK(selectAdapters({mixed[2]}).empty());
}

TEST_CASE("An unsupported fps keeps the sender idle") {
    for (const int fps : {0, 23, 60, -1}) {
        Fixture f;
        f.sender.configure("255.255.255.255", "", fps);
        f.run(0.2);
        CHECK(f.out->sendAttempts == 0);
        CHECK(f.sender.status().state == ArtNetStatus::State::Idle);
    }
}

TEST_CASE("A failing send is retried once per frame period") {
    Fixture f;
    f.sender.configure("255.255.255.255", "", 25);
    f.out->failSend = true;
    f.run(1.0);
    CHECK(std::abs(f.out->sendAttempts - 25) <= 1);  // not 1000
}

TEST_CASE("A bind failure reports Error with a clear text") {
    Fixture f;
    f.out->failBind = true;
    f.sender.configure("255.255.255.255", "", 25);
    f.run(0.1);
    CHECK(f.sender.status().state == ArtNetStatus::State::Error);
    CHECK(f.sender.status().text == "Art-Net: cannot use default adapter");
    CHECK(f.out->sendAttempts == 0);
}

TEST_CASE("Target and adapter changes apply at the next frame") {
    Fixture f;
    f.adapters = {{juce::IPAddress("192.168.1.20"), juce::IPAddress("192.168.1.255"), "Wi-Fi"}};
    f.sender.configure("10.0.0.255", "", 25);
    f.run(0.1);
    CHECK(f.out->sent.back().host == "10.0.0.255");
    f.sender.configure("10.0.1.255", "", 25);
    f.run(0.002);
    CHECK(f.out->sent.back().host == "10.0.1.255");
    f.sender.configure("10.0.1.255", "192.168.1.20", 25);
    f.run(0.002);
    CHECK(f.out->sent.back().host == "192.168.1.255");
    CHECK(f.out->binds.back() == "192.168.1.20");
}

TEST_CASE("A returning driver sends its first frame at once") {
    Fixture f;
    f.sender.configure("255.255.255.255", "", 25);
    f.run(0.1);
    f.driving = false;
    f.run(0.1);
    const auto before = f.out->sent.size();
    f.driving = true;
    f.run(0.002, false);  // same position as before the loss
    CHECK(f.out->sent.size() == before + 1);
}

TEST_CASE("selectAdapters hides point-to-point (/32) adapters") {
    const auto r = selectAdapters({{"VPN", ip(198, 51, 100, 7), 32}, {"Wi-Fi", ip(192, 168, 1, 20), 24}});
    REQUIRE(r.size() == 1);
    CHECK(r[0].name == "Wi-Fi");
    CHECK(selectAdapters({{"VPN", ip(198, 51, 100, 7), 32}}).empty());  // nothing extra; Default remains
}

// The console cable plugged in after the app started: the chosen adapter was
// missing at configure, so sending fell back to Default. It must switch over
// once the adapter shows up, without a settings change.
TEST_CASE("A missing adapter is looked up again and used once it appears") {
    Fixture f;
    f.sender.configure("203.0.113.255", "192.0.2.20", 25);
    f.run(0.5);
    REQUIRE_FALSE(f.out->sent.empty());
    CHECK(f.out->sent.back().host == "203.0.113.255");
    f.adapters = {{juce::IPAddress("192.0.2.20"), juce::IPAddress("192.0.2.255"), "Console port"}};
    f.run(1.1);  // looked up again about once a second
    CHECK(f.out->binds.back() == "192.0.2.20");
    CHECK(f.out->sent.back().host == "192.0.2.255");
    CHECK(f.sender.status().text == "Art-Net: sending");
}

TEST_CASE("A missing adapter is not looked up on every step") {
    Fixture f;
    int lookups = 0;
    ArtNetSender sender(f.makeSnapshot(), std::make_unique<FakeOut>(), false, [&] {
        ++lookups;
        return std::vector<NetAdapter>{};
    });
    sender.configure("203.0.113.255", "192.0.2.20", 25);
    for (ArtNetSender::Micros t = 1'000'000; t <= 3'000'000; t += 1000) sender.step(t);
    CHECK(lookups >= 2);
    CHECK(lookups <= 4);
}

// A pulled cable makes sends fail; the socket may be dead for good, so the
// next attempt binds a fresh one.
TEST_CASE("A send failure rebinds at the next attempt") {
    Fixture f;
    f.adapters = {{juce::IPAddress("192.0.2.20"), juce::IPAddress("192.0.2.255"), "Console port"}};
    f.sender.configure("203.0.113.255", "192.0.2.20", 25);
    f.run(0.1);
    REQUIRE(f.out->binds.size() == 1);
    f.out->failSend = true;
    f.run(0.05);
    f.out->failSend = false;
    f.run(0.1);
    CHECK(f.out->binds.size() >= 2);
    CHECK(f.out->binds.back() == "192.0.2.20");
    CHECK(f.sender.status().state == ArtNetStatus::State::Sending);
}
