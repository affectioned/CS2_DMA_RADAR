#include "pch.h"
#include "gui.h"
#include "sdk.h"

static constexpr float PI = 3.14159265f;

gui::RadarFrame gui::RadarFrame::compute(const CGame& game) {
	RadarFrame f{};
	auto it = maps::mapBounds.find(game.mapName);
	f.bounds = (it != maps::mapBounds.end() && it->second.scale != 0.0f) ? &it->second : nullptr;
	f.rotates = settings::rotateRadar && game.localPlayer.controllerBase != 0;

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	if (f.rotates) {
		// Rotated radar fills the entire display with the local player at center.
		// Baseline pxPerWorld off the screen diagonal keeps a rotating WxH
		// viewport fully inside the square map texture so corners don't fall
		// into the transparent-border region. The user-controllable radarZoom
		// multiplies that baseline: >1 zooms in tighter; <1 widens the view
		// but allows borders.
		f.halfX = display.x * 0.5f;
		f.halfY = display.y * 0.5f;
		const float diag = sqrtf(display.x * display.x + display.y * display.y);
		if (f.bounds) f.pxPerWorld = settings::radarZoom * diag / (1024.0f * f.bounds->scale);

		f.yawDeg = game.localPlayer.pawn.eyeAngles.y;
		const float yaw = f.yawDeg * PI / 180.0f;
		f.sinYaw = sinf(yaw);
		f.cosYaw = cosf(yaw);
		f.localX = game.localPlayer.pawn.position.x;
		f.localY = game.localPlayer.pawn.position.y;
	} else {
		f.halfX = maps::radarSize * 0.5f;
		f.halfY = maps::radarSize * 0.5f;
		if (f.bounds) f.pxPerWorld = maps::radarSize / (1024.0f * f.bounds->scale);
	}
	return f;
}

void gui::gameLoop(const CGame& game) {
	ZoneScoped;
	std::string mapName = game.mapName;
	if (mapName.empty()) return;

	auto boundsIt = maps::mapBounds.find(mapName);
	if (boundsIt != maps::mapBounds.end()) {
		float thresh = boundsIt->second.lowerZThreshold;
		if (game.localPlayer.pawn.position.z <= thresh) {
			std::string lower = mapName + "_lower";
			if (maps::mapTextures.count(lower))
				mapName = lower;
		}
	}

	auto texIt = maps::mapTextures.find(mapName);
	if (texIt == maps::mapTextures.end() || !texIt->second) return;

	ImVec2 display = ImGui::GetIO().DisplaySize;
	maps::radarSize = std::min(display.x, display.y);

	const RadarFrame frame = RadarFrame::compute(game);

	renderMap(texIt->second, frame);
	renderPlayers(game, frame);
	renderBomb(game, frame);
	ImGui::End();
}

