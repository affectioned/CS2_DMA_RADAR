#include "pch.h"
#include "share/ShareServer.h"
#include "share/webroot.h"
#include "sdk.h"

#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

using namespace std::chrono_literals;

// ── Winsock lifetime (ref-counted so we don't WSAStartup twice) ────────────────
namespace {
    std::atomic<int> g_WsaRef{0};
    void WsaAcquire() {
        if (g_WsaRef.fetch_add(1) == 0) {
            WSADATA d{};
            ::WSAStartup(MAKEWORD(2, 2), &d);
        }
    }
    void WsaRelease() {
        if (g_WsaRef.fetch_sub(1) == 1) ::WSACleanup();
    }

    constexpr const char* kWsMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    bool caseInsensitiveContains(const std::string& hay, const std::string& needle) {
        auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
            [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
        return it != hay.end();
    }

    // Extract a header value from a raw HTTP request. Case-insensitive on the
    // name; the value is trimmed. Returns empty string on miss.
    std::string httpHeader(const std::string& req, const std::string& name) {
        size_t pos = 0;
        while (pos < req.size()) {
            size_t eol = req.find("\r\n", pos);
            if (eol == std::string::npos) break;
            std::string line = req.substr(pos, eol - pos);
            pos = eol + 2;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string k = line.substr(0, colon);
            if (k.size() != name.size()) continue;
            bool match = true;
            for (size_t i = 0; i < k.size(); ++i)
                if (std::tolower((unsigned char)k[i]) != std::tolower((unsigned char)name[i])) { match = false; break; }
            if (!match) continue;
            std::string v = line.substr(colon + 1);
            size_t s = v.find_first_not_of(" \t");
            size_t e = v.find_last_not_of(" \t\r");
            if (s == std::string::npos) return "";
            return v.substr(s, e - s + 1);
        }
        return "";
    }

    // SHA1 → base64 via Windows CNG. Used once per WS handshake.
    std::string sha1Base64(const std::string& input) {
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
        DWORD hashLen = 0, cb = 0;
        BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb, 0);
        std::vector<UCHAR> hash(hashLen);
        BCRYPT_HASH_HANDLE hHash = nullptr;
        BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
        BCryptHashData(hHash, (PUCHAR)input.data(), (ULONG)input.size(), 0);
        BCryptFinishHash(hHash, hash.data(), hashLen, 0);
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);

        static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < hash.size(); i += 3) {
            uint32_t v = hash[i] << 16;
            if (i + 1 < hash.size()) v |= hash[i + 1] << 8;
            if (i + 2 < hash.size()) v |= hash[i + 2];
            out.push_back(b64[(v >> 18) & 63]);
            out.push_back(b64[(v >> 12) & 63]);
            out.push_back(i + 1 < hash.size() ? b64[(v >> 6) & 63] : '=');
            out.push_back(i + 2 < hash.size() ? b64[v & 63]        : '=');
        }
        return out;
    }

    // Emit a JSON-safe string literal (quotes included). Escapes control chars
    // and the mandatory \ and " characters; everything else passes through.
    void jsonStr(std::string& out, const char* s) {
        out.push_back('"');
        for (; *s; ++s) {
            unsigned char c = (unsigned char)*s;
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back((char)c);
                    }
            }
        }
        out.push_back('"');
    }

    void jsonNum(std::string& out, float v) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", v);
        out += buf;
    }

    void jsonInt(std::string& out, long long v) {
        out += std::to_string(v);
    }
}

// ── ShareServer ───────────────────────────────────────────────────────────────

ShareServer::ShareServer(const CGame& game, std::mutex& gameMutex)
    : m_Game(game), m_GameMutex(gameMutex), m_ListenSock(INVALID_SOCKET) {
    WsaAcquire();
}

ShareServer::~ShareServer() {
    Stop();
    WsaRelease();
}

bool ShareServer::Start(uint16_t port) {
    if (m_Running.load()) return true;

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        Log::Error("[Share]: socket() failed, WSA={}", ::WSAGetLastError());
        return false;
    }
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log::Error("[Share]: bind({}) failed, WSA={}", port, ::WSAGetLastError());
        ::closesocket(s);
        return false;
    }
    if (::listen(s, 8) == SOCKET_ERROR) {
        Log::Error("[Share]: listen() failed, WSA={}", ::WSAGetLastError());
        ::closesocket(s);
        return false;
    }

    m_ListenSock = s;
    m_Port       = port;
    m_Running.store(true, std::memory_order_release);
    ResolveUrls();

    m_AcceptThread    = std::thread(&ShareServer::AcceptLoop,    this);
    m_BroadcastThread = std::thread(&ShareServer::BroadcastLoop, this);

    Log::Info("[Share]: Listening on port {}", port);
    for (const auto& u : m_Urls) Log::Info("[Share]:   {}", u);
    return true;
}

