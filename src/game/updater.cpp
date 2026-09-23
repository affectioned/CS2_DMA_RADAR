#include "pch.h"
#include "updater.h"
#include "offsets.h"
#include "DMA/Memory/SigScan.h"

#include <regex>
#include <sstream>
#include <wininet.h>
#pragma comment(lib, "wininet")

namespace {

constexpr const char* kClientDllUrl =
    "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.hpp";
constexpr const char* kOffsetsUrl =
    "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/offsets.hpp";

struct HttpResult {
    DWORD       status = 0;     // 0 = network failure, otherwise HTTP status code
    std::string body;           // empty on 304 / failure
    std::string etag;           // ETag header value (preserve exactly as received)
};

HttpResult httpGet(const char* url, const std::string& ifNoneMatch)
{
    HttpResult r{};

    HINTERNET hNet = InternetOpenA("CS2Radar", INTERNET_OPEN_TYPE_PRECONFIG,
                                   nullptr, nullptr, 0);
    if (!hNet) return r;

    std::string headers;
    if (!ifNoneMatch.empty())
        headers = "If-None-Match: " + ifNoneMatch + "\r\n";

    HINTERNET hUrl = InternetOpenUrlA(
        hNet, url,
        headers.empty() ? nullptr : headers.c_str(),
        headers.empty() ? 0u : static_cast<DWORD>(headers.size()),
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
        0);
    if (!hUrl) { InternetCloseHandle(hNet); return r; }

    DWORD status = 0, statusLen = sizeof(status), idx = 0;
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &statusLen, &idx))
        r.status = status;

    char etagBuf[256] = {};
    DWORD etagLen = sizeof(etagBuf) - 1;
    idx = 0;
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_ETAG, etagBuf, &etagLen, &idx))
        r.etag.assign(etagBuf, etagLen);

    if (r.status == 200) {
        char buf[8192]; DWORD n = 0;
        while (InternetReadFile(hUrl, buf, sizeof(buf), &n) && n > 0)
            r.body.append(buf, n);
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);
    return r;
}

std::filesystem::path exeDir()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeFile(const std::filesystem::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    return f.good();
}

// Each namespace's body is regex-scanned for `identifier = 0xHEX` pairs. We
// advance past the OPEN brace (not close) so nested namespaces like
// `cs2_dumper::schemas::client_dll::C_BaseEntity` are picked up too.

using OffsetMap    = std::unordered_map<std::string, std::ptrdiff_t>;
using NamespaceMap = std::unordered_map<std::string, OffsetMap>;

NamespaceMap parseHeader(const std::string& src)
{
    NamespaceMap out;
    static const std::regex fieldRx(R"((\w+)\s*=\s*0x([0-9A-Fa-f]+))");

    size_t pos = 0;
    while (pos < src.size()) {
        size_t ns = src.find("namespace ", pos);
        if (ns == std::string::npos) break;

        size_t nameStart = ns + 10;
        size_t nameEnd   = nameStart;
        while (nameEnd < src.size() &&
               (isalnum(static_cast<unsigned char>(src[nameEnd])) || src[nameEnd] == '_'))
            nameEnd++;
        if (nameEnd == nameStart) { pos = nameStart; continue; }

        std::string name = src.substr(nameStart, nameEnd - nameStart);

        size_t openBrace = src.find('{', nameEnd);
        if (openBrace == std::string::npos) break;

        int depth = 1;
        size_t i = openBrace + 1;
        for (; i < src.size() && depth > 0; i++) {
            if      (src[i] == '{') depth++;
            else if (src[i] == '}') depth--;
        }
        if (depth != 0) break;

        std::string body(src.begin() + openBrace + 1, src.begin() + i - 1);
        OffsetMap m;
        for (auto it = std::sregex_iterator(body.begin(), body.end(), fieldRx);
             it != std::sregex_iterator(); ++it)
        {
            m[(*it)[1].str()] =
                static_cast<std::ptrdiff_t>(std::stoull((*it)[2].str(), nullptr, 16));
        }
        if (!m.empty()) out[name] = std::move(m);
        pos = openBrace + 1;
    }
    return out;
}

struct OffsetSpec { const char* cls; const char* field; std::ptrdiff_t* target; };