void gui::renderMap(ID3D11ShaderResourceView* texture, const RadarFrame& f) {
	ZoneScoped;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	if (f.rotates) {
		// Cover the full display so the map fills the screen — the square
		// min(W,H) window would leave letterbox bars on widescreen.
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(display);
	} else {
		ImGui::SetNextWindowPos(ImVec2(
			(display.x - maps::radarSize) * 0.5f,
			(display.y - maps::radarSize) * 0.5f
		));
		ImGui::SetNextWindowSize(ImVec2(maps::radarSize, maps::radarSize));
	}
	ImGui::Begin("MAP", nullptr,
		ImGuiWindowFlags_NoBackground |
		ImGuiWindowFlags_NoScrollbar  |
		ImGuiWindowFlags_NoTitleBar   |
		ImGuiWindowFlags_NoResize     |
		ImGuiWindowFlags_NoInputs
	);

	if (f.rotates && f.bounds) {
		// Off-map corners (only reachable when the local player is near a map
		// edge — the diagonal-zoom in compute() prevents it elsewhere) need to
		// render as transparent black rather than the tiled wraparound that
		// ImGui's default WRAP sampler produces. Swap in g_pRadarSampler
		// (BORDER mode) for this quad and reset render state immediately after.
		const float texWorldSize = 1024.0f * f.bounds->scale;
		ImVec2 winPos = ImGui::GetWindowPos();
		ImVec2 corners[4] = {
			{0,         0        },
			{display.x, 0        },
			{display.x, display.y},
			{0,         display.y},
		};
		ImVec2 uvs[4];
		for (int i = 0; i < 4; i++) {
			const float sx = corners[i].x - f.halfX;
			const float sy = corners[i].y - f.halfY;
			// Inverse of worldToRadar's rotation: screen offset -> world delta.
			const float dwx = ( f.sinYaw * sx - f.cosYaw * sy) / f.pxPerWorld;
			const float dwy = (-f.cosYaw * sx - f.sinYaw * sy) / f.pxPerWorld;
			const float wx  = f.localX + dwx;
			const float wy  = f.localY + dwy;
			uvs[i].x = (wx - f.bounds->xBound) / texWorldSize;
			uvs[i].y = (f.bounds->yBound - wy) / texWorldSize;
		}

		// Emit into the background drawlist so the full-screen map sits below
		// every panel (ControlPanel/Team/Bomb) without depending on Begin order.
		ImDrawList* dl = ImGui::GetBackgroundDrawList();
		dl->AddCallback([](const ImDrawList*, const ImDrawCmd*) {
			gui::g_pd3dDeviceContext->PSSetSamplers(0, 1, &gui::g_pRadarSampler);
		}, nullptr);
		dl->AddImageQuad(
			(ImTextureID)texture,
			ImVec2(winPos.x + corners[0].x, winPos.y + corners[0].y),
			ImVec2(winPos.x + corners[1].x, winPos.y + corners[1].y),
			ImVec2(winPos.x + corners[2].x, winPos.y + corners[2].y),
			ImVec2(winPos.x + corners[3].x, winPos.y + corners[3].y),
			uvs[0], uvs[1], uvs[2], uvs[3]);
		dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
	} else {
		ImGui::Image((ImTextureID)texture, ImVec2(maps::radarSize, maps::radarSize));
	}

	ImGui::PopStyleVar(2);
}

