// OntimeClient.cpp: see OntimeClient.h.
#include "bigtop/io/OntimeClient.h"

namespace bigtop::io {

namespace {
class JuceHttpGet : public HttpGet {
public:
    int get(const juce::String& url, int timeoutMs) override {
        int status = 0;
        auto stream = juce::URL(url).createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(timeoutMs)
                .withStatusCode(&status));
        if (stream == nullptr) return 0;
        stream->readEntireStreamAsString();  // the answer is small; reading it finishes the request
        return status;
    }
};
}  // namespace

std::unique_ptr<HttpGet> makeHttpGet() { return std::make_unique<JuceHttpGet>(); }

OntimeClient::OntimeClient(std::unique_ptr<HttpGet> http) : http_(std::move(http)), worker_([this] { run(); }) {}

OntimeClient::~OntimeClient() {
    {
        std::lock_guard<std::mutex> lock(m_);
        quit_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    worker_.join();
}

void OntimeClient::configure(bool enabled, const juce::String& host, int port) {
    std::lock_guard<std::mutex> lock(m_);
    const bool on = enabled && host.isNotEmpty();
    const juce::String base = on ? "http://" + host + ":" + juce::String(port) : juce::String();
    if (on == enabled_ && base == base_) return;
    enabled_ = on;
    base_ = base;
    ++generation_;
    queue_.clear();
    state_ = on ? State::Checking : State::Off;
    error_.clear();
    startFailed_ = startReported_ = false;
    healthSent_ = false;
}

void OntimeClient::queue(const juce::String& path, bool start) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!enabled_) return;
        if (queue_.size() >= kMaxQueued) {  // a hung host must not grow the queue forever
            if (queue_.front().start) startFailed_ = true;
            queue_.pop_front();
        }
        queue_.push_back({base_ + path, start, juce::Time::getMillisecondCounter()});
    }
    cv_.notify_one();
}

void OntimeClient::startCue(const juce::String& cue) {
    if (cue.isNotEmpty()) queue("/api/start/cue/" + juce::URL::addEscapeChars(cue, false), true);
}
void OntimeClient::pause() { queue("/api/pause", false); }
void OntimeClient::resume() { queue("/api/start", false); }
void OntimeClient::stop() { queue("/api/stop", false); }

void OntimeClient::tick(juce::uint32 nowMs) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!enabled_ || (healthSent_ && nowMs - lastHealthMs_ < kHealthMs)) return;
        healthSent_ = true;
        lastHealthMs_ = nowMs;
    }
    queue("/api/version", false);
}

OntimeClient::State OntimeClient::state() const {
    std::lock_guard<std::mutex> lock(m_);
    return state_;
}

juce::String OntimeClient::lastError() const {
    std::lock_guard<std::mutex> lock(m_);
    return error_;
}

bool OntimeClient::takeStartFailure() {
    std::lock_guard<std::mutex> lock(m_);
    if (!startFailed_ || startReported_) return false;
    startReported_ = true;
    return true;
}

bool OntimeClient::busy() const {
    std::lock_guard<std::mutex> lock(m_);
    return inFlight_ || !queue_.empty();
}

void OntimeClient::run() {
    std::unique_lock<std::mutex> lock(m_);
    for (;;) {
        cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
        if (quit_) return;
        const Request r = queue_.front();
        queue_.pop_front();
        // A cue that would start seconds after its GO is worse than none: it counts as failed.
        if (r.start && juce::Time::getMillisecondCounter() - r.queuedMs > static_cast<juce::uint32>(kTimeoutMs)) {
            startFailed_ = true;
            continue;
        }
        const uint64_t generation = generation_;
        const juce::String host = base_.fromFirstOccurrenceOf("http://", false, false);
        inFlight_ = true;
        lock.unlock();
        const int status = http_->get(r.url, kTimeoutMs);
        lock.lock();
        inFlight_ = false;
        if (generation != generation_) continue;  // configured elsewhere meanwhile: this answer is stale
        if (status >= 200 && status < 300) {
            state_ = State::Connected;
            error_.clear();
            // Only a start that worked ends a failure streak; a pause or health check says nothing about cues.
            if (r.start) startFailed_ = startReported_ = false;
        } else {
            state_ = State::Error;
            error_ = status == 0 ? "no answer from " + host : "HTTP " + juce::String(status) + " from " + host;
            if (r.start) startFailed_ = true;
        }
    }
}

}  // namespace bigtop::io
