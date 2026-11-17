#include "pch.h"
#include "share/Tunnel.h"

#include <filesystem>
#include <regex>

namespace {
    std::filesystem::path cloudflaredPath() {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        return std::filesystem::path(exePath).parent_path() / L"cloudflared.exe";
    }
}

Tunnel::Tunnel() = default;

Tunnel::~Tunnel() {
    Stop();
}

bool Tunnel::Start(uint16_t localPort) {
    if (m_Running.load()) return true;

    auto exe = cloudflaredPath();
    if (!std::filesystem::exists(exe)) {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Error = "cloudflared.exe not found next to the radar — download it from "
                  "https://github.com/cloudflare/cloudflared/releases and drop it in.";
        Log::Warn("[Tunnel]: {}", m_Error);
        return false;
    }

    // Build the command line. --no-autoupdate avoids background self-update
    // chatter in stdout. --metrics 127.0.0.1:0 picks an ephemeral local
    // metrics port so a user running this twice on the same box doesn't get
    // a "metrics port in use" error.
    std::wstring cmd = L"\"" + exe.wstring() +
                       L"\" tunnel --no-autoupdate --metrics 127.0.0.1:0 --url http://localhost:" +
                       std::to_wstring(localPort);

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Error = "CreatePipe failed";
        return false;
    }
    // Only the write end is inherited by the child; the read end stays here.
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = wr;
    si.hStdError   = wr;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd;
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Error = "CreateProcess cloudflared.exe failed (" + std::to_string(::GetLastError()) + ")";
        Log::Error("[Tunnel]: {}", m_Error);
        return false;
    }
    CloseHandle(pi.hThread);

    m_Process  = pi.hProcess;
    m_StdoutRd = rd;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        m_Url.clear();
        m_Error.clear();
    }
    m_Running.store(true, std::memory_order_release);
    m_Reader = std::thread(&Tunnel::ReaderLoop, this);

    Log::Info("[Tunnel]: cloudflared.exe started (PID {})", pi.dwProcessId);
    return true;
}

void Tunnel::Stop() {
    if (!m_Running.exchange(false, std::memory_order_acq_rel)) return;

    if (m_Process) {
        // cloudflared responds to a plain TerminateProcess within a few ms;
        // we don't bother with CTRL+C because the console-attach dance is
        // flaky from a GUI app.
        TerminateProcess(m_Process, 0);
        WaitForSingleObject(m_Process, 2000);
        CloseHandle(m_Process);
        m_Process = nullptr;
    }
    if (m_StdoutRd) {
        CloseHandle(m_StdoutRd);
        m_StdoutRd = nullptr;
    }
    if (m_Reader.joinable()) m_Reader.join();

    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Url.clear();
}

std::string Tunnel::Url() {
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_Url;
}

std::string Tunnel::LastError() {
    std::lock_guard<std::mutex> lk(m_Mutex);
    return m_Error;
}

void Tunnel::ReaderLoop() {
    // cloudflared prints its startup banner on stderr, including the line
    // `INF |  https://<random>.trycloudflare.com                              |`
    // somewhere near the top. We stream bytes line-by-line and regex for it.
    static const std::regex urlRe(R"(https://[a-z0-9-]+\.trycloudflare\.com)");
    std::string buffer;
    char raw[1024];
    DWORD n = 0;

    while (m_Running.load(std::memory_order_acquire)) {
        BOOL ok = ReadFile(m_StdoutRd, raw, sizeof(raw), &n, nullptr);
        if (!ok || n == 0) {
            // Pipe closed (process exited or Stop() closed our end).
            if (m_Running.load()) {
                std::lock_guard<std::mutex> lk(m_Mutex);
                if (m_Url.empty() && m_Error.empty())
                    m_Error = "cloudflared.exe exited before producing a URL";
                m_Running.store(false);
            }
            return;
        }
        buffer.append(raw, n);
        size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);

            std::smatch m;
            if (std::regex_search(line, m, urlRe)) {
                std::lock_guard<std::mutex> lk(m_Mutex);
                if (m_Url.empty()) {
                    m_Url = m.str();
                    Log::Info("[Tunnel]: URL -> {}", m_Url);
                }
            }
        }
    }
}