void gui::renderPlayers(const CGame& game, const RadarFrame& f) {
	ZoneScoped;
	ImVec2      windowPos = ImGui::GetWindowPos();
	const float localZ    = game.localPlayer.pawn.position.z;

	for (int i = 0; i < MAX_ENTITIES; i++) {
		const CPlayer& p = game.players[i];

		bool visible = p.controllerBase && p.pawn.lifeState == 0 && p.ctrl.teamID >= TEAM_T && p.pawn.health > 0;
		if (!visible) continue;

		// Local player counts as friendly — friendly toggles cover both teammates and self.
		const bool isEnemy = p.ctrl.teamID != game.localPlayer.ctrl.teamID;

		float x = p.pawn.position.x;
		float y = p.pawn.position.y;
		float z = p.pawn.position.z;
		// In rotated mode the world frame turns with the local player: their
		// yaw becomes "up" (90°), everyone else offsets relative to that.
		// Use f.yawDeg for consistency with the map quad's rotation, even
		// though it's currently equal to raw eyeAngles.y.
		float angleDeg = p.pawn.eyeAngles.y;
		if (f.rotates) angleDeg = angleDeg - f.yawDeg + 90.0f;
		const float angle = angleDeg * PI / 180.0f;
		worldToRadar(x, y, f);

		float  opacity = setOpacity(localZ, z, game);
		ImVec2 pos     = ImVec2(windowPos.x + x, windowPos.y + y);

		// Aim line
		const bool wantAim = isEnemy ? settings::showAimLinesEnemies : settings::showAimLinesFriendlies;
		if (wantAim) {
			ImVec2 endpoint = ImVec2(pos.x + settings::aimLineLength * cosf(angle) + 1.0f,
			                         pos.y + settings::aimLineLength * sinf(angle) * -1.0f + 1.0f);
			ImGui::GetForegroundDrawList()->AddLine(pos, endpoint, IM_COL32(0,   0,   0,   (int)opacity), 6.5f);
			ImGui::GetForegroundDrawList()->AddLine(pos, endpoint, IM_COL32(255, 255, 255, (int)opacity), 4.0f);
		}

		// Weapon icon
		const bool wantWpnIcon = isEnemy ? settings::showWeaponIconsEnemies : settings::showWeaponIconsFriendlies;
		if (wantWpnIcon) {
			int weaponID = p.pawn.activeWeaponID;
			auto texIt = icons::iconTextures.find(weaponID);
			if (texIt != icons::iconTextures.end() && texIt->second
				&& icons::iconWidths.count(weaponID) && icons::iconHeights.count(weaponID)) {
				float  iconW   = (float)icons::iconWidths[weaponID]  * settings::iconScale;
				float  iconH   = (float)icons::iconHeights[weaponID] * settings::iconScale;
				ImVec2 iconPos = (angle >= 0 && angle <= PI)
					? ImVec2(pos.x - iconW / 2, pos.y + 20.f)
					: ImVec2(pos.x - iconW / 2, pos.y - 20.f - iconH);
				ImVec2 iconBR  = ImVec2(iconPos.x + iconW, iconPos.y + iconH);
				ImDrawList* fdl = ImGui::GetForegroundDrawList();

				// Outline: 8 black-tinted copies offset by 1px in cardinal +
				// diagonal directions, then the colored icon on top. Tinting
				// the texture by IM_COL32(0,0,0,255) multiplies RGB to zero
				// while preserving the PNG's alpha mask, so each offset copy
				// is the icon's silhouette in solid black.
				constexpr float kOL = 1.0f;
				static const ImVec2 kOutlineOffsets[8] = {
					{-kOL, -kOL}, {0, -kOL}, {kOL, -kOL},
					{-kOL,    0},            {kOL,    0},
					{-kOL,  kOL}, {0,  kOL}, {kOL,  kOL},
				};
				for (const ImVec2& o : kOutlineOffsets) {
					fdl->AddImage(
						(ImTextureID)texIt->second,
						ImVec2(iconPos.x + o.x, iconPos.y + o.y),
						ImVec2(iconBR.x  + o.x, iconBR.y  + o.y),
						ImVec2(0, 0), ImVec2(1, 1),
						IM_COL32(0, 0, 0, 255)
					);
				}
				fdl->AddImage(
					(ImTextureID)texIt->second,
					iconPos, iconBR,
					ImVec2(0, 0), ImVec2(1, 1),
					IM_COL32(255, 255, 255, 255)
				);
			}
		}

		// Player dot — friendlies (incl. local player) use their compTeammateColor;
		// enemies are red. Local player no longer special-cased white so their
		// dot matches the team-panel/teammate scheme.
		const ImU32 dotColor = (p.ctrl.teamID == game.localPlayer.ctrl.teamID)
			? setColor(p.ctrl.color, opacity)
			: IM_COL32(255, 9, 9, (int)opacity);

		// Defusing ring -- orange, enemies only
		if (p.pawn.isDefusing && p.ctrl.teamID != game.localPlayer.ctrl.teamID)
			ImGui::GetForegroundDrawList()->AddCircle(pos, settings::dotRadius + 3.5f, IM_COL32(255, 150, 20, (int)opacity), 0, 2.0f);

		ImGui::GetForegroundDrawList()->AddCircleFilled(pos, settings::dotRadius + 1.25f, IM_COL32(0, 0, 0, 255));
		ImGui::GetForegroundDrawList()->AddCircleFilled(pos, settings::dotRadius,         dotColor);

		// Health bar
		const bool wantHpBar = isEnemy ? settings::showHealthBarsEnemies : settings::showHealthBarsFriendlies;
		if (wantHpBar) {
			constexpr float barW = 3.0f, barH = 18.0f, barX = 14.0f;
			float filled = (float)p.pawn.health / 100.0f;
			float r = 255.0f * (1.0f - filled);
			float g = 255.0f * filled;
			ImVec2 barTL = ImVec2(pos.x - barX - barW, pos.y - barH / 2);
			ImVec2 barBR = ImVec2(pos.x - barX,        pos.y + barH / 2);
			float  fillY = barBR.y - barH * filled;
			ImGui::GetForegroundDrawList()->AddRectFilled(barTL, barBR, IM_COL32(0, 0, 0, (int)opacity));
			ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(barTL.x, fillY), barBR, IM_COL32((int)r, (int)g, 0, (int)opacity));
		}

		// Player name (always skipped on the local player — own name on top of own dot is noise)
		const bool wantName = isEnemy ? settings::showPlayerNamesEnemies : settings::showPlayerNamesFriendlies;
		if (wantName && p.controllerBase != game.localPlayer.controllerBase && p.ctrl.name[0]) {
			ImVec2 textSize = ImGui::CalcTextSize(p.ctrl.name);
			ImVec2 namePos  = ImVec2(pos.x - textSize.x * 0.5f, pos.y + settings::dotRadius + 4.0f);
			ImGui::GetForegroundDrawList()->AddText(ImVec2(namePos.x + 1, namePos.y + 1), IM_COL32(0,   0,   0,   (int)opacity), p.ctrl.name);
			ImGui::GetForegroundDrawList()->AddText(namePos,                               IM_COL32(255, 255, 255, (int)opacity), p.ctrl.name);
		}
	}
}

