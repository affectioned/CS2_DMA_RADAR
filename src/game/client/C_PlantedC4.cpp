#include "pch.h"
#include "C_PlantedC4.h"
#include "offsets.h"
#include "GameGlobals.h"
#include "../CS2Context.h"

// ── t_BombState — 16 ms ───────────────────────────────────────────────────────
// Dereferences dwPlantedC4 → entity, then reads bomb flags, site, scene node,
// and (if ticking) world position. Latches plantTime on the rising edge of
// m_bBombTicking so timeRemaining() works off a steady clock.

void CS2Context::t_BombState()
{
	ZoneScoped;
	uint64_t plantedC4Ptr = 0;
	int32_t  newRoundEndWinner = m_LastRoundEndWinner;
	uint64_t c4 = 0;

	// Merged pass 1+2: read dwPlantedC4 + roundEndWinner, and speculatively
	// dereference the previous plantedC4Ptr in the same scatter call.
	g_Scatter->Add(g_ClientBase + client_dll::dwPlantedC4, &plantedC4Ptr);
	if (m_GameRulesPtr)
		g_Scatter->Add(m_GameRulesPtr + client_dll::C_CSGameRules::m_iRoundEndWinnerTeam, &newRoundEndWinner);
	bool specDeref = isValidPtr(m_PlantedC4Ptr);
	if (specDeref)
		g_Scatter->Add(m_PlantedC4Ptr, &c4);
	g_Scatter->Execute();
	g_Scatter->Clear();

	if (m_LastRoundEndWinner == 0 && newRoundEndWinner != 0) {
		Log::Info("[Bomb]: Round end (winner team {}), clearing bomb state", newRoundEndWinner);
		m_Local->bomb      = {};
		m_PlantedC4Ptr     = 0;
		m_SuppressedC4Blow = 0.0f;
	}
	m_LastRoundEndWinner = newRoundEndWinner;

	if (!isValidPtr(plantedC4Ptr)) {
		m_Local->bomb      = {};
		m_SuppressedC4Blow = 0.0f;
		return;
	}

	if (plantedC4Ptr != m_PlantedC4Ptr || !specDeref) {
		m_PlantedC4Ptr = plantedC4Ptr;
		c4 = 0;
		g_Scatter->Add(plantedC4Ptr, &c4);
		g_Scatter->Execute();
		g_Scatter->Clear();
	}
	m_PlantedC4Ptr = plantedC4Ptr;

	if (!c4) {
		m_Local->bomb.entity      = 0;
		m_Local->bomb.isTicking   = false;
		m_Local->bomb.plantTimeSet = false;
		return;
	}

	if (c4 != m_Local->bomb.entity) {
		m_Local->bomb.plantTimeSet = false;
		m_Local->bomb.position     = {};
	}
	m_Local->bomb.entity = c4;

	// Merged pass 3+4: read bomb fields and speculatively read position from
	// the previously-known sceneNode in the same scatter call.
	bool    newActivated = false, newTicking  = false;
	bool    newDefusing  = false, newExploded = false, newDefused = false;
	int32_t newSite      = -1;
	float   newC4Blow    = 0.0f;
	Vector3 specPos      = {};
	uint64_t prevSceneNode = m_Local->bomb.sceneNode;

	g_Scatter->Add(c4 + client_dll::C_BaseEntity::m_pGameSceneNode, &m_Local->bomb.sceneNode);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bC4Activated,    &newActivated);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bBombTicking,    &newTicking);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_nBombSite,       &newSite);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bBeingDefused,   &newDefusing);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bHasExploded,    &newExploded);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bBombDefused,    &newDefused);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_flC4Blow,        &newC4Blow);
	bool specPos_ok = isValidPtr(prevSceneNode);
	if (specPos_ok)
		g_Scatter->Add(prevSceneNode + client_dll::CGameSceneNode::m_vecAbsOrigin, &specPos);
	g_Scatter->Execute();
	g_Scatter->Clear();

	if (!newActivated) {
		m_Local->bomb.entity       = 0;
		m_Local->bomb.isTicking    = false;
		m_Local->bomb.plantTimeSet = false;
		return;
	}

	if (m_SuppressedC4Blow != 0.0f && newC4Blow == m_SuppressedC4Blow) {
		m_Local->bomb = {};
		return;
	}
	m_SuppressedC4Blow = 0.0f;

	if (newTicking && newSite != 0 && newSite != 1) return;

	if (newTicking && !m_Local->bomb.isTicking) {
		m_Local->bomb.plantTime    = C_PlantedC4::Clock::now();
		m_Local->bomb.plantTimeSet = true;
		Log::Info("[Bomb]: Planted - site {}", newSite == 0 ? "A" : "B");
	}
	if (!newTicking)
		m_Local->bomb.plantTimeSet = false;

	m_Local->bomb.isTicking      = newTicking;
	m_Local->bomb.isBeingDefused = newDefusing;
	m_Local->bomb.hasExploded    = newExploded;
	m_Local->bomb.hasDefused     = newDefused;
	m_Local->bomb.site           = newSite;
	m_Local->bomb.c4Blow         = newC4Blow;

	if (newTicking && isValidPtr(m_Local->bomb.sceneNode)) {
		if (specPos_ok && m_Local->bomb.sceneNode == prevSceneNode) {
			m_Local->bomb.position = specPos;
		} else {
			g_Scatter->Add(m_Local->bomb.sceneNode + client_dll::CGameSceneNode::m_vecAbsOrigin,
			               &m_Local->bomb.position);
			g_Scatter->Execute();
			g_Scatter->Clear();
		}
	}
}

