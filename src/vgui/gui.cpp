#include "pch.h"
#include "gui.h"
#include "sdk.h"
#include "updater.h"

extern std::atomic<bool> bRunning;

namespace gui {
	ID3D11Device* g_pd3dDevice = nullptr;
	ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
	IDXGISwapChain* g_pSwapChain = nullptr;
	UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
	ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
	ID3D11SamplerState* g_pRadarSampler = nullptr;
	bool exitRequested = false;
	WNDCLASSEXW wc = {};
	HWND hwnd = nullptr;
}

namespace maps {
	float radarSize = 1080;
	std::unordered_map<std::string, ID3D11ShaderResourceView*> mapTextures;
	std::unordered_map<std::string, mapData> mapBounds;
}

namespace icons {
	std::unordered_map<int, ID3D11ShaderResourceView*> iconTextures;
	std::unordered_map<int, int> iconWidths;
	std::unordered_map<int, int> iconHeights;
}

namespace settings {
	bool  showAimLinesEnemies    = true,  showAimLinesFriendlies    = false;
	bool  showWeaponIconsEnemies = true,  showWeaponIconsFriendlies = false;
	bool  showHealthBarsEnemies  = true,  showHealthBarsFriendlies  = false;
	bool  showPlayerNamesEnemies = false, showPlayerNamesFriendlies = false;
	bool  showTeamPanels         = true;
	bool  rotateRadar            = true;
	float iconScale              = 0.2f;
	float aimLineLength          = 40.0f;
	float dotRadius              = 8.0f;
	float radarZoom              = 0.7f;
}

void gui::CreateAppWindow() {
	wc = { sizeof(wc), CS_VREDRAW | CS_HREDRAW, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"CS2Radar", nullptr };
	::RegisterClassExW(&wc);
	hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"CS2Radar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
	Log::Info("[GUI]: Window created");
}

bool gui::InitD3D() {
	if (!CreateDeviceD3D(hwnd)) {
		CleanupDeviceD3D();
		::UnregisterClassW(wc.lpszClassName, wc.hInstance);
		Log::Error("[GUI]: Direct3D initialization failed");
		return false;
	}
	Log::Info("[GUI]: Direct3D initialized");
	return true;
}

void gui::ShowAppWindow() {
	::ShowWindow(hwnd, SW_MAXIMIZE);
	::UpdateWindow(hwnd);
}