const OffsetSpec kClassSpecs[] = {
    { "C_BaseEntity",            "m_iHealth",               &client_dll::C_BaseEntity::m_iHealth },
    { "C_BaseEntity",            "m_lifeState",             &client_dll::C_BaseEntity::m_lifeState },
    { "C_BaseEntity",            "m_iTeamNum",              &client_dll::C_BaseEntity::m_iTeamNum },
    { "C_BaseEntity",            "m_pGameSceneNode",        &client_dll::C_BaseEntity::m_pGameSceneNode },
    { "C_BaseEntity",            "m_hOwnerEntity",          &client_dll::C_BaseEntity::m_hOwnerEntity },
    { "CGameSceneNode",          "m_vecAbsOrigin",          &client_dll::CGameSceneNode::m_vecAbsOrigin },
    { "C_BasePlayerPawn",        "m_pWeaponServices",       &client_dll::C_BasePlayerPawn::m_pWeaponServices },
    { "C_BasePlayerPawn",        "m_pObserverServices",     &client_dll::C_BasePlayerPawn::m_pObserverServices },
    { "C_BasePlayerPawn",        "m_vOldOrigin",            &client_dll::C_BasePlayerPawn::m_vOldOrigin },
    { "CPlayer_WeaponServices",  "m_hActiveWeapon",         &client_dll::CPlayer_WeaponServices::m_hActiveWeapon },
    { "CPlayer_ObserverServices", "m_hObserverTarget",      &client_dll::CPlayer_ObserverServices::m_hObserverTarget },
    { "CCSPlayerController",     "m_sSanitizedPlayerName",  &client_dll::CCSPlayerController::m_sSanitizedPlayerName },
    { "CCSPlayerController",     "m_iCompTeammateColor",    &client_dll::CCSPlayerController::m_iCompTeammateColor },
    { "CCSPlayerController",     "m_hPlayerPawn",           &client_dll::CCSPlayerController::m_hPlayerPawn },
    { "CCSPlayerController",     "m_iPawnArmor",            &client_dll::CCSPlayerController::m_iPawnArmor },
    { "CCSPlayerController",     "m_bPawnHasDefuser",       &client_dll::CCSPlayerController::m_bPawnHasDefuser },
    { "CCSPlayerController",     "m_bPawnHasHelmet",        &client_dll::CCSPlayerController::m_bPawnHasHelmet },
    { "C_CSPlayerPawn",          "m_szLastPlaceName",       &client_dll::C_CSPlayerPawn::m_szLastPlaceName },
    { "C_CSPlayerPawn",          "m_bIsDefusing",           &client_dll::C_CSPlayerPawn::m_bIsDefusing },
    { "C_CSPlayerPawn",          "m_angEyeAngles",          &client_dll::C_CSPlayerPawn::m_angEyeAngles },
    { "C_PlantedC4",             "m_bBombTicking",          &client_dll::C_PlantedC4::m_bBombTicking },
    { "C_PlantedC4",             "m_nBombSite",             &client_dll::C_PlantedC4::m_nBombSite },
    { "C_PlantedC4",             "m_flC4Blow",              &client_dll::C_PlantedC4::m_flC4Blow },
    { "C_PlantedC4",             "m_bHasExploded",          &client_dll::C_PlantedC4::m_bHasExploded },
    { "C_PlantedC4",             "m_bBeingDefused",         &client_dll::C_PlantedC4::m_bBeingDefused },
    { "C_PlantedC4",             "m_bC4Activated",          &client_dll::C_PlantedC4::m_bC4Activated },
    { "C_PlantedC4",             "m_bBombDefused",          &client_dll::C_PlantedC4::m_bBombDefused },
    { "C_EconEntity",            "m_AttributeManager",      &client_dll::C_EconEntity::m_AttributeManager },
    { "C_AttributeContainer",    "m_Item",                  &client_dll::C_AttributeContainer::m_Item },
    { "C_EconItemView",          "m_iItemDefinitionIndex",  &client_dll::C_EconItemView::m_iItemDefinitionIndex },
    { "C_CSGameRulesProxy",      "m_pGameRules",            &client_dll::C_CSGameRulesProxy::m_pGameRules },
    { "C_CSGameRules",           "m_iRoundEndWinnerTeam",   &client_dll::C_CSGameRules::m_iRoundEndWinnerTeam },
};

// Each offset has exactly one source: pattern-match (live client.dll resolution) OR
// fetch (cs2-dumper offsets.hpp). Don't list pattern-resolved fields here — keeping
// two sources for the same field creates a stale-data trap.
const OffsetSpec kModuleSpecs[] = {
    { "engine2_dll", "dwBuildNumber",                   &engine2_dll::dwBuildNumber },
    { "engine2_dll", "dwNetworkGameClient",             &engine2_dll::dwNetworkGameClient },
    { "engine2_dll", "dwNetworkGameClient_signOnState", &engine2_dll::dwNetworkGameClient_signOnState },
};

