#pragma once
/// @file OntimeClient.h
/// @brief Sends Bigtop's GOs to an Ontime server over its HTTP API: start a cue,
///        and optionally pause, resume and stop. One background thread sends
///        the requests in order, so GO never waits for the network.

#include <juce_core/juce_core.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace bigtop::io {

/// @brief One HTTP GET; the seam that keeps tests off the network.
class HttpGet {
public:
    virtual ~HttpGet() = default;
    /// @brief Fetches @p url and throws the body away.
    /// @param url Full URL.
    /// @param timeoutMs Connection timeout in ms.
    /// @return The HTTP status code, or 0 when there was no answer.
    /// @note Called on the client's worker thread only.
    virtual int get(const juce::String& url, int timeoutMs) = 0;
};

/// @brief The real HTTP GET (juce::URL).
/// @return A new instance.
std::unique_ptr<HttpGet> makeHttpGet();

/// @brief Bigtop -> Ontime: queued GET requests to /api/... of one host.
/// @note Every public member is called on one control thread (the message
///       thread); the worker only calls HttpGet. A failed start is never
///       retried: a late start is worse than none.
class OntimeClient {
public:
    /// @brief What the status bar shows.
    enum class State {
        Off,        ///< @brief Disabled: nothing is sent, no light.
        Checking,   ///< @brief Enabled, no answer yet.
        Connected,  ///< @brief The last request got a 2xx answer.
        Error,      ///< @brief The last request failed; see lastError().
    };

    static constexpr int kTimeoutMs = 1000;            ///< @brief Per request.
    static constexpr size_t kMaxQueued = 16;           ///< @brief Waiting requests kept; the oldest go first.
    static constexpr juce::uint32 kHealthMs = 5000;    ///< @brief How often /api/version is asked.

    /// @brief Starts the worker thread; the client is off until configure().
    /// @param http How requests go out (makeHttpGet() in the app).
    explicit OntimeClient(std::unique_ptr<HttpGet> http);
    /// @brief Stops the worker; a request in flight ends within its timeout.
    ~OntimeClient();
    OntimeClient(const OntimeClient&) = delete;
    OntimeClient& operator=(const OntimeClient&) = delete;

    /// @brief Sets where requests go; a change drops what was queued for the old host.
    /// @param enabled False: Off, nothing sent.
    /// @param host IPv4 address or host name; empty counts as disabled.
    /// @param port TCP port.
    void configure(bool enabled, const juce::String& host, int port);

    /// @brief Queues /api/start/cue/CUE, the cue URL-encoded; an empty cue queues nothing.
    /// @param cue The event's text in Ontime's Cue column.
    void startCue(const juce::String& cue);
    void pause();   ///< @brief Queues /api/pause.
    void resume();  ///< @brief Queues /api/start (the loaded event goes on).
    void stop();    ///< @brief Queues /api/stop.

    /// @brief Queues the health check (/api/version) every kHealthMs.
    /// @param nowMs A millisecond clock.
    void tick(juce::uint32 nowMs);

    /// @brief The light's state. @return See State.
    State state() const;
    /// @brief Why the last request failed. @return E.g. "no answer from host:port"; empty when fine.
    juce::String lastError() const;
    /// @brief Whether a cue start failed since the last success, reported once per failure streak.
    /// @return True the first time after a failed start; false until a success and a new failure.
    bool takeStartFailure();
    /// @brief Whether requests are queued or in flight. @return True while the worker has work.
    bool busy() const;

private:
    struct Request {
        juce::String url;
        bool start = false;
        juce::uint32 queuedMs = 0;  // a start older than kTimeoutMs is dropped: late is worse than never
    };
    void queue(const juce::String& path, bool start);
    void run();

    std::unique_ptr<HttpGet> http_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    bool quit_ = false, inFlight_ = false, enabled_ = false;
    juce::String base_;            // "http://host:port"
    uint64_t generation_ = 0;      // bumped by configure(): answers for an old host are ignored
    State state_ = State::Off;
    juce::String error_;
    bool startFailed_ = false, startReported_ = false;
    juce::uint32 lastHealthMs_ = 0;
    bool healthSent_ = false;
    std::thread worker_;  // last: started once everything above exists
};

}  // namespace bigtop::io
