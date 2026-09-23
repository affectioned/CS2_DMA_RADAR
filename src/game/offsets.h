#pragma once
#include <cstddef>

// All offsets below are populated at startup:
//   - dw* RVA pointers via pattern matching (updater::resolveOffsets)
//   - class member offsets via HTTP fetch + cache (updater::fetchClassOffsets)
// Compiled-in values act as fallbacks when both network and cache are unavailable.

namespace client_dll {
	// ── Module-level RVA pointers ────────────────────────────────────────────
	// dw* with no pattern in updater.cpp are populated from
	// cs2-dumper's offsets.hpp (HTTP fetch with ETag cache, same pattern as
	// class members).
	extern std::ptrdiff_t dwEntityList;
	extern std::ptrdiff_t dwLocalPlayerController;
	extern std::ptrdiff_t dwLocalPlayerPawn;
	extern std::ptrdiff_t dwGlobalVars;
	extern std::ptrdiff_t dwPlantedC4;
	extern std::ptrdiff_t dwWeaponC4;          // global C4 weapon entity ptr (held, not planted)
	extern std::ptrdiff_t dwGameRules;         // C_CSGameRulesProxy* — pattern-resolved

	// ── Class member offsets ─────────────────────────────────────────────────

	namespace C_BaseEntity {
		extern std::ptrdiff_t m_iHealth;          // int32
		extern std::ptrdiff_t m_lifeState;        // uint8
		extern std::ptrdiff_t m_iTeamNum;         // uint8
		extern std::ptrdiff_t m_pGameSceneNode;   // CGameSceneNode*
		extern std::ptrdiff_t m_hOwnerEntity;     // CHandle<C_BaseEntity>
	}

	namespace CGameSceneNode {
		extern std::ptrdiff_t m_vecAbsOrigin;     // VectorWS
	}

	namespace C_BasePlayerPawn {
		extern std::ptrdiff_t m_pWeaponServices;    // CPlayer_WeaponServices*
		extern std::ptrdiff_t m_pObserverServices;  // CPlayer_ObserverServices*
		extern std::ptrdiff_t m_vOldOrigin;         // Vector
	}

	namespace CPlayer_WeaponServices {
		extern std::ptrdiff_t m_hActiveWeapon;    // CHandle<C_BasePlayerWeapon>
	}

	namespace CPlayer_ObserverServices {
		extern std::ptrdiff_t m_hObserverTarget;  // CHandle<C_BaseEntity> — pawn we're spectating
	}

	namespace CCSPlayerController {
		extern std::ptrdiff_t m_sSanitizedPlayerName; // CUtlString
		extern std::ptrdiff_t m_iCompTeammateColor;   // int32
		extern std::ptrdiff_t m_hPlayerPawn;          // CHandle<C_CSPlayerPawn>
		extern std::ptrdiff_t m_iPawnArmor;           // int32
		extern std::ptrdiff_t m_bPawnHasDefuser;      // bool
		extern std::ptrdiff_t m_bPawnHasHelmet;       // bool
	}

	namespace C_CSPlayerPawn {
		extern std::ptrdiff_t m_szLastPlaceName;  // char[18]
		extern std::ptrdiff_t m_bIsDefusing;      // bool
		extern std::ptrdiff_t m_angEyeAngles;     // QAngle
	}

	namespace C_CSGameRulesProxy {
		extern std::ptrdiff_t m_pGameRules;       // C_CSGameRules*
	}

	namespace C_CSGameRules {
		extern std::ptrdiff_t m_iRoundEndWinnerTeam; // int32 — 0 mid-round, non-zero at round end
	}

	namespace C_PlantedC4 {
		extern std::ptrdiff_t m_bBombTicking;     // bool
		extern std::ptrdiff_t m_nBombSite;        // int32
		extern std::ptrdiff_t m_flC4Blow;         // GameTime_t
		extern std::ptrdiff_t m_bHasExploded;     // bool
		extern std::ptrdiff_t m_bBeingDefused;    // bool
		extern std::ptrdiff_t m_bC4Activated;     // bool
		extern std::ptrdiff_t m_bBombDefused;     // bool
	}

	namespace C_EconEntity {
		extern std::ptrdiff_t m_AttributeManager; // C_AttributeContainer
	}

	namespace C_AttributeContainer {
		extern std::ptrdiff_t m_Item;             // C_EconItemView
	}

	namespace C_EconItemView {
		extern std::ptrdiff_t m_iItemDefinitionIndex; // uint16
	}
}

namespace engine2_dll {
	// Module: engine2.dll — populated from cs2-dumper's offsets.hpp.
	extern std::ptrdiff_t dwBuildNumber;                       // int32 — CS2 build at this address
	extern std::ptrdiff_t dwNetworkGameClient;                 // CNetworkGameClient* (static singleton ptr)
	extern std::ptrdiff_t dwNetworkGameClient_signOnState;     // u8 inside the CNetworkGameClient
}
