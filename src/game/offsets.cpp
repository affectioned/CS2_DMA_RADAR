#include "pch.h"
#include "offsets.h"

// Compiled-in offset values. Used at startup until updater::sigscanOffsets()
// (for dw*) and updater::fetchClassOffsets() (for class members) overwrite
// them. Values are taken from the cs2-dumper snapshot at the time of build,
// and serve as a fallback when both the cs2-dumper HTTP fetch and the local
// disk cache are unavailable.

namespace client_dll {
	// ── Module-level RVA pointers ────────────────────────────────────────────
	std::ptrdiff_t dwEntityList            = 0x2710038;
	std::ptrdiff_t dwLocalPlayerController = 0x25324D8;
	std::ptrdiff_t dwLocalPlayerPawn       = 0x255B598;
	std::ptrdiff_t dwGlobalVars            = 0x2226F08;
	std::ptrdiff_t dwPlantedC4             = 0x24C3D28;
	std::ptrdiff_t dwWeaponC4              = 0x24BF400;
	std::ptrdiff_t dwGameRules             = 0x255AA88;

	// ── Class member offsets ─────────────────────────────────────────────────

	namespace C_BaseEntity {
		std::ptrdiff_t m_iHealth          = 0x34C;
		std::ptrdiff_t m_lifeState        = 0x354;
		std::ptrdiff_t m_iTeamNum         = 0x3E7;
		std::ptrdiff_t m_pGameSceneNode   = 0x330;
		std::ptrdiff_t m_hOwnerEntity     = 0x520;
	}

	namespace CGameSceneNode {
		std::ptrdiff_t m_vecAbsOrigin     = 0xC8;
	}

	namespace C_BasePlayerPawn {
		std::ptrdiff_t m_pWeaponServices    = 0x12F0;
		std::ptrdiff_t m_pObserverServices  = 0x1308;
		std::ptrdiff_t m_vOldOrigin         = 0x14A4;
	}

	namespace CPlayer_WeaponServices {
		std::ptrdiff_t m_hActiveWeapon    = 0x60;
	}

	namespace CPlayer_ObserverServices {
		std::ptrdiff_t m_hObserverTarget  = 0x4C;
	}

	namespace CCSPlayerController {
		std::ptrdiff_t m_sSanitizedPlayerName = 0x878;
		std::ptrdiff_t m_iCompTeammateColor   = 0x858;
		std::ptrdiff_t m_hPlayerPawn          = 0x92C;
		std::ptrdiff_t m_iPawnArmor           = 0x93C;
		std::ptrdiff_t m_bPawnHasDefuser      = 0x940;
		std::ptrdiff_t m_bPawnHasHelmet       = 0x941;
	}

	namespace C_CSPlayerPawn {
		std::ptrdiff_t m_szLastPlaceName  = 0x15BC;
		std::ptrdiff_t m_bIsDefusing      = 0x1EA2;
		std::ptrdiff_t m_angEyeAngles     = 0x35F0;
	}

	namespace C_CSGameRulesProxy {
		std::ptrdiff_t m_pGameRules       = 0x600;
	}

	namespace C_CSGameRules {
		std::ptrdiff_t m_iRoundEndWinnerTeam = 0xF08;
	}

	namespace C_PlantedC4 {
		std::ptrdiff_t m_bBombTicking     = 0x1288;
		std::ptrdiff_t m_nBombSite        = 0x128C;
		std::ptrdiff_t m_flC4Blow         = 0x12B8;
		std::ptrdiff_t m_bHasExploded     = 0x12BD;
		std::ptrdiff_t m_bBeingDefused    = 0x12C4;
		std::ptrdiff_t m_bC4Activated     = 0x12D0;
		std::ptrdiff_t m_bBombDefused     = 0x12DC;
	}

	namespace C_EconEntity {
		std::ptrdiff_t m_AttributeManager = 0x1290;
	}

	namespace C_AttributeContainer {
		std::ptrdiff_t m_Item             = 0x50;
	}

	namespace C_EconItemView {
		std::ptrdiff_t m_iItemDefinitionIndex = 0x1BA;
	}
}

namespace engine2_dll {
	std::ptrdiff_t dwBuildNumber                   = 0x61C1EC;
	std::ptrdiff_t dwNetworkGameClient             = 0x91A150;
	std::ptrdiff_t dwNetworkGameClient_signOnState = 0x230;
}