void ShareServer::Stop() {
    if (!m_Running.exchange(false, std::memory_order_acq_rel)) return;

    if (m_ListenSock != INVALID_SOCKET) {
        ::closesocket(m_ListenSock);
        m_ListenSock = INVALID_SOCKET;
    }
    if (m_AcceptThread.joinable())    m_AcceptThread.join();
    if (m_BroadcastThread.joinable()) m_BroadcastThread.join();

    {
        std::lock_guard<std::mutex> lk(m_ClientsMutex);
        for (auto& c : m_Clients) {
            if (c->alive.load()) {
                SendWSClose(c->sock);
                ::closesocket(c->sock);
                c->alive.store(false);
            }
        }
        m_Clients.clear();
    }
    m_Urls.clear();
    Log::Info("[Share]: Stopped");
}

void ShareServer::ResolveUrls() {
    m_Urls.clear();
    m_Urls.push_back("http://localhost:" + std::to_string(m_Port));

    // GetAdaptersAddresses gives IPv4 unicast addrs for every up adapter.
    // Filter to private LAN ranges — 10/8, 172.16/12, 192.168/16 — so we don't
    // surface link-local or auto-config addresses that nothing routes to.
    ULONG size = 15 * 1024;
    std::vector<unsigned char> buf(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
                               nullptr, adapters, &size) != NO_ERROR)
        return;

    for (auto* a = adapters; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sa = (sockaddr_in*)u->Address.lpSockaddr;
            uint32_t ip = ntohl(sa->sin_addr.s_addr);
            bool privateNet =
                (ip >> 24) == 10 ||
                ((ip >> 20) == ((172u << 4) | 1)) ||     // 172.16.0.0/12
                ((ip >> 16) == ((192u << 8) | 168));     // 192.168.0.0/16
            if (!privateNet) continue;
            char ipStr[32];
            ::inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));
            m_Urls.push_back(std::string("http://") + ipStr + ":" + std::to_string(m_Port));
        }
    }
}

void ShareServer::AcceptLoop() {
    while (m_Running.load(std::memory_order_acquire)) {
        sockaddr_in peer{};
        int plen = sizeof(peer);
        SOCKET cs = ::accept(m_ListenSock, (sockaddr*)&peer, &plen);
        if (cs == INVALID_SOCKET) {
            if (!m_Running.load()) break;
            std::this_thread::sleep_for(50ms);
            continue;
        }
        // Each connection runs on its own detached thread; typical clients are
        // one GET for /index.html + one long-lived /ws, so the fan-out is tiny.
        std::thread([this, cs]{ HandleConnection(cs); }).detach();
    }
}

bool ShareServer::SendAll(SOCKET sock, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
        int n = ::send(sock, p, (int)len, 0);
        if (n <= 0) return false;
        p   += n;
        len -= (size_t)n;
    }
    return true;
}

void ShareServer::HandleConnection(SOCKET sock) {
    // Read the HTTP request headers. We stop at CRLFCRLF; anything a browser
    // puts in the body doesn't matter for GET/WS upgrade.
    std::string req;
    req.reserve(1024);
    char buf[2048];
    DWORD timeout = 5000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        int n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) { ::closesocket(sock); return; }
        req.append(buf, (size_t)n);
    }
    if (req.find("\r\n\r\n") == std::string::npos) { ::closesocket(sock); return; }

    // WebSocket upgrade if the client asked for one on any path — we only use
    // /ws but accept any to keep clients simple.
    std::string upgrade = httpHeader(req, "Upgrade");
    if (!upgrade.empty() && caseInsensitiveContains(upgrade, "websocket")) {
        if (DoWebSocketHandshake(sock, req)) return; // ownership passed to broadcast loop
        ::closesocket(sock);
        return;
    }

    ServeHttp(sock, req);
    ::closesocket(sock);
}