int applyOffsets(const OffsetSpec* specs, int specCount, const NamespaceMap& parsed,
                 const char* label)
{
    int matched = 0, changed = 0;
    for (int i = 0; i < specCount; i++) {
        const auto& spec = specs[i];
        auto cit = parsed.find(spec.cls);
        if (cit == parsed.end()) continue;
        auto fit = cit->second.find(spec.field);
        if (fit == cit->second.end()) continue;

        matched++;
        if (*spec.target != fit->second) {
            Log::Info("[Updater]: {}::{} 0x{:X} -> 0x{:X}",
                      spec.cls, spec.field, *spec.target, fit->second);
            *spec.target = fit->second;
            changed++;
        }
    }
    if (changed == 0 && matched > 0)
        Log::Info("[Updater]: {} {} offsets verified, none changed", matched, label);
    return matched;
}

struct OffsetSource {
    const char*       url;
    const char*       cacheName;   // filename, sans directory
    const OffsetSpec* specs;
    int               specCount;
    const char*       label;       // for log lines, e.g. "class" or "module"
};

bool fetchAndApply(const OffsetSource& src)
{
    const auto dir       = exeDir();
    const auto cachePath = dir / src.cacheName;
    const auto etagPath  = dir / (std::string(src.cacheName) + ".etag");

    Log::Info("[Updater]: Fetching {} offsets ({})...", src.label, src.cacheName);

    std::string cachedEtag = readFile(etagPath);
    HttpResult  res        = httpGet(src.url, cachedEtag);

    std::string body;
    if (res.status == 304) {
        body = readFile(cachePath);
        Log::Info("[Updater]: 304 Not Modified - using cached {} ({} bytes)",
                  src.cacheName, body.size());
    }
    else if (res.status == 200 && !res.body.empty()) {
        body = std::move(res.body);
        if (!writeFile(cachePath, body))
            Log::Warn("[Updater]: failed to write cache to {}", cachePath.string());
        if (!res.etag.empty() && !writeFile(etagPath, res.etag))
            Log::Warn("[Updater]: failed to write etag to {}", etagPath.string());
        Log::Info("[Updater]: 200 OK - cached fresh {} ({} bytes)",
                  src.cacheName, body.size());
    }
    else {
        Log::Warn("[Updater]: HTTP fetch failed (status={}) - falling back to disk cache",
                  res.status);
        body = readFile(cachePath);
    }

    if (body.empty()) {
        Log::Warn("[Updater]: no {} available - using compiled-in defaults", src.cacheName);
        return false;
    }

    auto parsed = parseHeader(body);
    if (parsed.empty()) {
        Log::Warn("[Updater]: parsed 0 namespaces from {} - using compiled-in defaults",
                  src.cacheName);
        return false;
    }

    const int matched = applyOffsets(src.specs, src.specCount, parsed, src.label);
    Log::Info("[Updater]: {}/{} {} offsets resolved", matched, src.specCount, src.label);
    return matched > 0;
}

} // anonymous namespace

// ── Public entry points ───────────────────────────────────────────────────────

bool updater::fetchClassOffsets()
{
    return fetchAndApply({
        kClientDllUrl, "client_dll.hpp",
        kClassSpecs, static_cast<int>(sizeof(kClassSpecs) / sizeof(*kClassSpecs)),
        "class"
    });
}

bool updater::fetchModuleOffsets()
{
    return fetchAndApply({
        kOffsetsUrl, "offsets.hpp",
        kModuleSpecs, static_cast<int>(sizeof(kModuleSpecs) / sizeof(*kModuleSpecs)),
        "module"
    });
}

// ── RIP-relative resolver ──────────────────────────────────────────────────────
// Resolve a RIP-relative MOV/LEA reference and return the module-relative RVA.
// sigHit   – absolute address of the first byte of the matched instruction
// dispOff  – byte offset within the instruction where the 32-bit displacement lives
// instrSz  – total size of the instruction (RIP advances past it before applying disp)
// Returns 0 if the resolved address is not readable (false-positive match).
static ptrdiff_t ResolveRIP(DMA_Connection* Conn, DWORD pid,
                             uintptr_t clientBase, uint64_t sigHit,
                             int dispOff, int instrSz)
{
    int32_t disp = ReadFromPID<int32_t>(Conn, sigHit + dispOff, pid);
    uintptr_t absAddr = static_cast<uintptr_t>(sigHit) + instrSz + disp;
    if (!IsAddressReadable(Conn, absAddr, pid))
        return 0;
    return static_cast<ptrdiff_t>(absAddr - clientBase);
}

