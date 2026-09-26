#pragma once
#include <stdint.h>

// Serializes every heavy internal-heap-hungry operation across the whole
// app - originally just HTTPS (WiFiClientSecure) calls (bus.cpp, today.cpp,
// ask.cpp, bus_stops.cpp all fetch on their own FreeRTOS tasks, and two TLS
// handshakes happening at the same moment were found, via real serial
// evidence not guesswork, to exhaust the ESP32's small internal (non-PSRAM)
// heap that mbedTLS/esp-sha needs for its buffers: "SSL - Memory allocation
// failed"/"esp-sha: Failed to allocate buf memory", both connections
// failing with HTTP -1 at once), later extended to mic_capture.cpp's/
// speaker.cpp's I2S sessions too once the exact same class of failure
// showed up there ("Error malloc dma buffer"/"I2S1 rx DMA buffer malloc
// failed") - the I2S driver's DMA buffers draw from that same scarce
// internal heap pool, and a background bus/today fetch task's ~20KB stack
// being alive at the same moment was enough to starve it. See CLAUDE.md's
// postmortem for the full story.
void netLockInit(); // call once from setup(), before any fetch can start

// Force the lock back to "available" regardless of who (if anyone) holds
// it. Needed because a watchdog force-killing a wedged fetch task via
// vTaskDelete() can't run that task's NetLockGuard destructor any more than
// it can run WiFiClientSecure's - if the killed task happened to be
// holding this lock, it would otherwise stay held forever, permanently
// blocking every other tab's network calls. Safe to call even when nothing
// holds the lock (a no-op in that case) - implemented as a binary
// semaphore specifically so "give" from a task that isn't the current
// holder is well-defined, unlike a real FreeRTOS mutex.
void netLockForceRelease();

// True once Wi-Fi has reported WL_CONNECTED continuously for a short
// grace period (~3s). Real serial evidence (not guessed) showed the very
// first HTTPS attempt made in the same instant Wi-Fi transitions to
// connected can fail with "SSL - Memory allocation failed" even with no
// other fetch competing for the lock above - most likely the Wi-Fi/lwIP
// stack's own post-connect work (DHCP, ARP, etc.) transiently competing
// for the same internal heap mbedTLS needs for its handshake buffers.
// Callers should check this instead of (or alongside) a bare
// WiFi.status() == WL_CONNECTED before starting a fetch.
bool wifiSettled();

// RAII guard, same "let C++ destructors do cleanup" convention already used
// for WiFiClientSecure/HTTPClient throughout this codebase - acquire at the
// top of a fetch function (wrapping the whole network operation, not just
// the handshake, since only one HTTPS call is useful at a time on this
// device anyway), it releases automatically on scope exit. Always check
// acquired() before touching the network - a timed-out acquire means
// someone else is mid-fetch, not that anything is broken.
class NetLockGuard
{
public:
    // Default of 60s matches the fetch watchdogs' own worst-case ceiling
    // (bus.cpp/today.cpp/ask.cpp - see their comments on why a single
    // fetch can legitimately take that long, mostly DNS resolution) - a
    // queued caller should be willing to wait roughly as long as the
    // current holder is allowed to legitimately run before being force-
    // killed (which releases this lock too, via netLockForceRelease()).
    explicit NetLockGuard(uint32_t timeoutMs = 60000);
    ~NetLockGuard();
    bool acquired() const { return ok; }

private:
    bool ok;
};
