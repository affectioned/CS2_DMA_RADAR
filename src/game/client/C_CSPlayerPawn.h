#pragma once
#include <cstdint>
#include "vec.h"

struct C_CSPlayerPawn {
	uint32_t health              = 0;
	uint8_t  lifeState           = 0;
	Vector2  eyeAngles           = {};
	Vector3  position            = {};
	uint16_t activeWeaponID      = 0;
	bool     isDefusing          = false;
	char     lastPlaceName[18]   = {};
	uint64_t weaponServicesPtr   = 0;

	void Read(uint64_t base);
	void ReadFast(uint64_t base);
	void ReadStatus(uint64_t base);
};