void gui::InitImGui() {
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	// Hook our settings into ImGui's .ini system — loads on first NewFrame(), saves on DestroyContext()
	ImGui::GetIO().IniFilename = "CS2_DMA_RADAR.ini";

	ImGuiSettingsHandler h;
	h.TypeName   = "CS2Radar";
	h.TypeHash   = ImHashStr("CS2Radar");
	h.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char*) -> void* { return (void*)1; };
	struct BoolKey  { const char* key; bool*  val; };
	struct FloatKey { const char* key; float* val; };
	static const BoolKey kBoolKeys[] = {
		{ "AimLinesEnemies",       &settings::showAimLinesEnemies    },
		{ "AimLinesFriendlies",    &settings::showAimLinesFriendlies },
		{ "WeaponIconsEnemies",    &settings::showWeaponIconsEnemies },
		{ "WeaponIconsFriendlies", &settings::showWeaponIconsFriendlies },
		{ "HealthBarsEnemies",     &settings::showHealthBarsEnemies  },
		{ "HealthBarsFriendlies",  &settings::showHealthBarsFriendlies },
		{ "PlayerNamesEnemies",    &settings::showPlayerNamesEnemies },
		{ "PlayerNamesFriendlies", &settings::showPlayerNamesFriendlies },
		{ "TeamPanels",            &settings::showTeamPanels         },
		{ "RotateRadar",           &settings::rotateRadar            },
	};
	static const FloatKey kFloatKeys[] = {
		{ "IconScale",  &settings::iconScale     },
		{ "AimLength",  &settings::aimLineLength },
		{ "DotRadius",  &settings::dotRadius     },
		{ "RadarZoom",  &settings::radarZoom     },
	};

	h.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
		for (const auto& k : kBoolKeys) {
			char fmt[64];
			snprintf(fmt, sizeof(fmt), "%s=%%d", k.key);
			int i;
			if (sscanf_s(line, fmt, &i) == 1) { *k.val = (i != 0); return; }
		}
		for (const auto& k : kFloatKeys) {
			char fmt[64];
			snprintf(fmt, sizeof(fmt), "%s=%%f", k.key);
			float f;
			if (sscanf_s(line, fmt, &f) == 1) { *k.val = f; return; }
		}
	};
	h.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
		buf->appendf("[%s][Settings]\n", h->TypeName);
		for (const auto& k : kBoolKeys)  buf->appendf("%s=%d\n",   k.key, *k.val);
		for (const auto& k : kFloatKeys) buf->appendf("%s=%.3f\n", k.key, *k.val);
		buf->append("\n");
	};
	ImGui::AddSettingsHandler(&h);

	// Load a font with broad Unicode coverage (Latin + Cyrillic) for player names
	{
		static const ImWchar kRanges[] = {
			0x0020, 0x00FF,  // Latin Basic + Latin-1 Supplement
			0x0400, 0x052F,  // Cyrillic + Cyrillic Supplement
			0,
		};
		// 16px Segoe UI Semibold — the overlay runs on a second monitor,
		// so glance-readability matters more than a tight UI. Microsoft uses
		// the truncated "segui" prefix for the semibold/light variants
		// (seguisb.ttf, not segoeuisb.ttf). Try semibold first; fall back to
		// regular, then to ImGui's bitmap font if neither system font loads.
		ImFontConfig cfg;
		cfg.OversampleH = 1; cfg.OversampleV = 1; cfg.PixelSnapH = true;
		constexpr float kFontPx = 16.0f;
		ImFont* font =
			ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", kFontPx, &cfg, kRanges);
		if (!font)
			font = ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", kFontPx, &cfg, kRanges);
		if (!font)
			ImGui::GetIO().Fonts->AddFontDefault();
	}

	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding    = 6.0f;
	style.FrameRounding     = 4.0f;
	style.GrabRounding      = 4.0f;
	style.WindowBorderSize  = 0.0f;
	style.FrameBorderSize   = 0.0f;
	style.ItemSpacing       = ImVec2(8.0f, 6.0f);
	style.WindowPadding     = ImVec2(12.0f, 10.0f);
	style.FramePadding      = ImVec2(8.0f, 4.0f);

	ImVec4* c = style.Colors;
	c[ImGuiCol_WindowBg]          = ImVec4(0.08f, 0.08f, 0.10f, 0.92f);
	c[ImGuiCol_Text]              = ImVec4(0.90f, 0.90f, 0.95f, 1.00f);
	c[ImGuiCol_TextDisabled]      = ImVec4(0.50f, 0.50f, 0.58f, 1.00f);
	c[ImGuiCol_Separator]         = ImVec4(0.22f, 0.22f, 0.28f, 1.00f);
	c[ImGuiCol_Button]            = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
	c[ImGuiCol_ButtonHovered]     = ImVec4(0.28f, 0.28f, 0.36f, 1.00f);
	c[ImGuiCol_ButtonActive]      = ImVec4(0.20f, 0.20f, 0.28f, 1.00f);
	c[ImGuiCol_FrameBg]           = ImVec4(0.12f, 0.12f, 0.16f, 1.00f);
	c[ImGuiCol_FrameBgHovered]    = ImVec4(0.20f, 0.20f, 0.26f, 1.00f);
	c[ImGuiCol_Header]            = ImVec4(0.20f, 0.20f, 0.26f, 1.00f);
	c[ImGuiCol_HeaderHovered]     = ImVec4(0.28f, 0.28f, 0.36f, 1.00f);

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
	Log::Info("[GUI]: ImGui {} initialized", IMGUI_VERSION);
}