bool ShareServer::DoWebSocketHandshake(SOCKET sock, const std::string& req) {
    std::string key = httpHeader(req, "Sec-WebSocket-Key");
    if (key.empty()) return false;
    std::string accept = sha1Base64(key + kWsMagic);

    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
    std::string s = resp.str();
    if (!SendAll(sock, s.data(), s.size())) return false;

    auto client = std::make_shared<Client>();
    client->sock = sock;
    {
        std::lock_guard<std::mutex> lk(m_ClientsMutex);
        m_Clients.push_back(client);
    }
    Log::Info("[Share]: WS client connected ({} total)", m_Clients.size());
    return true;
}

// Serve either the embedded index.html or a file from the texture tree
// relative to the exe's working directory. Only GET requests for a small
// whitelist of paths are honored; anything else is 404. No directory
// traversal — the path after the whitelist prefix is lowercased+sanitized.
void ShareServer::ServeHttp(SOCKET sock, const std::string& req) {
    size_t sp1 = req.find(' ');
    size_t sp2 = req.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    std::string method = req.substr(0, sp1);
    std::string path   = req.substr(sp1 + 1, sp2 - sp1 - 1);
    if (method != "GET") {
        std::string r = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        SendAll(sock, r.data(), r.size());
        return;
    }

    auto sendText = [&](const char* type, const std::string& body) {
        std::ostringstream h;
        h << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: " << type << "\r\n"
          << "Content-Length: " << body.size() << "\r\n"
          << "Cache-Control: no-store\r\n\r\n";
        SendAll(sock, h.str().data(), h.str().size());
        SendAll(sock, body.data(), body.size());
    };
    auto sendFile = [&](const std::filesystem::path& fp, const char* type) {
        std::ifstream f(fp, std::ios::binary);
        if (!f) {
            std::string r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            SendAll(sock, r.data(), r.size());
            return;
        }
        std::ostringstream body;
        body << f.rdbuf();
        std::string b = body.str();
        std::ostringstream h;
        h << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: " << type << "\r\n"
          << "Content-Length: " << b.size() << "\r\n"
          << "Cache-Control: public, max-age=3600\r\n\r\n";
        SendAll(sock, h.str().data(), h.str().size());
        SendAll(sock, b.data(), b.size());
    };

    if (path == "/" || path == "/index.html") {
        sendText("text/html; charset=utf-8", kWebRootHtml);
        return;
    }

    // Strip query string. Only serve the two whitelisted prefixes so no one
    // can path-traverse out to cs2radar.log or somewhere else next to the exe.
    size_t q = path.find('?');
    if (q != std::string::npos) path.resize(q);

    const std::string texPfx = "/textures/maps/";
    if (path.rfind(texPfx, 0) == 0) {
        std::string name = path.substr(texPfx.size());
        bool safe = !name.empty();
        // Allow a-z A-Z 0-9 plus . _ - only; the '.' is needed for .png / .txt
        // extensions. Any path separator is already excluded by that whitelist.
        for (char c : name) {
            if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) { safe = false; break; }
        }
        // Reject any ".." sequence explicitly
        if (name.find("..") != std::string::npos) safe = false;
        if (!safe) {
            std::string r = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            SendAll(sock, r.data(), r.size());
            return;
        }
        auto fp = std::filesystem::current_path() / "textures" / "maps" / name;
        const char* type = "application/octet-stream";
        if (name.size() >= 4) {
            if (name.rfind(".png") == name.size() - 4) type = "image/png";
            else if (name.rfind(".txt") == name.size() - 4) type = "text/plain; charset=utf-8";
        }
        sendFile(fp, type);
        return;
    }

    std::string r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    SendAll(sock, r.data(), r.size());
}

bool ShareServer::SendWSText(SOCKET sock, const std::string& payload) {
    // Server-to-client frames don't need masking. Build the header for a
    // single text frame (FIN=1, opcode=0x1) with the three RFC 6455 length
    // encodings: ≤125 fits in the first byte, ≤65535 uses a 16-bit field,
    // bigger uses a 64-bit field.
    std::vector<unsigned char> hdr;
    hdr.push_back(0x81);
    size_t n = payload.size();
    if (n <= 125) {
        hdr.push_back((unsigned char)n);
    } else if (n <= 0xFFFF) {
        hdr.push_back(126);
        hdr.push_back((unsigned char)(n >> 8));
        hdr.push_back((unsigned char)(n & 0xFF));
    } else {
        hdr.push_back(127);
        uint64_t nn = n;
        for (int i = 7; i >= 0; --i) hdr.push_back((unsigned char)((nn >> (i * 8)) & 0xFF));
    }
    if (!SendAll(sock, hdr.data(), hdr.size())) return false;
    if (!SendAll(sock, payload.data(), payload.size())) return false;
    return true;
}

