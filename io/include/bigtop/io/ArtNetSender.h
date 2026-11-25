/// @file ArtNetSender.h
/// @brief Sends Art-Net timecode (ArtTimeCode) so the lighting console follows
///        the music, on its own thread, optionally out of one chosen network
///        adapter.
///
/// The sender only reads engine snapshots; it never touches the audio thread
/// and never shows a dialog. A network failure becomes a status, nothing more.
/// Timing comes from the snapshot's playback position, not from counting
/// wall-clock frames, so it cannot drift away from the audio.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <juce_core/juce_core.h>

#include "bigtop/Engine.h"
#include "bigtop/Timecode.h"

namespace bigtop::io {

/// @brief One IPv4 network interface address of this machine.
struct NetAdapter {
    juce::IPAddress address;    ///< @brief The interface's own address.
    juce::IPAddress broadcast;  ///< @brief Its subnet broadcast address (from the real prefix length).
    juce::String name;          ///< @brief The OS's friendly name, e.g. "Ethernet 2".
    /// @brief Text for the settings list.
    /// @return "name — address (broadcast broadcast-address)",
    ///         e.g. "Ethernet 2 — 2.0.0.10 (broadcast 2.255.255.255)".
    juce::String label() const;
};

/// @brief One unicast IPv4 address as the OS reports it, before filtering.
struct RawAdapter {
    juce::String name;      ///< @brief Friendly name.
    uint32_t address = 0;   ///< @brief IPv4 address, host byte order (a.b.c.d = a<<24 | ... | d).
    int prefixLength = 0;   ///< @brief Subnet prefix length, 0..32.
};

/// @brief The subnet broadcast address of an interface.
/// @param address IPv4 address, host byte order.
/// @param prefixLength Subnet prefix length; clamped to 0..32.
/// @return @p address with all host bits set (/32 gives the address itself, /0 gives 255.255.255.255).
uint32_t ipv4Broadcast(uint32_t address, int prefixLength);

/// @brief Whether an address can be the local end of an Art-Net link.
/// @param address IPv4 address, host byte order.
/// @return False for 0.0.0.0, loopback 127/8, multicast 224/4 and limited broadcast.
///         Link-local 169.254/16 is usable here; selectAdapters() ranks it last.
bool isUsableAdapterAddress(uint32_t address);

/// @brief Turns the OS's raw address list into the adapters to offer.
/// @param raw Every unicast IPv4 address of the interfaces that are up.
/// @return Usable addresses with their real broadcast; point-to-point (/32)
///         adapters are left out because they have no broadcast. Link-local 169.254/16
///         (an unconfigured adapter) is left out unless nothing else is usable,
///         so a lone direct cable to a console still shows up.
std::vector<NetAdapter> selectAdapters(const std::vector<RawAdapter>& raw);

/// @brief The machine's usable IPv4 adapters (see selectAdapters()).
/// @return One entry per address; empty if the system reports none.
/// @note Any thread; queries the OS, so not for the audio thread.
std::vector<NetAdapter> listAdapters();

/// @brief What the sender is doing, for the status bar light.
struct ArtNetStatus {
    /// @brief The sender's condition.
    enum class State {
        Idle,     ///< @brief No driving sound; nothing is sent.
        Sending,  ///< @brief Packets go out.
        Error     ///< @brief The last bind or send failed.
    };
    State state = State::Idle;  ///< @brief Current condition.
    juce::String text;  ///< @brief Human text ("Art-Net: sending", the error reason, ...).
};

/// @brief A UDP socket seen from the sender; a fake in tests, a real one in the app.
class UdpOut {
public:
    virtual ~UdpOut() = default;
    /// @brief Binds the outgoing socket to a local interface.
    /// @param localAddress Local IPv4 address, or "" for any/system choice.
    /// @return False if binding failed.
    /// @note Called from the sender thread only; may be called again to rebind.
    virtual bool bind(const juce::String& localAddress) = 0;
    /// @brief Sends one datagram to UDP port 6454 of @p host.
    /// @param host Destination IPv4 address.
    /// @param data Bytes to send.
    /// @param size Byte count.
    /// @return False if the datagram was not sent.
    virtual bool send(const juce::String& host, const uint8_t* data, int size) = 0;
};

/// @brief A real UDP socket with broadcast enabled (juce::DatagramSocket).
/// @return A socket that has not been bound yet; bind() before send().
std::unique_ptr<UdpOut> makeDatagramOut();

/// @brief The Art-Net timecode sender.
///
/// Per step it reads a snapshot; while a sound drives timecode it sends one
/// packet each time the frame changes, and while paused it resends the frozen
/// frame once per frame period. With no driver it sends nothing (Idle).
class ArtNetSender {
public:
    /// @brief Microseconds from any monotonic clock; only differences matter.
    using Micros = int64_t;

