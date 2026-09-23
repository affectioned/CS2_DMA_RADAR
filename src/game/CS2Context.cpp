#include "pch.h"
#include "CS2Context.h"
#include "GameModules.h"
#include "updater.h"

using namespace std::chrono_literals;

// ── Globals (set in Initialize, used by entity Read() methods) ────────────────
ScatterRead*      g_Scatter    = nullptr;
uintptr_t         g_ClientBase = 0;
std::atomic<bool> g_Connected{ false };

// ── CS2Context lifecycle ───────────────────────────────────────────────────────

CS2Context::CS2Context(CGame& game, std::mutex& gameMutex)
	: m_Game(game), m_Mutex(gameMutex) {}

CS2Context::~CS2Context()
{
	g_Connected.store(false);
	g_Scatter    = nullptr;
	g_ClientBase = 0;
	delete m_Scatter;
}

bool CS2Context::Initialize(DMA_Connection* conn)
{
	if (!m_Process.GetProcessInfo(GameModules::ProcessName, GameModules::ModuleList(), conn))
		return false;

	g_ClientBase = m_Process.GetModuleBase(GameModules::ClientDll);
	if (!g_ClientBase) {
		Log::Error("[CS2Context]: {} base not found", GameModules::ClientDll);
		return false;
	}
	Log::Info("[CS2Context]: {} at 0x{:X}", GameModules::ClientDll, g_ClientBase);

	m_Engine2Base = m_Process.GetModuleBase(GameModules::Engine2Dll);
	if (m_Engine2Base)
		Log::Info("[CS2Context]: {} at 0x{:X}", GameModules::Engine2Dll, m_Engine2Base);
	else
		Log::Warn("[CS2Context]: {} base not found - signOnState/build number unavailable",
		          GameModules::Engine2Dll);

	m_Scatter = new ScatterRead(conn->GetHandle(), m_Process.GetPID());
	g_Scatter  = m_Scatter;
	m_Local    = std::make_unique<CGame>();

	updater::resolveOffsets(conn, &m_Process);

	// Build-number log lets us cross-check the cs2-dumper snapshot when offsets drift.
	if (m_Engine2Base) {
		uint32_t buildNumber = 0;
		g_Scatter->Add(m_Engine2Base + engine2_dll::dwBuildNumber, &buildNumber);
		g_Scatter->Execute();
		g_Scatter->Clear();
		if (buildNumber)
			Log::Info("[CS2Context]: CS2 build number: {}", buildNumber);
	}

	// Register timers in dependency order.
	// CTimer::m_LastExecutionTime defaults to epoch, so every timer fires on
	// the very first tick — all state is populated before the render thread
	// takes its first snapshot.
	m_Timers.emplace_back(1000ms, [this]{ t_ModulePtrs();      });  // seeds entity list + ptrs
	m_Timers.emplace_back( 100ms, [this]{ t_MapName();          });  // depends on m_GlobalVarsPtr
	m_Timers.emplace_back( 100ms, [this]{ t_EntityChain();      });  // depends on entityList
	m_Timers.emplace_back(   8ms, [this]{ t_LocalPlayerPos();   });  // depends on localPlayer.pawnBase
	m_Timers.emplace_back(   8ms, [this]{ t_PlayerPositions();  });  // depends on pawnBase
	m_Timers.emplace_back(  50ms, [this]{ t_PlayerCtrl();       });  // depends on controllerBase
	m_Timers.emplace_back( 100ms, [this]{ t_PlayerWeapons();    });  // depends on activeWeapon
	m_Timers.emplace_back(5000ms, [this]{ t_PlayerNames();      });  // depends on ctrl.nameAddr
	m_Timers.emplace_back( 100ms, [this]{ t_CarrierScan();      });  // depends on dwWeaponC4 + entity list
	m_Timers.emplace_back(  16ms, [this]{ t_BombState();        });  // depends on m_PlantedC4Ptr
	m_Timers.emplace_back( 500ms, [this]{ t_NetworkState();     });  // depends on m_Engine2Base

	g_Connected.store(true, std::memory_order_release);
	Log::Info("[CS2Context]: Initialized - {} timers registered", m_Timers.size());
	return true;
}

