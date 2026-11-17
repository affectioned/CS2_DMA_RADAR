#pragma once
#include "vec.h"
#include <chrono>
#include <cstdint>

struct C_PlantedC4 {
	uint64_t entity    = 0;
	uint64_t sceneNode = 0;
	Vector3  position  = {};

	bool    isTicking      = false;
	bool    isBeingDefused = false;
	bool    hasExploded    = false;
	bool    hasDefused     = false;
	int32_t site           = -1;

	bool isCarried   = false;
	int  carrierSlot = -1;

	// Dropped-on-ground state. Independent of isCarried / planted — when
	// isDropped is true the C4 weapon entity is sitting in the world and
	// droppedPos tracks its scene-node origin. Gated by
	// CCSGameRules::m_bBombDropped (server-authoritative) so it won't
	// flicker from the race on C_C4::m_hOwnerEntity during handoff.
	bool     isDropped  = false;
	Vector3  droppedPos = {};

	float c4Blow = 0.0f; // m_flC4Blow: absolute game time of detonation; used as bomb identity

	using Clock = std::chrono::steady_clock;
	Clock::time_point plantTime    = {};
	bool              plantTimeSet = false;

	float timeRemaining() const {
		if (!isTicking || !plantTimeSet) return 40.0f;
		float elapsed = std::chrono::duration<float>(Clock::now() - plantTime).count();
		return std::max(0.0f, 40.0f - elapsed);
	}
};