void gui::OnFrame(CGame& game, std::mutex& gameMutex) {
	ZoneScoped;
	MSG msg;
	while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);
		if (msg.message == WM_QUIT) {
			Log::Info("Exit: WM_QUIT received");
			bRunning.store(false, std::memory_order_release);
			return;
		}
	}

	if (exitRequested) {
		Log::Info("Exit: exitRequested");
		bRunning.store(false, std::memory_order_release);
		return;
	}

	if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
		CleanupRenderTarget();
		g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
		g_ResizeWidth = g_ResizeHeight = 0;
		CreateRenderTarget();
	}

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	RenderControlPanel();

	if (g_Connected.load(std::memory_order_acquire)) {
		CGame snapshot;
		{
			std::lock_guard<std::mutex> lock(gameMutex);
			snapshot = game;
		}
		gameLoop(snapshot);
		if (settings::showTeamPanels)
			RenderTeamPanels(snapshot);
		RenderBombPanel(snapshot);
	}

	ImGui::Render();
	constexpr float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
	g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, black);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	g_pSwapChain->Present(0, 0);
	FrameMark;
}

void gui::Cleanup() {
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	for (auto& [k, v] : maps::mapTextures)  { if (v) { v->Release(); v = nullptr; } }
	for (auto& [k, v] : icons::iconTextures) { if (v) { v->Release(); v = nullptr; } }
	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(wc.lpszClassName, wc.hInstance);
	Log::Info("[GUI]: Cleanup complete");
}

void gui::RenderControlPanel() {
	ImGuiIO& io = ImGui::GetIO();
	constexpr float panelW = 220.0f;

	// FirstUseEver on the anchor lets the user drag the panel and have ImGui
	// persist the chosen position via its built-in [Window][##panel] section.
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panelW - 10.0f, 10.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(panelW, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin("##panel", nullptr,
		ImGuiWindowFlags_NoTitleBar        |
		ImGuiWindowFlags_NoResize          |
		ImGuiWindowFlags_NoScrollbar       |
		ImGuiWindowFlags_AlwaysAutoResize
	);

	// Title
	ImGui::SetCursorPosX((panelW - ImGui::CalcTextSize("CS2Radar").x) * 0.5f);
	ImGui::TextColored(ImVec4(0.5f, 0.78f, 1.0f, 1.0f), "CS2Radar");

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// FPS counter
	char fpsBuf[16];
	snprintf(fpsBuf, sizeof(fpsBuf), "%.0f FPS", io.Framerate);
	ImGui::TextDisabled("%s", fpsBuf);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Exit — neutral until hovered, then red tint
	ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.14f, 0.14f, 0.18f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.12f, 0.12f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.10f, 0.10f, 1.0f));
	if (ImGui::Button("Exit", ImVec2(-1.0f, 22.0f))) {
		Log::Info("Exit: button clicked");
		exitRequested = true;
		bRunning.store(false, std::memory_order_release);
	}
	ImGui::PopStyleColor(3);

	// Settings
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (ImGui::CollapsingHeader("Settings")) {
		ImGui::Spacing();

		if (ImGui::BeginTable("##overlay", 3,
		    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableSetupColumn("");
			ImGui::TableSetupColumn("Enemies");
			ImGui::TableSetupColumn("Friendlies");
			ImGui::TableHeadersRow();

			auto row = [](const char* label, bool* enem, bool* friendly,
			              const char* idEnem, const char* idFriend) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(label);
				ImGui::TableNextColumn(); ImGui::Checkbox(idEnem,   enem);
				ImGui::TableNextColumn(); ImGui::Checkbox(idFriend, friendly);
			};

			row("Aim Lines",    &settings::showAimLinesEnemies,    &settings::showAimLinesFriendlies,    "##aim_e",  "##aim_f");
			row("Weapon Icons", &settings::showWeaponIconsEnemies, &settings::showWeaponIconsFriendlies, "##wpn_e",  "##wpn_f");
			row("Health Bars",  &settings::showHealthBarsEnemies,  &settings::showHealthBarsFriendlies,  "##hp_e",   "##hp_f");
			row("Player Names", &settings::showPlayerNamesEnemies, &settings::showPlayerNamesFriendlies, "##name_e", "##name_f");
			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::Checkbox("Team Panels",  &settings::showTeamPanels);
		ImGui::Checkbox("Rotate Radar", &settings::rotateRadar);

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::SliderFloat("##iconscale", &settings::iconScale,    0.05f, 0.50f,  "Icons  %.2fx");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::SliderFloat("##aimlength", &settings::aimLineLength, 10.0f, 100.0f, "Aim  %.0fpx");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::SliderFloat("##dotradius", &settings::dotRadius,     3.0f,  14.0f,  "Dot  %.1fpx");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::SliderFloat("##zoom",      &settings::radarZoom,    0.5f,  3.0f,   "Zoom  %.2fx");

		ImGui::Spacing();
	}

	ImGui::End();
}

