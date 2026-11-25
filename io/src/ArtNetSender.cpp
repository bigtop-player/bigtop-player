// ArtNetSender.cpp: the timecode sender loop, adapter listing and the real
// UDP socket.
#include "bigtop/io/ArtNetSender.h"

#include <algorithm>
#include <chrono>

#include "bigtop/ShowController.h"

// The one OS-specific part of this file is queryOsAddresses() below.
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX  // windows.h's min/max macros break std::min/max and JUCE
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <iphlpapi.h>
#else
 #include <arpa/inet.h>
 #include <ifaddrs.h>
 #include <net/if.h>
 #include <netinet/in.h>
#endif

namespace bigtop::io {

namespace {
constexpr double kSampleRate = 48000.0;  // the engine's rate; the snapshot position is in these frames
constexpr int kArtNetPort = 6454;

// A real socket. Broadcast must be enabled or sending to x.x.x.255 is refused.
class DatagramOut final : public UdpOut {
public:
    bool bind(const juce::String& localAddress) override {
        socket_ = std::make_unique<juce::DatagramSocket>(true);  // true = allow broadcast
        // Port 0: any free local port; only the interface matters here.
        return socket_->bindToPort(0, localAddress);
    }
    bool send(const juce::String& host, const uint8_t* data, int size) override {
        if (socket_ == nullptr) return false;
        return socket_->write(host, kArtNetPort, data, size) == size;
    }

private:
    std::unique_ptr<juce::DatagramSocket> socket_;
};
}  // namespace

juce::String NetAdapter::label() const {
    // U+2014 em dash between the name and the address.
    return name + " " + juce::String::charToString(0x2014) + " " + address.toString() + " (broadcast " + broadcast.toString() + ")";
}

uint32_t ipv4Broadcast(uint32_t address, int prefixLength) {
    const int p = std::clamp(prefixLength, 0, 32);
    // Shifting a 32-bit value by 32 is undefined, so /32 (no host bits) is handled apart.
    const uint32_t hostBits = p == 32 ? 0u : (0xFFFFFFFFu >> p);
    return address | hostBits;
}

namespace {
bool isLinkLocal(uint32_t a) { return (a >> 16) == 0xA9FE; }  // 169.254/16: no DHCP answered
}  // namespace

bool isUsableAdapterAddress(uint32_t a) {
    const uint32_t top = a >> 24;
    return a != 0 && top != 127 && (top < 224 || top > 239) && a != 0xFFFFFFFFu;
}

std::vector<NetAdapter> selectAdapters(const std::vector<RawAdapter>& raw) {
    std::vector<NetAdapter> normal, linkLocal;
    for (const auto& r : raw) {
        if (!isUsableAdapterAddress(r.address)) continue;
        // /32 = point-to-point (e.g. a VPN link): no broadcast exists, so sending would go nowhere.
        if (r.prefixLength >= 32) continue;
        NetAdapter n{juce::IPAddress(r.address), juce::IPAddress(ipv4Broadcast(r.address, r.prefixLength)), r.name};
        (isLinkLocal(r.address) ? linkLocal : normal).push_back(std::move(n));
    }
    // Link-local only means an adapter without a lease, which no console can
    // answer on; but with nothing else (a direct cable to a console) it is the
    // only way out, so then it is listed.
    return normal.empty() ? linkLocal : normal;
}

namespace {
// Every unicast IPv4 address of the interfaces that are up, with the OS's real
// prefix length (JUCE reports neither names nor a correct broadcast).
std::vector<RawAdapter> queryOsAddresses() {
    std::vector<RawAdapter> raw;
#if JUCE_WINDOWS
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    // The table can grow between calls; retry a few times with the size asked for.
    for (int attempt = 0; attempt < 4 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) return raw;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            if (u->Address.lpSockaddr == nullptr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            raw.push_back({juce::String(a->FriendlyName), ntohl(sin->sin_addr.s_addr), static_cast<int>(u->OnLinkPrefixLength)});
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return raw;
    for (auto* i = list; i != nullptr; i = i->ifa_next) {
        if (i->ifa_addr == nullptr || i->ifa_netmask == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
        if ((i->ifa_flags & IFF_UP) == 0) continue;
        const uint32_t mask = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
        int prefix = 0;
        for (uint32_t m = mask; (m & 0x80000000u) != 0; m <<= 1) ++prefix;  // a netmask is contiguous ones
        raw.push_back({i->ifa_name, ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr), prefix});
    }
    freeifaddrs(list);
#endif
    return raw;
}
}  // namespace

std::vector<NetAdapter> listAdapters() { return selectAdapters(queryOsAddresses()); }

std::unique_ptr<UdpOut> makeDatagramOut() { return std::make_unique<DatagramOut>(); }

ArtNetSender::ArtNetSender(std::function<Snapshot()> snapshot, std::unique_ptr<UdpOut> out, bool startThread,
                           std::function<std::vector<NetAdapter>()> adapters)
    : snapshot_(std::move(snapshot)), out_(std::move(out)), adapters_(std::move(adapters)) {
    if (!startThread) return;
    thread_ = std::thread([this] {
        while (!stop_.load()) {
            step(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                     .count());
            // ~1 ms: fine enough for a frame period of 33-42 ms, light on the CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

ArtNetSender::~ArtNetSender() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

void ArtNetSender::configure(juce::String target, juce::String adapterAddress, int fps) {
    const std::lock_guard lock(mutex_);
    target_ = std::move(target);
    adapter_ = std::move(adapterAddress);
    fps_ = fps;
    ++generation_;
}

ArtNetStatus ArtNetSender::status() const {
    const std::lock_guard lock(mutex_);
    return status_;
}

void ArtNetSender::setStatus(ArtNetStatus::State state, const juce::String& text) {
    const std::lock_guard lock(mutex_);
    status_ = {state, text};
}

void ArtNetSender::step(Micros now) {
    juce::String target, adapter;
    int fps = 25;
    uint32_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        target = target_;
        adapter = adapter_;
        fps = fps_;
        generation = generation_;
    }

    const bool newConfig = generation != seenGeneration_;
    // A chosen adapter that was missing (cable not plugged in yet) is looked
    // up again about once a second, so sending switches to it when it appears.
    const bool lookAgain = adapterNote_.isNotEmpty() && now - lastLookup_ >= 1'000'000;
    if (newConfig || lookAgain) {
        lastLookup_ = now;
        juce::String destination = target, note;
        if (adapter.isNotEmpty()) {
            bool found = false;
            for (const auto& a : adapters_()) {
                if (a.address.toString() == adapter) {
                    destination = a.broadcast.toString();
                    found = true;
                    break;
                }
            }
            // Keep sending through the default route rather than go silent.
            if (!found) {
                note = "Art-Net: adapter " + adapter + " missing, using default";
                adapter.clear();
            }
        }
        // A look-up that still misses the adapter changes nothing; anything
        // else rebinds and sends the current frame at once (so an fps, target
        // or adapter change shows up immediately).
        if (newConfig || note.isEmpty()) {
            seenGeneration_ = generation;
            bound_ = false;
            failed_ = false;
            lastSent_.reset();
            destination_ = destination;
            adapterNote_ = note;
            boundTo_ = adapter;
        }
    }

    const auto snap = snapshot_();
    const auto tc = snapshotTimecode(snap, kSampleRate, fps);
    if (!tc) {
        lastSent_.reset();  // the next driver sends its first frame at once
        failed_ = false;
        setStatus(ArtNetStatus::State::Idle, adapterNote_.isEmpty() ? juce::String("Art-Net: idle") : adapterNote_);
        return;
    }

    // fps is supported here (snapshotTimecode returned nullopt otherwise, above),
    // so it is 24, 25 or 30 and this division is safe.
    const auto period = static_cast<Micros>(1'000'000 / fps);
    const bool periodOver = now - lastAttempt_ >= period;
    bool due = false;
    if (failed_) due = periodOver;                       // retry at the next frame, not every millisecond
    else if (!lastSent_ || *lastSent_ != *tc) due = true;  // the frame changed (or a seek, or first)
    else if (snap.paused) due = periodOver;         // frozen frame: keep the console alive
    if (!due) return;
    lastAttempt_ = now;

    if (!bound_) {
        bound_ = out_->bind(boundTo_);
        if (!bound_) {
            failed_ = true;
            setStatus(ArtNetStatus::State::Error, "Art-Net: cannot use " + (boundTo_.isEmpty() ? juce::String("default adapter") : "network adapter " + boundTo_));
            return;
        }
    }
    const auto packet = encodeArtTimeCode(*tc, fps);
    if (!out_->send(destination_, packet.data(), static_cast<int>(packet.size()))) {
        failed_ = true;
        bound_ = false;  // a pulled cable can leave the socket dead: the retry binds a fresh one
        setStatus(ArtNetStatus::State::Error, "Art-Net: send to " + destination_ + " failed");
        return;
    }
    failed_ = false;
    lastSent_ = *tc;
    setStatus(ArtNetStatus::State::Sending, adapterNote_.isEmpty() ? juce::String("Art-Net: sending") : adapterNote_);
}

}  // namespace bigtop::io
