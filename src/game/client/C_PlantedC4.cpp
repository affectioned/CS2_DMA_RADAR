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
	// Re-read dwPlantedC4 fresh — never cache this pointer across map transitions.
	// Also piggy-back the round-end-winner read so we can detect "round just
	// ended" without a separate scatter pass — the transition 0 → non-zero
	// fires the moment CS2 declares a winner, even on T-elimination where
	// m_bC4Activated and m_bBombTicking stay true on the stale entity.
	uint64_t plantedC4Ptr = 0;
	int32_t  newRoundEndWinner = m_LastRoundEndWinner;
	g_Scatter->Add(g_ClientBase + client_dll::dwPlantedC4, &plantedC4Ptr);
	if (m_GameRulesPtr)
		g_Scatter->Add(m_GameRulesPtr + client_dll::C_CSGameRules::m_iRoundEndWinnerTeam, &newRoundEndWinner);
	g_Scatter->Execute();
	g_Scatter->Clear();

	// Round-end transition: clear bomb state immediately so the panel doesn't
	// keep showing a counting-down timer (T-elim) or a stale "DEFUSED"/
	// "EXPLODED" banner across rounds. Latch the new winner so we don't loop.
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
	m_PlantedC4Ptr = plantedC4Ptr;

	uint64_t c4 = 0;
	g_Scatter->Add(plantedC4Ptr, &c4);
	g_Scatter->Execute();
	g_Scatter->Clear();

	if (!c4) {
		m_Local->bomb.entity      = 0;
		m_Local->bomb.isTicking   = false;
		m_Local->bomb.plantTimeSet = false;
		return;
	}

	// Reset plant timer and position if the entity pointer changed (new round)
	if (c4 != m_Local->bomb.entity) {
		m_Local->bomb.plantTimeSet = false;
		m_Local->bomb.position     = {};
	}
	m_Local->bomb.entity = c4;

	bool    newActivated = false, newTicking  = false;
	bool    newDefusing  = false, newExploded = false, newDefused = false;
	int32_t newSite      = -1;
	float   newC4Blow    = 0.0f;

	g_Scatter->Add(c4 + client_dll::C_BaseEntity::m_pGameSceneNode, &m_Local->bomb.sceneNode);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bC4Activated,    &newActivated);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bBombTicking,    &newTicking);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_nBombSite,       &newSite);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bBeingDefused,   &newDefusing);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bHasExploded,    &newExploded);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_bBombDefused,    &newDefused);
	g_Scatter->Add(c4 + client_dll::C_PlantedC4::m_flC4Blow,        &newC4Blow);
	g_Scatter->Execute();
	g_Scatter->Clear();

	// Stale entity from previous round — clear entity so t_CarrierScan isn't suppressed
	if (!newActivated) {
		m_Local->bomb.entity       = 0;
		m_Local->bomb.isTicking    = false;
		m_Local->bomb.plantTimeSet = false;
		return;
	}

	// Suppressed after a respawn: ignore this entity until its m_flC4Blow changes,
	// which only happens when a new bomb is planted (each plant gets a unique blow time).
	// We suppress by blow-time rather than pointer because CS2 reuses the entity address.
	if (m_SuppressedC4Blow != 0.0f && newC4Blow == m_SuppressedC4Blow) {
		m_Local->bomb = {};
		return;
	}
	m_SuppressedC4Blow = 0.0f;

	// Reject corrupt reads: site must be 0 (A) or 1 (B) once ticking
	if (newTicking && newSite != 0 && newSite != 1) return;

	// Latch plant time on rising edge
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
		g_Scatter->Add(m_Local->bomb.sceneNode + client_dll::CGameSceneNode::m_vecAbsOrigin,
		               &m_Local->bomb.position);
		g_Scatter->Execute();
		g_Scatter->Clear();
	}
}

// ── t_CarrierScan — 100 ms ───────────────────────────────────────────────────
// Reads the global C4-weapon entity (dwWeaponC4 → wrapper → real C_C4) and
// follows its m_hOwnerEntity CHandle through the entity-list chunk → slot to
// find who is holding the bomb, then matches that pawn pointer against
// players[]. Skipped once the bomb is planted; carrier state stays cleared
// from then on (no planter tracking).

void CS2Context::t_CarrierScan()
{
	ZoneScoped;
	// Once planted, no carrier to find — the planted-bomb dot on the radar
	// is the only post-plant cue; we deliberately do not keep tagging the
	// planter on either the radar or the team panel.
	if (m_Local->bomb.entity) {
		m_Local->bomb.isCarried   = false;
		m_Local->bomb.carrierSlot = -1;
		return;
	}
	if (!m_Local->entityList) return;

	auto clearCarrier = [&] {
		m_Local->bomb.isCarried   = false;
		m_Local->bomb.carrierSlot = -1;
	};

	// dwWeaponC4 resolves to a small wrapper (likely a "current/last C4" cache
	// or list head) whose first 8 bytes hold the pointer to the actual C_C4
	// entity. Confirmed empirically by hex-dumping both: the wrapper has its
	// vtable at +0x10 (not +0x0) and 8-byte pointer-shaped data where you'd
	// expect a 4-byte CHandle at +0x520; the deref'd address has a real
	// vtable at +0x0 and a sensible CHandle at +0x520.
	uint64_t c4Wrapper = 0;
	g_Scatter->Add(g_ClientBase + client_dll::dwWeaponC4, &c4Wrapper);
	g_Scatter->Execute();
	g_Scatter->Clear();
	if (!isValidPtr(c4Wrapper)) { clearCarrier(); return; }

	uint64_t c4Weapon = 0;
	g_Scatter->Add(c4Wrapper, &c4Weapon);
	g_Scatter->Execute();
	g_Scatter->Clear();
	if (!isValidPtr(c4Weapon)) { clearCarrier(); return; }

	uint32_t ownerHandle = 0;
	g_Scatter->Add(c4Weapon + client_dll::C_BaseEntity::m_hOwnerEntity, &ownerHandle);
	g_Scatter->Execute();
	g_Scatter->Clear();
	if (!ownerHandle || ownerHandle == 0xFFFFFFFF) { clearCarrier(); return; }

	uint64_t chunkPtr = 0;
	g_Scatter->Add(m_Local->entityList + 0x8 * ((ownerHandle & 0x7FFF) >> 9) + 16, &chunkPtr);
	g_Scatter->Execute();
	g_Scatter->Clear();
	if (!isValidPtr(chunkPtr)) { clearCarrier(); return; }

	uint64_t ownerPawn = 0;
	g_Scatter->Add(chunkPtr + 0x70 * (ownerHandle & 0x1FF), &ownerPawn);
	g_Scatter->Execute();
	g_Scatter->Clear();
	if (!isValidPtr(ownerPawn)) { clearCarrier(); return; }

	// Match owner pawn against known player slots.
	for (size_t i = 0; i < MAX_ENTITIES; i++) {
		if (m_Local->players[i].pawnBase == ownerPawn) {
			m_Local->bomb.isCarried   = true;
			m_Local->bomb.carrierSlot = (int)i;
			return;
		}
	}
	clearCarrier();
}