void CS2Context::Tick(DMA_Connection* /*conn*/,
                      std::chrono::steady_clock::time_point now)
{
	for (auto& t : m_Timers)
		t.Tick(now);

	std::lock_guard<std::mutex> lock(m_Mutex);
	m_Game = *m_Local;
}

// ── t_ModulePtrs — 1000 ms ────────────────────────────────────────────────────
// Reads module-level global pointers from client.dll.

void CS2Context::t_ModulePtrs()
{
	uint64_t base = g_ClientBase;
	uint64_t newEntityList = 0, newController = 0, newPawn = 0,
	         newGlobalVars = 0, newC4Ptr = 0, newNetworkClient = 0,
	         newGameRulesProxy = 0;

	g_Scatter->Add(base + client_dll::dwGlobalVars,            &newGlobalVars);
	g_Scatter->Add(base + client_dll::dwLocalPlayerController, &newController);
	g_Scatter->Add(base + client_dll::dwLocalPlayerPawn,       &newPawn);
	g_Scatter->Add(base + client_dll::dwEntityList,            &newEntityList);
	g_Scatter->Add(base + client_dll::dwPlantedC4,             &newC4Ptr);
	g_Scatter->Add(base + client_dll::dwGameRules,             &newGameRulesProxy);
	if (m_Engine2Base)
		g_Scatter->Add(m_Engine2Base + engine2_dll::dwNetworkGameClient, &newNetworkClient);
	g_Scatter->Execute();
	g_Scatter->Clear();

	// dwGameRules → C_CSGameRulesProxy* → m_pGameRules → C_CSGameRules*.
	// Resolve once a second; t_BombState reads the rules counter directly off
	// m_GameRulesPtr without needing the proxy hop on every 16ms tick.
	if (isValidPtr(newGameRulesProxy)) {
		uint64_t newRules = 0;
		g_Scatter->Add(newGameRulesProxy + client_dll::C_CSGameRulesProxy::m_pGameRules, &newRules);
		g_Scatter->Execute();
		g_Scatter->Clear();
		if (isValidPtr(newRules)) m_GameRulesPtr = newRules;
	}

	if (isValidPtr(newNetworkClient)) m_NetworkClientPtr = newNetworkClient;

	if (isValidPtr(newGlobalVars)) m_GlobalVarsPtr = newGlobalVars;
	if (isValidPtr(newC4Ptr))      m_PlantedC4Ptr  = newC4Ptr;

	static uint64_t prevEL = 0, prevCtrl = 0;

	if (isValidPtr(newEntityList)) {
		if (newEntityList != prevEL) {
			Log::Info("[SDK]: Entity list -> 0x{:X}", newEntityList);
			prevEL = newEntityList;
		}
		m_Local->entityList = newEntityList;
	}
	if (isValidPtr(newController)) {
		if (newController != prevCtrl) {
			Log::Info("[SDK]: Local controller -> 0x{:X} (map gen {})", newController, m_MapGeneration + 1);
			prevCtrl = newController;
			m_MapGeneration++;
		}
		m_Local->localPlayer.controllerBase = newController;
	}
	if (isValidPtr(newPawn))
		m_Local->localPlayer.pawnBase = newPawn;
}

// ── t_MapName — 100 ms ────────────────────────────────────────────────────────
// Follows globalVars+activeOff → mapPtr → map name string.
// On each gen change, resets to the canonical offset 0x188 and forces an
// immediate scan pass. If 0x188 fails, scans gv+[0x140..0x280] in 8-byte
// steps every 5 s until a valid pointer to a printable map-prefixed string
// is found, then sticks to that offset. Logs the found offset so it can be
// hardcoded if it proves stable.