void gui::renderBomb(const CGame& game, const RadarFrame& f) {
	ZoneScoped;
	ImVec2 windowPos = ImGui::GetWindowPos();
	const C_PlantedC4& b = game.bomb;

	// Carrier-only highlight — pre-plant. After the bomb is planted, the
	// planted-bomb dot itself is the visual cue; we don't continue tagging the
	// player who put it down.
	int highlightSlot = b.isCarried ? b.carrierSlot : -1;
	if (highlightSlot >= 0 && highlightSlot < (int)MAX_ENTITIES) {
		const CPlayer& carrier = game.players[highlightSlot];
		if (carrier.controllerBase && carrier.pawn.lifeState == 0 && carrier.pawn.health > 0) {
			float x = carrier.pawn.position.x;
			float y = carrier.pawn.position.y;
			worldToRadar(x, y, f);
			ImVec2 pos = ImVec2(windowPos.x + x, windowPos.y + y);
			// Two concentric rings: black backdrop + bright yellow on top, so
			// the halo is unambiguous against any map color (yellow dust2
			// sand, white snow maps, etc.) where a thin yellow ring alone
			// blended in.
			const float haloR = settings::dotRadius + 5.0f;
			ImDrawList* fdl = ImGui::GetForegroundDrawList();
			fdl->AddCircle(pos, haloR, IM_COL32(0,   0,   0, 220), 0, 3.5f);
			fdl->AddCircle(pos, haloR, IM_COL32(255, 220, 0, 255), 0, 2.0f);
		}
	}

	if (!b.entity || !b.isTicking || b.hasDefused || b.hasExploded) return;

	float bx = b.position.x;
	float by = b.position.y;
	worldToRadar(bx, by, f);
	ImVec2 pos = ImVec2(windowPos.x + bx, windowPos.y + by);

	ImGui::GetForegroundDrawList()->AddCircleFilled(pos, settings::dotRadius + 1.25f, IM_COL32(0, 0, 0, 255));
	ImGui::GetForegroundDrawList()->AddCircleFilled(pos, settings::dotRadius,         IM_COL32(255, 200, 0, 255));

	if (b.isBeingDefused)
		ImGui::GetForegroundDrawList()->AddCircle(pos, settings::dotRadius + 3.5f, IM_COL32(0, 200, 255, 220), 0, 2.0f);
}

void gui::worldToRadar(float& x, float& y, const RadarFrame& f) {
	if (!f.bounds || maps::radarSize < 1.0f) return;

	if (f.rotates) {
		const float dwx = x - f.localX;
		const float dwy = y - f.localY;
		const float right   = dwx * f.sinYaw - dwy * f.cosYaw;
		const float forward = dwx * f.cosYaw + dwy * f.sinYaw;
		x = f.halfX + right   * f.pxPerWorld;
		y = f.halfY - forward * f.pxPerWorld;
		return;
	}

	// Standard CS2 overview projection.
	x = (x - f.bounds->xBound) * f.pxPerWorld;
	y = (f.bounds->yBound - y) * f.pxPerWorld;
}

ImU32 gui::setColor(DWORD color, float opacity) {
	switch (color) {
	case -1: return IM_COL32(142, 212, 210, (int)opacity);
	case  0: return IM_COL32(  0, 255, 251, (int)opacity);
	case  1: return IM_COL32( 47, 255,   0, (int)opacity);
	case  2: return IM_COL32(255, 255,   0, (int)opacity);
	case  3: return IM_COL32(250, 130,   2, (int)opacity);
	case  4: return IM_COL32(250,   2, 182, (int)opacity);
	default: return IM_COL32(133, 204, 148, (int)opacity);
	}
}

float gui::setOpacity(float localZ, float entZ, const CGame& game) {
	auto it = maps::mapBounds.find(game.mapName);
	if (it == maps::mapBounds.end()) return 255;
	float thresh = it->second.lowerZThreshold;
	if (thresh == std::numeric_limits<float>::infinity()) return 255;
	if ((localZ <= thresh) != (entZ <= thresh)) return 155;
	return 255;
}
