#include "pch.h"
#include "CCSPlayerController.h"
#include "offsets.h"
#include "GameGlobals.h"
#include "../CS2Context.h"

void CCSPlayerController::Read(uint64_t base) {
	g_Scatter->Add(base + client_dll::CCSPlayerController::m_sSanitizedPlayerName, &nameAddr);
	g_Scatter->Add(base + client_dll::C_BaseEntity::m_iTeamNum,                    &teamID);
	g_Scatter->Add(base + client_dll::CCSPlayerController::m_iCompTeammateColor,   &color);
	g_Scatter->Add(base + client_dll::CCSPlayerController::m_iPawnArmor,           &armor);
	g_Scatter->Add(base + client_dll::CCSPlayerController::m_bPawnHasDefuser,      &hasDefuser);
	g_Scatter->Add(base + client_dll::CCSPlayerController::m_bPawnHasHelmet,       &hasHelmet);
}

void CCSPlayerController::ReadName() {
	if (nameAddr)
		g_Scatter->AddRaw(nameAddr, sizeof(name), name);
}

// ── t_PlayerCtrl — 150 ms ─────────────────────────────────────────────────────
// Reads controller fields (teamID, color, ping, armor, defuser, helmet)
// for every player with a valid controllerBase.

void CS2Context::t_PlayerCtrl()
{
	ZoneScoped;
	if (m_Local->localPlayer.controllerBase)
		m_Local->localPlayer.ctrl.Read(m_Local->localPlayer.controllerBase);
	for (int i = 0; i < MAX_ENTITIES; i++)
		if (m_Local->players[i].controllerBase)
			m_Local->players[i].ctrl.Read(m_Local->players[i].controllerBase);
	g_Scatter->Execute();
	g_Scatter->Clear();
}

// ── t_PlayerNames — 5000 ms ───────────────────────────────────────────────────
// Reads player name strings via the nameAddr pointer from the controller.
// 5 s interval is fine — names only change on team-swap or reconnect.

void CS2Context::t_PlayerNames()
{
	ZoneScoped;
	for (int i = 0; i < MAX_ENTITIES; i++)
		if (m_Local->players[i].ctrl.nameAddr)
			m_Local->players[i].ctrl.ReadName();
	g_Scatter->Execute();
	g_Scatter->Clear();

	for (int i = 0; i < MAX_ENTITIES; i++)
		m_Local->players[i].ctrl.name[sizeof(m_Local->players[i].ctrl.name) - 1] = '\0';
}