// Scan for sig, resolve the RIP-relative displacement, write target.
// On sig failure, preserve the existing target value (which may have been
// populated earlier by updater::fetchModuleOffsets from cs2-dumper). This
// gives pattern-match-first / fetch-second / compiled-in-third resilience.
static void ResolveOffset(DMA_Connection* Conn, DWORD pid, uintptr_t clientBase, uintptr_t clientEnd,
                           const char* name, ptrdiff_t& target,
                           const char* sig, int dispOff, int instrSz)
{
    uint64_t hit = FindSignature(Conn, sig, clientBase, clientEnd, pid);
    ptrdiff_t offset = hit ? ResolveRIP(Conn, pid, clientBase, hit, dispOff, instrSz) : 0;

    if (offset)
    {
        target = offset;
        Log::Info("[Updater]: {} = 0x{:X}", name, target);
    }
    else if (target)
    {
        Log::Warn("[Updater]: {} sig failed, keeping existing 0x{:X} (fetched/compiled-in)",
                  name, target);
    }
    else
    {
        Log::Warn("[Updater]: {} sig failed, no fallback available - reads will return zeros",
                  name);
    }
}

// ── resolveOffsets ────────────────────────────────────────────────────────────

bool updater::resolveOffsets(DMA_Connection* conn, Process* proc)
{
    Log::Info("[Updater]: Scanning RVA pointers...");

    const DWORD     pid        = proc->GetPID();
    const uintptr_t clientBase = proc->GetModuleBase("client.dll");
    const uintptr_t clientEnd  = clientBase + proc->GetModuleSize("client.dll");

    if (!clientBase)
    {
        Log::Error("[Updater]: client.dll base not found, aborting.");
        return false;
    }

    int resolved = 0, total = 0;

    auto scanRIP = [&](ptrdiff_t& t, const char* sig, int dispOff, int instrSz, const char* name) {
        total++;
        ResolveOffset(conn, pid, clientBase, clientEnd, name, t, sig, dispOff, instrSz);
        if (t) resolved++;
    };

    // ── Module-level RVA pointers ─────────────────────────────────────────────
    // All are 7-byte RIP-relative instructions (3-byte opcode+ModRM + 4-byte disp).

    scanRIP(client_dll::dwEntityList,            "48 89 0D ? ? ? ? E9 ? ? ? ? CC",                                                  3, 7, "dwEntityList");
    scanRIP(client_dll::dwLocalPlayerController, "48 8B 05 ? ? ? ? 41 89 BE",                                                       3, 7, "dwLocalPlayerController");
    scanRIP(client_dll::dwGlobalVars,            "48 89 15 ? ? ? ? 48 89 42",                                                       3, 7, "dwGlobalVars");
    scanRIP(client_dll::dwPlantedC4,             "48 8B 15 ? ? ? ? 41 FF C0 48 8D 4C 24 ? 44 89 05 ? ? ? ?",                        3, 7, "dwPlantedC4");
    scanRIP(client_dll::dwWeaponC4,              "48 8B 15 ? ? ? ? 48 8B 5C 24 ? FF C0 89 05 ? ? ? ? 48 8B C6 48 89 34 EA 80 BE",   3, 7, "dwWeaponC4");
    // Pattern lifted from cs2-dumper. The RIP-relative load is the second
    // capture (4C 8B 05 ??) starting 9 bytes into the match — disp32 lives at
    // bytes 12-15, instruction ends at byte 16.
    scanRIP(client_dll::dwGameRules,             "F6 C1 01 0F 85 ? ? ? ? 4C 8B 05 ? ? ? ? 4D 85",                                  12, 16, "dwGameRules");

    // dwLocalPlayerPawn — two-step: resolve the list-entry base RVA then add the pawn field offset.
    {
        total++;
        uint64_t hit = FindSignature(conn,
            "48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 40 53 56 41 54",
            clientBase, clientEnd, pid);
        ptrdiff_t rva = hit ? ResolveRIP(conn, pid, clientBase, hit, 3, 7) : 0;

        uint64_t hit2 = FindSignature(conn, "4C 39 B6 ? ? ? ? 74 ? 44 88 BE", clientBase, clientEnd, pid);
        uint32_t off  = hit2 ? ReadFromPID<uint32_t>(conn, hit2 + 3, pid) : 0;

        if (rva && off)
        {
            client_dll::dwLocalPlayerPawn = rva + static_cast<ptrdiff_t>(off);
            Log::Info("[Updater]: dwLocalPlayerPawn = 0x{:X}", client_dll::dwLocalPlayerPawn);
            resolved++;
        }
        else if (client_dll::dwLocalPlayerPawn)
        {
            Log::Warn("[Updater]: dwLocalPlayerPawn sig failed (rva={} off={}), keeping existing 0x{:X}",
                      rva != 0, off != 0, client_dll::dwLocalPlayerPawn);
        }
        else
        {
            Log::Warn("[Updater]: dwLocalPlayerPawn sig failed (rva={} off={})", rva != 0, off != 0);
        }
    }

    Log::Info("[Updater]: {}/{} RVA pointers resolved.", resolved, total);
    return resolved > 0;
}
