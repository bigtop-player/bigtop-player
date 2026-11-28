// OntimeClientTests.cpp: the Ontime client against a fake HTTP stand-in (no
// socket is opened): URLs, order, the queue cap, timeouts, on/off and shutdown.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "bigtop/io/OntimeClient.h"

using namespace bigtop::io;

namespace {
// Shared with the test: what was asked for, and how to answer.
struct Recorder {
    std::mutex m;
    std::vector<juce::String> urls;
    std::atomic<int> status{200};
    std::atomic<bool> hold{false};  // a slow host: requests wait until released
};

struct FakeHttp : HttpGet {
    explicit FakeHttp(std::shared_ptr<Recorder> r) : rec(std::move(r)) {}
    int get(const juce::String& url, int) override {
        while (rec->hold) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard<std::mutex> lock(rec->m);
        rec->urls.push_back(url);
        return rec->status;
    }
    std::shared_ptr<Recorder> rec;
};

std::vector<juce::String> sent(Recorder& r) {
    std::lock_guard<std::mutex> lock(r.m);
    return r.urls;
}

// Waits (real time, at most 2 s) until the client has nothing left to send.
void settle(OntimeClient& c) {
    for (int i = 0; i < 1000 && c.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE_FALSE(c.busy());
}
}  // namespace

TEST_CASE("A cue start is sent to the host, URL-encoded") {
    auto rec = std::make_shared<Recorder>();
    OntimeClient c(std::make_unique<FakeHttp>(rec));
    c.configure(true, "192.0.2.10", 4001);
    c.startCue("A 1/2");
    settle(c);
    REQUIRE(sent(*rec) == std::vector<juce::String>{"http://192.0.2.10:4001/api/start/cue/A%201%2F2"});
    CHECK(c.state() == OntimeClient::State::Connected);
}

TEST_CASE("Requests go out in order: pause, start, stop") {
    auto rec = std::make_shared<Recorder>();
    OntimeClient c(std::make_unique<FakeHttp>(rec));
    c.configure(true, "192.0.2.10", 4001);
    c.pause();
    c.resume();
    c.stop();
    settle(c);
    CHECK(sent(*rec) == std::vector<juce::String>{"http://192.0.2.10:4001/api/pause", "http://192.0.2.10:4001/api/start",
                                                  "http://192.0.2.10:4001/api/stop"});
}

TEST_CASE("Disabled sends nothing and is Off") {
    auto rec = std::make_shared<Recorder>();
    OntimeClient c(std::make_unique<FakeHttp>(rec));
    c.configure(false, "192.0.2.10", 4001);
    c.startCue("OPEN");
    c.tick(100000);
    settle(c);
    CHECK(sent(*rec).empty());
    CHECK(c.state() == OntimeClient::State::Off);
}

TEST_CASE("No answer is an error, the next success clears it, and a failed start is reported once per streak") {
    auto rec = std::make_shared<Recorder>();
    OntimeClient c(std::make_unique<FakeHttp>(rec));
    c.configure(true, "192.0.2.10", 4001);
    rec->status = 0;
    c.startCue("OPEN");
    c.startCue("OPEN");
    settle(c);
    CHECK(c.state() == OntimeClient::State::Error);
    CHECK(c.lastError() == "no answer from 192.0.2.10:4001");
    CHECK(c.takeStartFailure());
    CHECK_FALSE(c.takeStartFailure());  // the second failure belongs to the same streak
    rec->status = 200;
    c.startCue("OPEN");
    settle(c);
    CHECK(c.state() == OntimeClient::State::Connected);
    CHECK(sent(*rec).size() == 3);  // a failed start is never retried
}

TEST_CASE("The queue keeps the newest 16 while the host hangs") {
    auto rec = std::make_shared<Recorder>();
    OntimeClient c(std::make_unique<FakeHttp>(rec));
    c.configure(true, "192.0.2.10", 4001);
    rec->hold = true;
    c.startCue("first");  // taken by the worker, waits on the host
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (int i = 0; i < 20; ++i) c.startCue(juce::String(i));
    rec->hold = false;
    settle(c);
    const auto urls = sent(*rec);
    REQUIRE(urls.size() == 1 + OntimeClient::kMaxQueued);
    CHECK(urls[1].endsWith("/cue/4"));
    CHECK(urls.back().endsWith("/cue/19"));
}

TEST_CASE("Changing the host drops what was queued for the old one") {
    auto rec = std::make_shared<Recorder>();
    OntimeClient c(std::make_unique<FakeHttp>(rec));
    c.configure(true, "192.0.2.10", 4001);
    rec->hold = true;
    c.startCue("first");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.startCue("queued");
    c.configure(true, "192.0.2.20", 4001);
    rec->hold = false;
    settle(c);
    for (const auto& u : sent(*rec)) CHECK_FALSE(u.endsWith("/cue/queued"));
}

TEST_CASE("The health check asks for the version every 5 s") {
    auto rec = std::make_shared<Recorder>();
    OntimeClient c(std::make_unique<FakeHttp>(rec));
    c.configure(true, "192.0.2.10", 4001);
    c.tick(1000);
    settle(c);
    c.tick(2000);  // too soon
    c.tick(1000 + OntimeClient::kHealthMs);
    settle(c);
    CHECK(sent(*rec) == std::vector<juce::String>{"http://192.0.2.10:4001/api/version", "http://192.0.2.10:4001/api/version"});
}

TEST_CASE("Shutting down with a hanging request returns") {
    auto rec = std::make_shared<Recorder>();
    const auto start = std::chrono::steady_clock::now();
    {
        OntimeClient c(std::make_unique<FakeHttp>(rec));
        c.configure(true, "192.0.2.10", 4001);
        rec->hold = true;
        c.startCue("x");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::thread release([rec] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));  // as a 1 s timeout would, only sooner
            rec->hold = false;
        });
        release.detach();
    }
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

TEST_CASE("A start that waited longer than the timeout is dropped, not sent late") {
    auto rec = std::make_shared<Recorder>();
    OntimeClient c(std::make_unique<FakeHttp>(rec));
    c.configure(true, "192.0.2.10", 4001);
    rec->hold = true;
    c.startCue("first");  // in flight, the host hangs
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.startCue("late1");
    c.startCue("late2");
    c.pause();  // not a start: still sent
    std::this_thread::sleep_for(std::chrono::milliseconds(OntimeClient::kTimeoutMs + 200));
    rec->hold = false;
    settle(c);
    CHECK(sent(*rec) == std::vector<juce::String>{"http://192.0.2.10:4001/api/start/cue/first", "http://192.0.2.10:4001/api/pause"});
    CHECK(c.takeStartFailure());
}
