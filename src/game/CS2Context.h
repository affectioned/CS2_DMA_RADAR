#pragma once
#include "DMA/DMA Thread.h"  // IGameContext + CTimer/Timer alias
#include "sdk.h"

class CS2Context : public IGameContext
{
public:
	CS2Context(CGame& game, std::mutex& gameMutex);

	bool Initialize(DMA_Connection* conn) override;
	bool Tick(DMA_Connection* conn, std::chrono::steady_clock::time_point now) override;

	~CS2Context() override;

private:
	// ── Infrastructure ────────────────────────────────────────────────────────
	Process               m_Process;
	ScatterRead*          m_Scatter = nullptr;
	VMM_HANDLE            m_VMMHandle = nullptr;
	CGame&                m_Game;       // shared with render thread (mutex-guarded)
	std::mutex&           m_Mutex;
	std::unique_ptr<CGame> m_Local;     // private working copy; timers write here
	std::vector<Timer>    m_Timers;     // fired in registration order every tick

	// ── Persistent DMA traversal state ────────────────────────────────────────
	// These are intermediate addresses not visible to the render thread.
	uint64_t m_GlobalVarsPtr    = 0;
	uint64_t m_PlantedC4Ptr     = 0;
	uint64_t m_EntityChunk      = 0;
	uint64_t m_Engine2Base      = 0;   // engine2.dll module base (for build/network reads)
	uint64_t m_NetworkClientPtr = 0;   // refreshed by t_ModulePtrs, consumed by t_NetworkState
	uint64_t m_GameRulesPtr     = 0;   // C_CSGameRules* — refreshed by t_ModulePtrs, used by t_BombState
	int32_t  m_LastRoundEndWinner = 0; // tracks m_iRoundEndWinnerTeam for 0→non-zero transition detection
	// Cached intermediate pointers for speculative scatter merging in t_CarrierScan
	uint64_t m_CachedC4Wrapper    = 0;
	uint64_t m_CachedC4Weapon     = 0;
	uint32_t m_CachedOwnerHandle  = 0;
	uint64_t m_CachedCarrierChunk = 0;
	// Set on respawn to the previous bomb's m_flC4Blow value. t_BombState suppresses
	// any entity whose m_flC4Blow matches until a new (different) plant is detected.
	float    m_SuppressedC4Blow = 0.0f;

	// Incremented by t_ModulePtrs when the local controller pointer changes
	// (i.e. a new map loaded). Each timer that needs to reset on map change
	// tracks its own last-seen generation and reacts independently.
	uint32_t m_MapGeneration  = 0;

	// ── Timer methods ─────────────────────────────────────────────────────────
	// Registered in dependency order — called sequentially via m_Timers.
	void t_ModulePtrs();      // 1000 ms — entityList, localController, globalVars, plantedC4Ptr
	void t_MapName();         // 1000 ms — map name string
	void t_EntityChain();     //  500 ms — 4-pass entity chain resolution
	void t_LocalPlayerPos();  //    8 ms — local player position
	void t_PlayerPositions(); //    8 ms — all player positions, health, angles, defusing
	void t_PlayerCtrl();      //  150 ms — all player controller fields (team, color, armor, …)
	void t_PlayerWeapons();   //  100 ms — active weapon definition IDs
	void t_PlayerNames();     // 5000 ms — player name strings
	void t_CarrierScan();     //  100 ms — C4 carrier detection (pre-plant only)
	void t_BombState();       //   16 ms — bomb flags, site, defuse timer, position
	void t_NetworkState();    //  500 ms — engine2.dll signOnState
};
