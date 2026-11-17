#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#ifndef _WINSOCKAPI_
#include <winsock2.h>
#endif

class CGame;

// ShareServer — local HTTP + WebSocket server.
//
// Serves a self-contained web radar at http://<host>:<port>/ and streams an
// anonymized game-state JSON frame per tick to every connected client.
// "Anonymized" here means: the stream carries no "this is the sharer"
// marker. Every player is just a T/CT roster entry; viewers see a
// spectator-POV radar with no friendly/enemy coloring derived from who's
// running the overlay. Rotation is also disabled on the shared view.
//
// Threading: Start() spawns an accept thread and a broadcast thread. The
// game-state read is taken under m_GameMutex (same lock the render thread
// uses) and copied into a local CGame before serialization.
class ShareServer {
public:
    ShareServer(const CGame& game, std::mutex& gameMutex);
    ~ShareServer();

    ShareServer(const ShareServer&)            = delete;
    ShareServer& operator=(const ShareServer&) = delete;

    bool     Start(uint16_t port);
    void     Stop();
    bool     IsRunning() const { return m_Running.load(std::memory_order_acquire); }
    uint16_t Port()      const { return m_Port; }

    // Reachable URLs for this server (IPv4 LAN addresses + loopback), resolved
    // at Start() time. Does not include any tunnel URL — that's a separate layer.
    const std::vector<std::string>& Urls() const { return m_Urls; }

private:
    const CGame& m_Game;
    std::mutex&  m_GameMutex;

    std::atomic<bool> m_Running{false};
    uint16_t          m_Port       = 0;
    SOCKET            m_ListenSock;  // init in ctor
    std::thread       m_AcceptThread;
    std::thread       m_BroadcastThread;

    struct Client {
        SOCKET            sock;
        std::atomic<bool> alive{true};
    };

    std::mutex                           m_ClientsMutex;
    std::vector<std::shared_ptr<Client>> m_Clients;
    std::vector<std::string>             m_Urls;

    void   AcceptLoop();
    void   BroadcastLoop();
    void   HandleConnection(SOCKET sock);
    bool   DoWebSocketHandshake(SOCKET sock, const std::string& req);
    void   ServeHttp(SOCKET sock, const std::string& req);
    bool   SendAll(SOCKET sock, const void* data, size_t len);
    bool   SendWSText(SOCKET sock, const std::string& payload);
    void   SendWSClose(SOCKET sock);
    void   PruneDead();
    void   ResolveUrls();
    std::string BuildFrameJson();
};