void CS2Context::t_MapName()
{
	using clk = std::chrono::steady_clock;
	static uint32_t      lastGen   = UINT32_MAX;
	static clk::time_point lastWait{};
	static int           activeOff = 0x188;

	if (m_MapGeneration != lastGen) {
		Log::Info("[Map]: Generation {} - clearing map name", m_MapGeneration);
		memset(m_Local->mapName, 0, sizeof(m_Local->mapName));
		lastGen   = m_MapGeneration;
		lastWait  = {};     // force immediate scan on first failed tick
		activeOff = 0x188;
	}

	uint64_t globalVars = 0;
	g_Scatter->Add(g_ClientBase + client_dll::dwGlobalVars, &globalVars);
	g_Scatter->Execute();
	g_Scatter->Clear();

	static uint64_t lastGV = 0;
	if (globalVars != lastGV) {
		Log::Info("[Map]: globalVars 0x{:X}", globalVars);
		lastGV = globalVars;
	}

	if (!isValidPtr(globalVars))
		return;

	// Try reading map name via a char* pointer at gv+off.
	// Returns true if a valid, map-prefixed string was found (and updates mapName).
	static const char* kPrefixes[] = { "de_", "cs_", "ar_", "gg_", "dm_", "dz_" };
	auto tryPtr = [&](int off) -> bool {
		uint64_t ptr = 0;
		g_Scatter->Add(globalVars + off, &ptr);
		g_Scatter->Execute();
		g_Scatter->Clear();
		if (!isValidPtr(ptr)) return false;

		char buf[32] = {};
		g_Scatter->AddRaw(ptr, sizeof(buf) - 1, buf);
		g_Scatter->Execute();
		g_Scatter->Clear();
		buf[sizeof(buf) - 1] = '\0';

		if (!buf[0]) return false;
		for (size_t i = 0; buf[i]; i++) {
			unsigned char c = (unsigned char)buf[i];
			if (c < 0x20 || c > 0x7E) return false;
		}
		bool hasPrefix = false;
		for (auto p : kPrefixes) if (strncmp(buf, p, 3) == 0) { hasPrefix = true; break; }
		if (!hasPrefix) return false;

		if (strncmp(m_Local->mapName, buf, sizeof(m_Local->mapName)) != 0) {
			if (off != 0x188)
				Log::Info("[Map]: found at gv+0x{:X}: {}", off, buf);
			else
				Log::Info("[Map]: -> {}", buf);
			memcpy(m_Local->mapName, buf, sizeof(m_Local->mapName));
		}
		return true;
	};

	if (tryPtr(activeOff)) return;

	auto now = clk::now();
	if (now - lastWait < std::chrono::seconds(5)) return;
	lastWait = now;

	// Log what the failing offset holds, then scan the neighbourhood in case the
	// offset shifted in a CS2 update. The scan runs every 5 s until the map loads.
	uint64_t badPtr = 0;
	g_Scatter->Add(globalVars + activeOff, &badPtr);
	g_Scatter->Execute();
	g_Scatter->Clear();
	Log::Info("[Map]: gen {} gv+0x{:X}=0x{:X} - scanning",
	          m_MapGeneration, activeOff, badPtr);

	for (int off = 0x140; off <= 0x280; off += 8) {
		if (off == activeOff) continue;
		if (tryPtr(off)) { activeOff = off; return; }
	}
}

// 6 = SIGNONSTATE_FULL (in-game). The CNetworkGameClient pointer is refreshed
// by t_ModulePtrs, so only the 1-byte state field is read here.
void CS2Context::t_NetworkState()
{
	if (!m_NetworkClientPtr) {
		if (m_Local->signOnState != 0) {
			Log::Info("[Network]: signOnState {} -> 0 (client null)", m_Local->signOnState);
			m_Local->signOnState = 0;
		}
		return;
	}

	uint8_t newState = 0;
	g_Scatter->Add(m_NetworkClientPtr + engine2_dll::dwNetworkGameClient_signOnState, &newState);
	g_Scatter->Execute();
	g_Scatter->Clear();

	if (newState != m_Local->signOnState) {
		Log::Info("[Network]: signOnState {} -> {}", m_Local->signOnState, newState);
		m_Local->signOnState = newState;
	}
}
