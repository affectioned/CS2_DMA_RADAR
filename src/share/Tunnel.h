#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

#include <windows.h>

// Tunnel — wraps a `cloudflared.exe tunnel --url http://localhost:PORT` child
// process to expose the ShareServer under a random *.trycloudflare.com URL.
// No account / no persistent subdomain — the URL rotates per invocation, so
// each session's public URL dies with the process.
//
// cloudflared.exe is looked up next to CS2_DMA_RADAR.exe. If it isn't there,
// Start() reports why and returns false; the overlay shows the error so the
// user can either drop the binary in place or leave the toggle off.
class Tunnel {
public:
    Tunnel();
    ~Tunnel();

    Tunnel(const Tunnel&)            = delete;
    Tunnel& operator=(const Tunnel&) = delete;

    bool Start(uint16_t localPort);
    void Stop();
    bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }

    // Resolved public URL (populated asynchronously after Start() while
    // cloudflared prints its banner). Empty until the handshake completes.
    std::string Url();

    // Non-empty when Start() failed or cloudflared exited unexpectedly;
    // UI surfaces this in place of the URL. Cleared on each Start().
    std::string LastError();

private:
    std::atomic<bool> m_Running{false};
    HANDLE            m_Process  = nullptr;
    HANDLE            m_StdoutRd = nullptr;
    std::thread       m_Reader;
    std::mutex        m_Mutex;
    std::string       m_Url;
    std::string       m_Error;

    void ReaderLoop();
};
