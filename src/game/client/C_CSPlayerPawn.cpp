#include "pch.h"
#include "C_CSPlayerPawn.h"
#include "offsets.h"
#include "GameGlobals.h"
#include "../CS2Context.h"

void C_CSPlayerPawn::Read(uint64_t base) {
	ReadFast(base);
	ReadStatus(base);
}

void C_CSPlayerPawn::ReadFast(uint64_t base) {
	g_Scatter->Add(base + client_dll::C_BasePlayerPawn::m_vOldOrigin,            &position);
	g_Scatter->Add(base + client_dll::C_CSPlayerPawn::m_angEyeAngles,            &eyeAngles);
}

void C_CSPlayerPawn::ReadStatus(uint64_t base) {
	g_Scatter->Add(base + client_dll::C_BaseEntity::m_iHealth,                   &health);
	g_Scatter->Add(base + client_dll::C_BaseEntity::m_lifeState,                 &lifeState);
	g_Scatter->Add(base + client_dll::C_CSPlayerPawn::m_bIsDefusing,             &isDefusing);
	g_Scatter->AddRaw(base + client_dll::C_CSPlayerPawn::m_szLastPlaceName,      sizeof(lastPlaceName), lastPlaceName);
	g_Scatter->Add(base + client_dll::C_BasePlayerPawn::m_pWeaponServices,       &weaponServicesPtr);
}

// CHandle → entity pointer. Two dependent reads, so two scatter executes.
static uint64_t resolveHandle(uint64_t entityList, uint32_t handle)
{
	if (!entityList || !handle || handle == 0xFFFFFFFF) return 0;

	uint64_t listEntry = 0;
	g_Scatter->Add(entityList + 0x8 * ((handle & 0x7FFF) >> 9) + 16, &listEntry);
	g_Scatter->Execute();
	g_Scatter->Clear();
	if (!isValidPtr(listEntry)) return 0;

	uint64_t ent = 0;
	g_Scatter->Add(listEntry + 0x70 * (handle & 0x1FF), &ent);
	g_Scatter->Execute();
	g_Scatter->Clear();
	return isValidPtr(ent) ? ent : 0;
}

// ── t_LocalPlayerPos — 8 ms ───────────────────────────────────────────────────
// Reads the local player's world position and view angles for low-latency
// self-tracking. eyeAngles is required by the rotated-radar code path — it
// does NOT come from t_PlayerPositions, which only writes into players[].

void CS2Context::t_LocalPlayerPos()
{
	ZoneScoped;
	if (!m_Local->localPlayer.pawnBase) return;

	// Spectate-follow chain state. While dead, our own pawn position freezes at
	// the corpse and would strand the radar. CS2 gives the controller a separate
	// C_CSObserverPawn (m_hObserverPawn), and it is *that* pawn's observer
	// services that carry the spectate target — the corpse's m_pObserverServices
	// never gets one, which is why reading it off the player pawn never worked.
	//
	// Both hops (handle → observer pawn, pawn → services) are cached, so the
	// steady-state cost is three extra reads folded into the batch below and no
	// extra scatter executes. After a handle changes, one hop re-resolves per
	// tick, so lock-on takes ~3 ticks (~24 ms).
	static uint32_t s_obsPawnHandle = 0;
	static uint64_t s_obsPawnPtr    = 0;
	static uint64_t s_obsSvcsPtr    = 0;

	// Pointers the batch below reads through; the post-batch logic needs to know
	// which ones were live at batch time to decide what the results belong to.
	const uint64_t batchObsPawnPtr = s_obsPawnPtr;
	const uint64_t batchObsSvcsPtr = s_obsSvcsPtr;

	uint8_t  curLifeState    = 255;
	uint32_t obsPawnHandle   = 0;
	uint64_t obsSvcsPtr      = 0;
	uint32_t obsTargetHandle = 0;

	g_Scatter->Add(m_Local->localPlayer.pawnBase + client_dll::C_BasePlayerPawn::m_vOldOrigin,
	               &m_Local->localPlayer.pawn.position);
	g_Scatter->Add(m_Local->localPlayer.pawnBase + client_dll::C_CSPlayerPawn::m_angEyeAngles,
	               &m_Local->localPlayer.pawn.eyeAngles);
	g_Scatter->Add(m_Local->localPlayer.pawnBase + client_dll::C_BaseEntity::m_lifeState,
	               &curLifeState);
	if (m_Local->localPlayer.controllerBase)
		g_Scatter->Add(m_Local->localPlayer.controllerBase + client_dll::CCSPlayerController::m_hObserverPawn,
		               &obsPawnHandle);
	if (batchObsPawnPtr)
		g_Scatter->Add(batchObsPawnPtr + client_dll::C_BasePlayerPawn::m_pObserverServices,
		               &obsSvcsPtr);
	if (batchObsSvcsPtr)
		g_Scatter->Add(batchObsSvcsPtr + client_dll::CPlayer_ObserverServices::m_hObserverTarget,
		               &obsTargetHandle);
	g_Scatter->Execute();
	g_Scatter->Clear();

	if (curLifeState == 0) {
		// Alive: drop the chain so a stale observer pawn from the previous death
		// (or from the previous map) can never be read through.
		s_obsPawnHandle = 0;
		s_obsPawnPtr    = 0;
		s_obsSvcsPtr    = 0;
	}
	else {
		// Resolve the observer pawn pointer when the handle changes, and retry
		// on a throttle if that resolve came back empty (torn read, or the pawn
		// isn't networked yet) instead of staying stranded for the whole death.
		static int s_retryIn = 0;
		if (s_retryIn > 0) s_retryIn--;
		const bool resolveFailed = !s_obsPawnPtr && obsPawnHandle &&
		                           obsPawnHandle != 0xFFFFFFFF && s_retryIn == 0;

		if (obsPawnHandle != s_obsPawnHandle || resolveFailed) {
			s_obsPawnHandle = obsPawnHandle;
			s_obsPawnPtr    = resolveHandle(m_Local->entityList, obsPawnHandle);
			s_obsSvcsPtr    = 0;
			s_retryIn       = 32;   // ~256 ms at the 8 ms tick rate
		}
		else if (batchObsPawnPtr) {
			s_obsSvcsPtr = isValidPtr(obsSvcsPtr) ? obsSvcsPtr : 0;
		}

		// Trust obsTargetHandle only if the services pointer it came from is
		// still the current one. Match by entity-list slot against the pawn
		// handles t_EntityChain already resolved — no extra reads needed — and
		// copy the spectated player's tracked position/angles over our own.
		if (batchObsSvcsPtr && batchObsSvcsPtr == s_obsSvcsPtr &&
		    obsTargetHandle && obsTargetHandle != 0xFFFFFFFF)
		{
			const uint32_t slot = obsTargetHandle & 0x7FFF;
			for (size_t i = 0; i < MAX_ENTITIES; i++) {
				if (!m_Local->players[i].pawnBase) continue;
				if ((m_Local->players[i].pawnHandle & 0x7FFF) != slot) continue;
				m_Local->localPlayer.pawn.position  = m_Local->players[i].pawn.position;
				m_Local->localPlayer.pawn.eyeAngles = m_Local->players[i].pawn.eyeAngles;
				break;
			}
		}
	}

	// Dead → alive transition = respawn = new round (fallback path; the
	// primary round-end signal is m_iRoundEndWinnerTeam handled in t_BombState).
	// Kept because some round-end paths leave m_bC4Activated/m_bBombTicking
	// true on the stale entity, and CS2 reuses the same entity address each
	// round, so we suppress by m_flC4Blow (unique per plant) rather than ptr.
	static uint8_t prevLifeState = 0;
	if (prevLifeState != 0 && curLifeState == 0) {
		m_SuppressedC4Blow = m_Local->bomb.c4Blow;
		m_Local->bomb      = {};
		m_PlantedC4Ptr     = 0;
	}
	prevLifeState = curLifeState;
}