    /// @brief Creates the sender and (normally) starts its thread.
    /// @param snapshot Returns the engine snapshot; called from the sender
    ///        thread, so it must be safe from any thread (Engine::snapshot()).
    /// @param out The socket; owned by the sender and used only by its thread.
    /// @param startThread False in tests: nobody calls step() but the test, so
    ///        runs are deterministic. True: a thread calls step() every ~1 ms.
    /// @param adapters Lists the adapters; replaceable so tests need no network.
    ArtNetSender(std::function<Snapshot()> snapshot, std::unique_ptr<UdpOut> out, bool startThread = true,
                 std::function<std::vector<NetAdapter>()> adapters = listAdapters);
    /// @brief Stops and joins the thread.
    ~ArtNetSender();
    ArtNetSender(const ArtNetSender&) = delete;
    ArtNetSender& operator=(const ArtNetSender&) = delete;

    /// @brief Sets where and how packets go; applies at the next step.
    /// @param target Destination address used with "Default" (e.g. 255.255.255.255).
    /// @param adapterAddress Address of the adapter to bind and send from (to its
    ///        broadcast), or "" for Default. A stored address that is not among
    ///        the adapters falls back to Default and says so in status(); it is
    ///        looked up again about once a second and used as soon as it appears.
    /// @param fps Timecode frame rate (24, 25 or 30); others make nothing send.
    /// @note Any thread (the app's message thread); never from the audio thread.
    void configure(juce::String target, juce::String adapterAddress, int fps);

    /// @brief The current status.
    /// @return A copy. Any thread.
    ArtNetStatus status() const;

    /// @brief One pass of the sender loop: read the snapshot, send if due.
    /// @param now Current time in microseconds (monotonic).
    /// @note The thread calls this with the real clock; tests call it with
    ///       simulated time. Not thread-safe against itself: call it only from
    ///       the thread, or from the test when no thread was started.
    void step(Micros now);

private:
    void setStatus(ArtNetStatus::State state, const juce::String& text);

    std::function<Snapshot()> snapshot_;
    std::unique_ptr<UdpOut> out_;
    std::function<std::vector<NetAdapter>()> adapters_;

    // Config, written by configure() and read by step(); the mutex is never
    // taken by the audio thread. generation_ lets step() notice a change.
    mutable std::mutex mutex_;
    juce::String target_ = "255.255.255.255";
    juce::String adapter_;
    int fps_ = 25;
    uint32_t generation_ = 1;
    ArtNetStatus status_{ArtNetStatus::State::Idle, "Art-Net: idle"};

    // Thread-only state.
    uint32_t seenGeneration_ = 0;
    bool bound_ = false;
    juce::String boundTo_;             // local address to bind, "" = system choice
    juce::String destination_;       // where packets go for the applied config
    juce::String adapterNote_;         // "adapter missing" text, empty if fine
    std::optional<Timecode> lastSent_; // last frame sent; reset to force a send
    Micros lastAttempt_ = 0;           // when a packet was last tried
    Micros lastLookup_ = 0;            // when the adapter list was last read
    bool failed_ = false;              // the last attempt failed

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace bigtop::io
