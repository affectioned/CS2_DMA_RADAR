#include "pch.h"
#include "C_CSPlayerPawn.h"
#include "offsets.h"
#include "GameGlobals.h"
#include "../CS2Context.h"

void C_CSPlayerPawn::Read(uint64_t base) {
	g_Scatter->Add(base + client_dll::C_BaseEntity::m_iHealth,                   &health);
	g_Scatter->Add(base + client_dll::C_BaseEntity::m_lifeState,                 &lifeState);
	g_Scatter->Add(base + client_dll::C_CSPlayerPawn::m_angEyeAngles,            &eyeAngles);
	g_Scatter->Add(base + client_dll::C_BasePlayerPawn::m_vOldOrigin,            &position);
	g_Scatter->Add(base + client_dll::C_CSPlayerPawn::m_bIsDefusing,             &isDefusing);
	g_Scatter->AddRaw(base + client_dll::C_CSPlayerPawn::m_szLastPlaceName,      sizeof(lastPlaceName), lastPlaceName);
	g_Scatter->Add(base + client_dll::C_BasePlayerPawn::m_pWeaponServices,       &weaponServicesPtr);
}

// ── t_LocalPlayerPos — 8 ms ───────────────────────────────────────────────────
// Reads the local player's world position and view angles for low-latency
// self-tracking. eyeAngles is required by the rotated-radar code path — it
// does NOT come from t_PlayerPositions, which only writes into players[].

void CS2Context::t_LocalPlayerPos()
{
	ZoneScoped;
	if (!m_Local->localPlayer.pawnBase) return;

	uint8_t  curLifeState  = 255;
	uint64_t observerSvcs  = 0;
	g_Scatter->Add(m_Local->localPlayer.pawnBase + client_dll::C_BasePlayerPawn::m_vOldOrigin,
	               &m_Local->localPlayer.pawn.position);
	g_Scatter->Add(m_Local->localPlayer.pawnBase + client_dll::C_CSPlayerPawn::m_angEyeAngles,
	               &m_Local->localPlayer.pawn.eyeAngles);
	g_Scatter->Add(m_Local->localPlayer.pawnBase + client_dll::C_BaseEntity::m_lifeState,
	               &curLifeState);
	g_Scatter->Add(m_Local->localPlayer.pawnBase + client_dll::C_BasePlayerPawn::m_pObserverServices,
	               &observerSvcs);
	g_Scatter->Execute();
	g_Scatter->Clear();

	// Spectate-follow: when the local player is dead, our own pawn position
	// freezes at the corpse, which strands the radar. If we're spectating
	// someone, resolve their pawn via m_hObserverTarget (CHandle → entity-list
	// chunk → pawn ptr) and copy their already-tracked position/eyeAngles from
	// players[] over our own. Three extra scatter executes are unavoidable here
	// because each step depends on the previous read; only paid while dead.
	if (curLifeState != 0 && isValidPtr(observerSvcs) && m_Local->entityList) {
		uint32_t targetHandle = 0;
		g_Scatter->Add(observerSvcs + client_dll::CPlayer_ObserverServices::m_hObserverTarget,
		               &targetHandle);
		g_Scatter->Execute();
		g_Scatter->Clear();

		if (targetHandle && targetHandle != 0xFFFFFFFF) {
			uint64_t listEntry = 0;
			g_Scatter->Add(m_Local->entityList + 0x8 * ((targetHandle & 0x7FFF) >> 9) + 16,
			               &listEntry);
			g_Scatter->Execute();
			g_Scatter->Clear();

			uint64_t targetPawn = 0;
			if (isValidPtr(listEntry)) {
				g_Scatter->Add(listEntry + 0x70 * (targetHandle & 0x1FF), &targetPawn);
				g_Scatter->Execute();
				g_Scatter->Clear();
			}

			if (isValidPtr(targetPawn)) {
				for (size_t i = 0; i < MAX_ENTITIES; i++) {
					if (m_Local->players[i].pawnBase == targetPawn) {
						m_Local->localPlayer.pawn.position  = m_Local->players[i].pawn.position;
						m_Local->localPlayer.pawn.eyeAngles = m_Local->players[i].pawn.eyeAngles;
						break;
					}
				}
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
			m_Local->players[i].pawn.Read(m_Local->players[i].pawnBase);
	g_Scatter->Execute();
	g_Scatter->Clear();

	for (int i = 0; i < MAX_ENTITIES; i++)
		m_Local->players[i].pawn.lastPlaceName[sizeof(m_Local->players[i].pawn.lastPlaceName) - 1] = '\0';
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