// ── t_CarrierScan — 100 ms ───────────────────────────────────────────────────
// Three bomb states for an unplanted round: carried / dropped / not spawned.
// The gating flags come from C_CSGameRules (m_bBombPlanted / m_bBombDropped) —
// server-authoritative, network-replicated, stable. Without that gate the old
// code raced the dwWeaponC4 → m_hOwnerEntity chain during carrier→drop
// handoff, which is exactly the flicker the user hit: for a tick or two the
// weapon still has a stale owner handle while the game's higher-level
// "dropped" bit is already set, so the carrier halo flashed on and off.
//
// Flow:
//   planted      → cleared here, t_BombState owns the planted visuals
//   dropped      → read dwWeaponC4 → C_C4 scene node → world position
//   carried      → resolve owner handle → pawn → match against players[]
//   not spawned  → everything cleared

void CS2Context::t_CarrierScan()
{
	ZoneScoped;

	auto clearAll = [&] {
		m_Local->bomb.isCarried   = false;
		m_Local->bomb.carrierSlot = -1;
		m_Local->bomb.isDropped   = false;
	};

	if (m_Local->bomb.entity) {
		clearAll();
		m_CachedC4Wrapper = 0;
		return;
	}
	if (!m_Local->entityList) return;

	// Pass 1 — read the two authoritative GameRules bits in parallel with
	// dwWeaponC4 and (speculatively) the cached wrapper deref. One scatter
	// round-trip either way.
	bool     bombPlanted = false;
	bool     bombDropped = false;
	uint64_t c4Wrapper   = 0;
	uint64_t c4Weapon    = 0;

	if (m_GameRulesPtr) {
		g_Scatter->Add(m_GameRulesPtr + client_dll::C_CSGameRules::m_bBombPlanted, &bombPlanted);
		g_Scatter->Add(m_GameRulesPtr + client_dll::C_CSGameRules::m_bBombDropped, &bombDropped);
	}
	g_Scatter->Add(g_ClientBase + client_dll::dwWeaponC4, &c4Wrapper);
	if (isValidPtr(m_CachedC4Wrapper))
		g_Scatter->Add(m_CachedC4Wrapper, &c4Weapon);
	g_Scatter->Execute();
	g_Scatter->Clear();

	// Planted ownership belongs to t_BombState — bail.
	if (bombPlanted) { clearAll(); return; }

	if (!isValidPtr(c4Wrapper)) { clearAll(); m_CachedC4Wrapper = 0; return; }
	if (c4Wrapper != m_CachedC4Wrapper) {
		m_CachedC4Wrapper = c4Wrapper;
		c4Weapon = 0;
		g_Scatter->Add(c4Wrapper, &c4Weapon);
		g_Scatter->Execute();
		g_Scatter->Clear();
	}
	if (!isValidPtr(c4Weapon)) { clearAll(); return; }
	m_CachedC4Weapon = c4Weapon;

	// Dropped — the bomb is in the world with no owner. Read the C_C4's
	// scene-node origin for the map marker, no carrier.
	if (bombDropped) {
		uint64_t sceneNode = 0;
		g_Scatter->Add(c4Weapon + client_dll::C_BaseEntity::m_pGameSceneNode, &sceneNode);
		g_Scatter->Execute();
		g_Scatter->Clear();

		Vector3 pos{};
		if (isValidPtr(sceneNode)) {
			g_Scatter->Add(sceneNode + client_dll::CGameSceneNode::m_vecAbsOrigin, &pos);
			g_Scatter->Execute();
			g_Scatter->Clear();
		}
		m_Local->bomb.isDropped   = true;
		m_Local->bomb.droppedPos  = pos;
		m_Local->bomb.isCarried   = false;
		m_Local->bomb.carrierSlot = -1;
		return;
	}

	m_Local->bomb.isDropped = false;

	// Carried — read owner handle + speculatively resolve the previously
	// cached handle's chunk + pawn in one scatter call.
	uint32_t ownerHandle = 0;
	uint64_t chunkPtr = 0, ownerPawn = 0;
	g_Scatter->Add(c4Weapon + client_dll::C_BaseEntity::m_hOwnerEntity, &ownerHandle);
	bool specChunk = m_CachedOwnerHandle && m_CachedOwnerHandle != 0xFFFFFFFF;
	if (specChunk) {
		g_Scatter->Add(m_Local->entityList + 0x8 * ((m_CachedOwnerHandle & 0x7FFF) >> 9) + 16, &chunkPtr);
		if (isValidPtr(m_CachedCarrierChunk))
			g_Scatter->Add(m_CachedCarrierChunk + 0x70 * (m_CachedOwnerHandle & 0x1FF), &ownerPawn);
	}
	g_Scatter->Execute();
	g_Scatter->Clear();

	if (!ownerHandle || ownerHandle == 0xFFFFFFFF) {
		m_Local->bomb.isCarried   = false;
		m_Local->bomb.carrierSlot = -1;
		m_CachedOwnerHandle       = 0;
		return;
	}

	if (ownerHandle != m_CachedOwnerHandle || !specChunk) {
		m_CachedOwnerHandle = ownerHandle;
		chunkPtr = 0;
		g_Scatter->Add(m_Local->entityList + 0x8 * ((ownerHandle & 0x7FFF) >> 9) + 16, &chunkPtr);
		g_Scatter->Execute();
		g_Scatter->Clear();
		if (!isValidPtr(chunkPtr)) {
			m_Local->bomb.isCarried   = false;
			m_Local->bomb.carrierSlot = -1;
			return;
		}
		m_CachedCarrierChunk = chunkPtr;

		ownerPawn = 0;
		g_Scatter->Add(chunkPtr + 0x70 * (ownerHandle & 0x1FF), &ownerPawn);
		g_Scatter->Execute();
		g_Scatter->Clear();
	} else {
		m_CachedOwnerHandle  = ownerHandle;
		m_CachedCarrierChunk = chunkPtr;
	}

	if (!isValidPtr(ownerPawn)) {
		m_Local->bomb.isCarried   = false;
		m_Local->bomb.carrierSlot = -1;
		return;
	}

	for (size_t i = 0; i < MAX_ENTITIES; i++) {
		if (m_Local->players[i].pawnBase == ownerPawn) {
			m_Local->bomb.isCarried   = true;
			m_Local->bomb.carrierSlot = (int)i;
			return;
		}
	}
	m_Local->bomb.isCarried   = false;
	m_Local->bomb.carrierSlot = -1;
}