// ── t_PlayerPositions — 8 ms ──────────────────────────────────────────────────
// Reads pawn fields (health, lifeState, position, angles, defusing, etc.)
// for every valid player slot.

void CS2Context::t_PlayerPositions()
{
	ZoneScoped;
	for (int i = 0; i < MAX_ENTITIES; i++)
		if (m_Local->players[i].pawnBase)
			m_Local->players[i].pawn.ReadFast(m_Local->players[i].pawnBase);
	g_Scatter->Execute();
	g_Scatter->Clear();
}

// ── t_PlayerWeapons — 100 ms ──────────────────────────────────────────────────
// Reads active weapon definition IDs (item definition index) for all players.

void CS2Context::t_PlayerWeapons()
{
	ZoneScoped;
	if (!m_Local->entityList) return;

	// Pass 1: weapon services → active weapon handle
	static uint32_t handles[MAX_ENTITIES];
	memset(handles, 0, sizeof(handles));
	for (int i = 0; i < MAX_ENTITIES; i++) {
		if (!m_Local->players[i].pawnBase || !m_Local->players[i].pawn.weaponServicesPtr) continue;
		g_Scatter->Add(m_Local->players[i].pawn.weaponServicesPtr + client_dll::CPlayer_WeaponServices::m_hActiveWeapon,
		               &handles[i]);
	}
	g_Scatter->Execute();
	g_Scatter->Clear();

	// Pass 2: handle → entity list chunk
	static uint64_t listEntries[MAX_ENTITIES];
	memset(listEntries, 0, sizeof(listEntries));
	for (int i = 0; i < MAX_ENTITIES; i++) {
		uint32_t h = handles[i];
		if (!h || h == 0xFFFFFFFF) continue;
		g_Scatter->Add(m_Local->entityList + 0x8 * ((h & 0x7FFF) >> 9) + 16, &listEntries[i]);
	}
	g_Scatter->Execute();
	g_Scatter->Clear();

	// Pass 3: list chunk + slot → weapon entity pointer
	static uint64_t weaponPtrs[MAX_ENTITIES];
	memset(weaponPtrs, 0, sizeof(weaponPtrs));
	for (int i = 0; i < MAX_ENTITIES; i++) {
		uint32_t h = handles[i];
		if (!h || h == 0xFFFFFFFF || !listEntries[i]) continue;
		g_Scatter->Add(listEntries[i] + 0x70 * (h & 0x1FF), &weaponPtrs[i]);
	}
	g_Scatter->Execute();
	g_Scatter->Clear();

	// Pass 4: weapon entity → item definition index
	for (int i = 0; i < MAX_ENTITIES; i++) {
		if (!weaponPtrs[i]) continue;
		uint64_t itemAddr = weaponPtrs[i]
			+ client_dll::C_EconEntity::m_AttributeManager
			+ client_dll::C_AttributeContainer::m_Item
			+ client_dll::C_EconItemView::m_iItemDefinitionIndex;
		g_Scatter->Add(itemAddr, &m_Local->players[i].pawn.activeWeaponID);
	}
	g_Scatter->Execute();
	g_Scatter->Clear();
}
