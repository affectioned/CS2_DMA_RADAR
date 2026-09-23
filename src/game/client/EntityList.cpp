#include "pch.h"
#include "../CS2Context.h"

// ── t_EntityChain — 100 ms ────────────────────────────────────────────────────
// 4-pass entity chain resolution: chunk ptr → controllerBase → pawnHandle
// → listEntry2 → pawnBase. Owns its own map-change detection: when the chunk 0
// pointer changes (new map load), all player state is wiped here. Chain pointer
// fields are always reset so that ctrl/pawn data written by slower timers persists
// between chain runs.
//
// Pass 1 and Pass 4 read a 20-byte header from each entity identity to validate
// the slot via handle: identity.handle & 0x7FFF must equal the expected slot index.
// This matches the CS2 entity identity layout:
//   +0x00: entity pointer (uint64)
//   +0x08: padding       (uint64)
//   +0x10: handle        (uint32, lower 15 bits = slot index)

namespace {
	struct IdentityHead {
		uint64_t entityPtr;  // +0x00
		uint64_t _pad;       // +0x08
		uint32_t handle;     // +0x10
		uint32_t _pad2;      // +0x14 (padding to align, not read from game memory)
	};
	static_assert(offsetof(IdentityHead, handle) == 0x10);
}

void CS2Context::t_EntityChain()
{
	ZoneScoped;
	// Merged: read dwEntityList and speculatively read chunk 0 from the
	// previously-known entityList address in one scatter call.
	uint64_t entityList = 0;
	uint64_t newChunk = 0;
	g_Scatter->Add(g_ClientBase + client_dll::dwEntityList, &entityList);
	if (m_Local->entityList)
		g_Scatter->Add(m_Local->entityList + 16, &newChunk);
	g_Scatter->Execute();
	g_Scatter->Clear();

	if (!entityList || entityList < 0x10000 || entityList > 0x7FFFFFFFFFFF) return;

	if (entityList != m_Local->entityList || !newChunk) {
		m_Local->entityList = entityList;
		newChunk = 0;
		g_Scatter->Add(entityList + 16, &newChunk);
		g_Scatter->Execute();
		g_Scatter->Clear();
	}
	m_Local->entityList = entityList;

	if (!newChunk || newChunk < 0x10000 || newChunk > 0x7FFFFFFFFFFF) return;

	// Wipe all player state when either the chunk pointer or the map generation
	// changes. The chunk pointer may or may not change between maps (it's a heap
	// allocation that CS2 might reuse), so the generation counter from
	// t_ModulePtrs (incremented on local controller change) is the reliable signal.
	static uint32_t lastGen = UINT32_MAX;
	bool mapChanged = (newChunk != m_EntityChunk) || (m_MapGeneration != lastGen);
	if (mapChanged) {
		Log::Info("[SDK]: Entity reset - chunk 0x{:X} gen {}", newChunk, m_MapGeneration);
		for (int i = 0; i < MAX_ENTITIES; i++)
			m_Local->players[i] = {};
		lastGen = m_MapGeneration;
	}
	m_EntityChunk = newChunk;

	// Snapshot old controller pointers before wiping chain fields so we can detect
	// slot reuse below (different controller in same slot = stale ctrl/pawn data).
	static uint64_t prevControllers[MAX_ENTITIES] = {};
	for (int i = 0; i < MAX_ENTITIES; i++)
		prevControllers[i] = m_Local->players[i].controllerBase;

	// Reset only chain pointer fields — preserve ctrl/pawn data for slower timers
	for (int i = 0; i < MAX_ENTITIES; i++) {
		m_Local->players[i].listEntry      = m_EntityChunk;
		m_Local->players[i].controllerBase = 0;
		m_Local->players[i].pawnHandle     = 0;
		m_Local->players[i].listEntry2     = 0;
		m_Local->players[i].pawnBase       = 0;
	}

	// Pass 1: read entity identity header for each controller slot.
	// Validates handle & 0x7FFF == slot index before trusting the entity pointer.
	// This filters stale slots (disconnected players, deathmatch reconnects) where
	// the old entity pointer lingers until the slot is reused or zeroed.
	static IdentityHead ctrlHeads[MAX_ENTITIES];
	memset(ctrlHeads, 0, sizeof(ctrlHeads));

	for (int i = 0; i < MAX_ENTITIES; i++)
		g_Scatter->AddRaw(m_EntityChunk + 0x70 * (i + 1), 0x14, &ctrlHeads[i]);
	g_Scatter->Execute();
	g_Scatter->Clear();

	for (int i = 0; i < MAX_ENTITIES; i++) {
		if ((ctrlHeads[i].handle & 0x7FFF) != (uint32_t)(i + 1)) continue;
		m_Local->players[i].controllerBase = ctrlHeads[i].entityPtr;
	}

	// If a slot is now occupied by a different controller, the preserved ctrl/pawn
	// data belongs to the old player. Clear it so the stale teamID doesn't miscolor
	// the new player until t_PlayerCtrl refreshes it.
	for (int i = 0; i < MAX_ENTITIES; i++) {
		uint64_t cur = m_Local->players[i].controllerBase;
		if (cur && cur != prevControllers[i]) {
			m_Local->players[i].ctrl = {};
			m_Local->players[i].pawn = {};
		}
	}

	// Pass 2: controllerBase → pawnHandle (uint32_t CHandle)
	for (int i = 0; i < MAX_ENTITIES; i++)
		if (m_Local->players[i].controllerBase)
			g_Scatter->Add(m_Local->players[i].controllerBase + client_dll::CCSPlayerController::m_hPlayerPawn,
			               &m_Local->players[i].pawnHandle);
	g_Scatter->Execute();
	g_Scatter->Clear();

	// Pass 3: pawnHandle → listEntry2
	// Skip 0 (no pawn) and 0xFFFFFFFF (invalid/pre-spawn handle)
	for (int i = 0; i < MAX_ENTITIES; i++) {
		uint32_t h = m_Local->players[i].pawnHandle;
		if (!h || h == 0xFFFFFFFF) continue;
		g_Scatter->Add(entityList + 0x8 * ((h & 0x7FFF) >> 9) + 16,
		               &m_Local->players[i].listEntry2);
	}
	g_Scatter->Execute();
	g_Scatter->Clear();

	// Pass 4: read pawn identity header to validate handle before storing pawnBase.
	// Same stale-slot guard as Pass 1: pawn slot index encoded in pawnHandle must
	// match the identity's own handle field.
	static IdentityHead pawnHeads[MAX_ENTITIES];
	memset(pawnHeads, 0, sizeof(pawnHeads));

	for (int i = 0; i < MAX_ENTITIES; i++) {
		uint32_t h = m_Local->players[i].pawnHandle;
		if (!h || h == 0xFFFFFFFF || !m_Local->players[i].listEntry2) continue;
		g_Scatter->AddRaw(m_Local->players[i].listEntry2 + 0x70 * (h & 0x1FF),
		                  0x14, &pawnHeads[i]);
	}
	g_Scatter->Execute();
	g_Scatter->Clear();

	for (int i = 0; i < MAX_ENTITIES; i++) {
		uint32_t h = m_Local->players[i].pawnHandle;
		if (!h || h == 0xFFFFFFFF) continue;
		if ((pawnHeads[i].handle & 0x7FFF) != (h & 0x7FFF)) continue;
		m_Local->players[i].pawnBase = pawnHeads[i].entityPtr;
	}
}