// ─── Team Panels ──────────────────────────────────────────────────────────────

static void renderPlayerEntry(const CPlayer& p, const CGame& /*game*/, bool isEnemy, bool isCarrier = false) {
	constexpr float kDotR   = 5.0f;
	constexpr float kIndent = kDotR * 2.0f + 8.0f;
	constexpr float kBarH   = 3.0f;
	constexpr float kIconH  = 14.0f;

	bool        alive  = p.pawn.lifeState == 0;
	float       alpha  = alive ? 1.0f : 0.38f;
	ImDrawList* dl     = ImGui::GetWindowDrawList();
	ImVec2      origin = ImGui::GetCursorScreenPos();
	float       lineH  = ImGui::GetTextLineHeight();
	float       winW   = ImGui::GetWindowWidth();

	// Dot
	ImU32 dotCol;
	if (!isEnemy)                                             dotCol = gui::setColor(p.ctrl.color, alpha * 255.0f);
	else                                                           dotCol = IM_COL32(255, 60,  60,  (int)(alpha * 255));

	ImVec2 dotCtr = { origin.x + kDotR + 2.0f, origin.y + lineH * 0.5f + 1.0f };
	//dl->AddCircleFilled(dotCtr, kDotR + 1.0f, IM_COL32(0, 0, 0, (int)(alpha * 180)));
	dl->AddCircleFilled(dotCtr, kDotR,         dotCol);

	// Name (left) + HP number (right)
	char hpBuf[8];
	if (alive) snprintf(hpBuf, sizeof(hpBuf), "%d", p.pawn.health);
	else       snprintf(hpBuf, sizeof(hpBuf), "---");

	float hpTextW  = ImGui::CalcTextSize(hpBuf).x;
	float nameMaxW = winW - kIndent - hpTextW - 20.0f;

	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kIndent);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.95f, alpha));

	ImVec2 nameScreen = ImGui::GetCursorScreenPos();
	ImGui::PushClipRect(nameScreen, { nameScreen.x + nameMaxW, nameScreen.y + lineH + 2.0f }, true);
	ImGui::TextUnformatted(p.ctrl.name[0] ? p.ctrl.name : "...");
	ImGui::PopClipRect();

	ImGui::SameLine(winW - hpTextW - 14.0f);
	if (alive) ImGui::Text("%d", p.pawn.health);
	else       ImGui::TextDisabled("---");
	ImGui::PopStyleColor();

	// Health bar
	float barStartX = origin.x + kIndent;
	float barW      = winW - kIndent - 14.0f;
	ImVec2 barTL    = { barStartX, ImGui::GetCursorScreenPos().y };
	float filled    = alive ? (float)p.pawn.health / 100.0f : 0.0f;
	if (filled > 1.0f) filled = 1.0f;
	float r = 255.0f * (1.0f - filled);
	float g = 255.0f * filled;
	dl->AddRectFilled(barTL, { barTL.x + barW,          barTL.y + kBarH }, IM_COL32(30, 30, 30, (int)(alpha * 200)), 1.5f);
	dl->AddRectFilled(barTL, { barTL.x + barW * filled, barTL.y + kBarH }, IM_COL32((int)r, (int)g, 0, (int)(alpha * 255)), 1.5f);
	ImGui::Dummy({ 0.0f, kBarH + 3.0f });

	// Weapon icon (left-indented)
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kIndent);
	if (alive) {
		auto texIt = icons::iconTextures.find(p.pawn.activeWeaponID);
		if (texIt != icons::iconTextures.end() && texIt->second) {
			int iw = icons::iconWidths.count(p.pawn.activeWeaponID)  ? icons::iconWidths.at(p.pawn.activeWeaponID)  : 1;
			int ih = icons::iconHeights.count(p.pawn.activeWeaponID) ? icons::iconHeights.at(p.pawn.activeWeaponID) : 1;
			float iconW = (ih > 0) ? kIconH * (float)iw / (float)ih : kIconH;
			ImGui::Image((ImTextureID)texIt->second, { iconW, kIconH });
		} else {
			ImGui::Dummy({ 0.0f, kIconH });
		}
	} else {
		ImGui::Dummy({ 0.0f, kIconH });
	}

	// Last place name (center of the row)
	if (alive && p.pawn.lastPlaceName[0]) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", p.pawn.lastPlaceName);
	}

	// Right-aligned: DEFUSING > C4 carrier > armor/defuser
	if (alive && p.pawn.isDefusing) {
		constexpr const char* kLabel = "DEFUSING";
		ImGui::SameLine(winW - ImGui::CalcTextSize(kLabel).x - 14.0f);
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f), kLabel);
	} else if (alive && isCarrier) {
		auto it = icons::iconTextures.find(49);
		if (it != icons::iconTextures.end() && it->second) {
			int iw = icons::iconWidths.count(49)  ? icons::iconWidths.at(49)  : 1;
			int ih = icons::iconHeights.count(49) ? icons::iconHeights.at(49) : 1;
			float iconW = (ih > 0) ? kIconH * (float)iw / (float)ih : kIconH;
			ImGui::SameLine(winW - iconW - 14.0f);
			// v1.91.9 dropped the tint_col arg from Image(); ImageWithBg replaces it
			// (signature: texture, size, uv0, uv1, bg_col, tint_col).
			ImGui::ImageWithBg((ImTextureID)it->second, { iconW, kIconH }, {0,0}, {1,1},ImVec4(0,0,0,0), ImVec4(1.0f, 0.88f, 0.0f, 1.0f));
		} else {
			constexpr const char* kLabel = "C4";
			ImGui::SameLine(winW - ImGui::CalcTextSize(kLabel).x - 14.0f);
			ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.0f, 1.0f), kLabel);
		}
	} else {
		char status[32] = {};
		if (alive && p.ctrl.armor > 0)
			snprintf(status, sizeof(status), "%s", p.ctrl.hasHelmet ? "H" : "K");
		if (alive && p.ctrl.hasDefuser) {
			size_t l = strlen(status);
			snprintf(status + l, sizeof(status) - l, "%sD", l ? " " : "");
		}
		if (status[0]) {
			float sw = ImGui::CalcTextSize(status).x;
			ImGui::SameLine(winW - sw - 14.0f);
			ImGui::TextDisabled("%s", status);
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
}

void gui::RenderTeamPanels(const CGame& game) {
	constexpr float kPanelW = 240.0f;
	constexpr float kPad    = 10.0f;
	constexpr ImGuiWindowFlags kFlags =
		ImGuiWindowFlags_NoTitleBar        |
		ImGuiWindowFlags_NoResize          |
		ImGuiWindowFlags_NoScrollbar       |
		ImGuiWindowFlags_AlwaysAutoResize;

	TeamID myTeam = game.localPlayer.ctrl.teamID;

	ImGui::SetNextWindowPos(ImVec2(kPad, kPad), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(kPanelW, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin("##enemies", nullptr, kFlags);

	ImGui::SetCursorPosX((kPanelW - ImGui::CalcTextSize("ENEMIES").x) * 0.5f);
	ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "ENEMIES");
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	for (int i = 0; i < MAX_ENTITIES; i++) {
		const CPlayer& p = game.players[i];
		if (!p.controllerBase || p.ctrl.teamID < TEAM_T || p.ctrl.teamID == myTeam || p.pawn.health == 0) continue;
		bool isCarrier = game.bomb.isCarried && game.bomb.carrierSlot == i;
		renderPlayerEntry(p, game, true, isCarrier);
	}
	ImGui::End();
}

// ─── Bomb Panel ───────────────────────────────────────────────────────────────

void gui::RenderBombPanel(const CGame& game) {
	const C_PlantedC4& b = game.bomb;

	// Only show when a bomb entity is live; both teams get the timer/site —
	// useful for T rotates and post-plant peeks, not just CT defuse calls.
	if (!b.entity) return;

	constexpr float kPanelW = 200.0f;
	constexpr float kPad    = 10.0f;
	constexpr ImGuiWindowFlags kFlags =
		ImGuiWindowFlags_NoTitleBar        |
		ImGuiWindowFlags_NoResize          |
		ImGuiWindowFlags_NoScrollbar       |
		ImGuiWindowFlags_AlwaysAutoResize;

	// Initial anchor at bottom-left (FirstUseEver — once dragged, ImGui
	// persists the chosen position to the ini file).
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(kPad, io.DisplaySize.y - kPad), ImGuiCond_FirstUseEver, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(kPanelW, 0.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin("##bomb", nullptr, kFlags);

	// Title
	ImGui::SetCursorPosX((kPanelW - ImGui::CalcTextSize("BOMB").x) * 0.5f);
	ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "BOMB");
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (b.hasDefused) {
		// Defused
		ImGui::SetCursorPosX((kPanelW - ImGui::CalcTextSize("DEFUSED").x) * 0.5f);
		ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "DEFUSED");

	} else if (b.hasExploded) {
		// Exploded
		ImGui::SetCursorPosX((kPanelW - ImGui::CalcTextSize("EXPLODED").x) * 0.5f);
		ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "EXPLODED");

	} else if (b.entity) {
		// Planted — show site + countdown
		const char* siteStr = (b.site == 0) ? "A" : (b.site == 1) ? "B" : "?";
		char plantBuf[24];
		snprintf(plantBuf, sizeof(plantBuf), "PLANTED - %s", siteStr);
		ImGui::SetCursorPosX((kPanelW - ImGui::CalcTextSize(plantBuf).x) * 0.5f);
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", plantBuf);

		// Timer — turns red below 10 seconds
		float   rem = b.timeRemaining();
		ImVec4  timerCol = (rem > 10.0f) ? ImVec4(1.0f, 0.9f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
		char    timerBuf[12];
		snprintf(timerBuf, sizeof(timerBuf), "%.1fs", rem);
		ImGui::SetCursorPosX((kPanelW - ImGui::CalcTextSize(timerBuf).x) * 0.5f);
		ImGui::TextColored(timerCol, "%s", timerBuf);

		if (b.isBeingDefused) {
			constexpr const char* kDefusing = "DEFUSING";
			ImGui::SetCursorPosX((kPanelW - ImGui::CalcTextSize(kDefusing).x) * 0.5f);
			ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), kDefusing);
		}

	}

	ImGui::Spacing();
	ImGui::End();
}