void ShareServer::SendWSClose(SOCKET sock) {
    unsigned char frame[4] = { 0x88, 0x02, 0x03, 0xE8 }; // 1000 normal closure
    SendAll(sock, frame, sizeof(frame));
}

void ShareServer::PruneDead() {
    std::lock_guard<std::mutex> lk(m_ClientsMutex);
    m_Clients.erase(std::remove_if(m_Clients.begin(), m_Clients.end(),
        [](const std::shared_ptr<Client>& c) {
            if (!c->alive.load()) { ::closesocket(c->sock); return true; }
            return false;
        }), m_Clients.end());
}

std::string ShareServer::BuildFrameJson() {
    // Snapshot under the same lock the render thread uses, then release
    // before touching the socket. CGame is copyable (POD-ish), so this is
    // cheap vs. serializing while holding the lock.
    CGame snap;
    {
        std::lock_guard<std::mutex> lk(m_GameMutex);
        snap = m_Game;
    }

    std::string out;
    out.reserve(2048);
    out.push_back('{');

    // map + signOnState
    out += "\"map\":";
    jsonStr(out, snap.mapName);
    out += ",\"signOn\":";
    jsonInt(out, snap.signOnState);

    // players — anonymized: no "local" flag, same roster order every frame.
    out += ",\"players\":[";
    bool first = true;
    for (size_t i = 0; i < MAX_ENTITIES; ++i) {
        const CPlayer& p = snap.players[i];
        if (!p.controllerBase) continue;
        if (p.ctrl.teamID < TEAM_T) continue; // skip unassigned / spectators
        if (p.pawn.lifeState != 0) continue;
        if (!first) out.push_back(',');
        first = false;
        out.push_back('{');
        out += "\"team\":";
        jsonStr(out, p.ctrl.teamID == TEAM_T ? "T" : "CT");
        out += ",\"pos\":[";
        jsonNum(out, p.pawn.position.x); out.push_back(',');
        jsonNum(out, p.pawn.position.y); out.push_back(',');
        jsonNum(out, p.pawn.position.z);
        out += "],\"yaw\":";
        jsonNum(out, p.pawn.eyeAngles.y);
        out += ",\"hp\":";
        jsonInt(out, (long long)p.pawn.health);
        if (p.pawn.isDefusing) out += ",\"defusing\":true";
        out.push_back('}');
    }
    out += "]";

    // bomb state — observational, no identity.
    const C_PlantedC4& b = snap.bomb;
    out += ",\"bomb\":{";
    if (b.hasDefused)      out += "\"state\":\"defused\"";
    else if (b.hasExploded) out += "\"state\":\"exploded\"";
    else if (b.entity && b.isTicking) {
        out += "\"state\":\"planted\"";
        out += ",\"time\":"; jsonNum(out, b.timeRemaining());
        out += ",\"site\":"; jsonStr(out, b.site == 0 ? "A" : b.site == 1 ? "B" : "?");
        out += ",\"pos\":[";
        jsonNum(out, b.position.x); out.push_back(',');
        jsonNum(out, b.position.y); out.push_back(',');
        jsonNum(out, b.position.z);
        out += "]";
        if (b.isBeingDefused) out += ",\"defusing\":true";
    }
    else out += "\"state\":\"none\"";
    out.push_back('}');

    out.push_back('}');
    return out;
}

void ShareServer::BroadcastLoop() {
    constexpr auto kInterval = std::chrono::milliseconds(33); // ~30 Hz
    auto next = std::chrono::steady_clock::now() + kInterval;

    while (m_Running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_until(next);
        next += kInterval;

        PruneDead();

        std::vector<std::shared_ptr<Client>> snapshot;
        {
            std::lock_guard<std::mutex> lk(m_ClientsMutex);
            snapshot = m_Clients;
        }
        if (snapshot.empty()) continue;

        std::string frame = BuildFrameJson();
        for (auto& c : snapshot) {
            if (!c->alive.load()) continue;
            if (!SendWSText(c->sock, frame)) {
                c->alive.store(false);
            }
        }
    }
}
