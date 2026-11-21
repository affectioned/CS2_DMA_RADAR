#include "pch.h"
#include "gui.h"
#include "sdk.h"
#include "updater.h"
#include "share/ShareServer.h"
#include "share/Tunnel.h"

extern std::atomic<bool> bRunning;

// Share lifecycle. Owned by gui.cpp because ControlPanel is the only place
// settings::shareEnabled changes; OnFrame reconciles the live objects with
// the settings state once per frame.
namespace gui {
    static std::unique_ptr<ShareServer> s_Share;
    static std::unique_ptr<Tunnel>      s_Tunnel;
}

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
	bool  shareEnabled           = false;
	bool  sharePublic            = false;
	int   sharePort              = 8787;
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
		{ "ShareEnabled",          &settings::shareEnabled           },
		{ "SharePublic",           &settings::sharePublic            },
	};
	struct IntKey { const char* key; int* val; };
	static const IntKey kIntKeys[] = {
		{ "SharePort", &settings::sharePort },
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
		for (const auto& k : kIntKeys) {
			char fmt[64];
			snprintf(fmt, sizeof(fmt), "%s=%%d", k.key);
			int i;
			if (sscanf_s(line, fmt, &i) == 1) { *k.val = i; return; }
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
		for (const auto& k : kIntKeys)   buf->appendf("%s=%d\n",   k.key, *k.val);
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
	style.WindowRounding    = 10.0f;
	style.ChildRounding     = 8.0f;
	style.FrameRounding     = 6.0f;
	style.PopupRounding     = 8.0f;
	style.GrabRounding      = 6.0f;
	style.ScrollbarRounding = 6.0f;
	style.TabRounding       = 6.0f;
	style.WindowBorderSize  = 1.0f;
	style.FrameBorderSize   = 0.0f;
	style.ItemSpacing       = ImVec2(10.0f, 8.0f);
	style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
	style.WindowPadding     = ImVec2(16.0f, 14.0f);
	style.FramePadding      = ImVec2(10.0f, 5.0f);
	style.IndentSpacing     = 18.0f;
	style.SeparatorTextBorderSize = 1.0f;

	// Palette: Linear/Vercel-class 2026 dark — near-black with cool blue tint,
	// one vibrant electric-blue accent (#7C9DFF). Hover tiers rise in 2% steps,
	// matching how modern dashboards treat state without any color shift.
	ImVec4* c = style.Colors;
	const ImVec4 bg      = ImVec4(0.039f, 0.047f, 0.063f, 0.96f); // #0A0C10
	const ImVec4 bgCard  = ImVec4(0.067f, 0.078f, 0.102f, 1.00f); // #11141A
	const ImVec4 bgHi    = ImVec4(0.098f, 0.118f, 0.153f, 1.00f); // #191E27
	const ImVec4 bgHover = ImVec4(0.125f, 0.145f, 0.188f, 1.00f);
	const ImVec4 txt     = ImVec4(0.902f, 0.914f, 0.941f, 1.00f); // #E6E9F0
	const ImVec4 txtDim  = ImVec4(0.420f, 0.463f, 0.518f, 1.00f); // #6B7684
	const ImVec4 border  = ImVec4(1.000f, 1.000f, 1.000f, 0.06f);
	const ImVec4 line    = ImVec4(1.000f, 1.000f, 1.000f, 0.07f);
	const ImVec4 accent  = ImVec4(0.486f, 0.616f, 1.000f, 1.00f); // #7C9DFF

	c[ImGuiCol_WindowBg]           = bg;
	c[ImGuiCol_ChildBg]            = ImVec4(0,0,0,0);
	c[ImGuiCol_PopupBg]            = ImVec4(0.039f, 0.047f, 0.063f, 0.98f);
	c[ImGuiCol_Border]             = border;
	c[ImGuiCol_Text]               = txt;
	c[ImGuiCol_TextDisabled]       = txtDim;
	c[ImGuiCol_Separator]          = line;
	c[ImGuiCol_SeparatorHovered]   = ImVec4(accent.x, accent.y, accent.z, 0.40f);
	c[ImGuiCol_SeparatorActive]    = ImVec4(accent.x, accent.y, accent.z, 0.70f);
	c[ImGuiCol_Button]             = bgCard;
	c[ImGuiCol_ButtonHovered]      = bgHover;
	c[ImGuiCol_ButtonActive]       = bgHi;
	c[ImGuiCol_FrameBg]            = ImVec4(0.055f, 0.067f, 0.090f, 1.00f);
	c[ImGuiCol_FrameBgHovered]     = bgHover;
	c[ImGuiCol_FrameBgActive]      = bgHi;
	c[ImGuiCol_Header]             = ImVec4(accent.x, accent.y, accent.z, 0.08f);
	c[ImGuiCol_HeaderHovered]      = ImVec4(accent.x, accent.y, accent.z, 0.14f);
	c[ImGuiCol_HeaderActive]       = ImVec4(accent.x, accent.y, accent.z, 0.22f);
	c[ImGuiCol_CheckMark]          = accent;
	c[ImGuiCol_SliderGrab]         = ImVec4(accent.x, accent.y, accent.z, 0.85f);
	c[ImGuiCol_SliderGrabActive]   = accent;
	c[ImGuiCol_ScrollbarBg]        = ImVec4(0,0,0,0);
	c[ImGuiCol_ScrollbarGrab]      = ImVec4(1,1,1, 0.10f);
	c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1,1,1, 0.18f);
	c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1,1,1, 0.28f);
	c[ImGuiCol_Tab]                = bgCard;
	c[ImGuiCol_TabHovered]         = bgHover;
	c[ImGuiCol_TabActive]          = ImVec4(accent.x, accent.y, accent.z, 0.22f);
	c[ImGuiCol_TableHeaderBg]      = ImVec4(0,0,0,0);
	c[ImGuiCol_TableBorderLight]   = line;
	c[ImGuiCol_TableBorderStrong]  = border;

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
	Log::Info("[GUI]: ImGui {} initialized", IMGUI_VERSION);
}

// Reconcile the actual ShareServer/Tunnel instances with the user's current
// settings. Run once per frame; cheap when nothing changed (just two
// pointer checks). Owns both objects for the lifetime of the GUI.
static void reconcileShare(CGame& game, std::mutex& gameMutex) {
	using gui::s_Share;
	using gui::s_Tunnel;

	if (settings::shareEnabled) {
		if (!s_Share) {
			s_Share = std::make_unique<ShareServer>(game, gameMutex);
			if (!s_Share->Start(static_cast<uint16_t>(settings::sharePort))) {
				s_Share.reset();
				settings::shareEnabled = false;      // revert the toggle on failure
				settings::sharePublic  = false;
			}
		}
		if (settings::sharePublic) {
			if (!s_Tunnel) s_Tunnel = std::make_unique<Tunnel>();
			if (s_Share && !s_Tunnel->IsRunning() && s_Tunnel->LastError().empty())
				if (!s_Tunnel->Start(static_cast<uint16_t>(settings::sharePort)))
					settings::sharePublic = false;
		} else if (s_Tunnel) {
			s_Tunnel->Stop();
			s_Tunnel.reset();
		}
	} else {
		if (s_Tunnel) { s_Tunnel->Stop(); s_Tunnel.reset(); }
		if (s_Share)  { s_Share->Stop();  s_Share.reset();  }
		settings::sharePublic = false;
	}
}

void gui::OnFrame(CGame& game, std::mutex& gameMutex) {
	ZoneScoped;
	reconcileShare(game, gameMutex);

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
	if (s_Tunnel) { s_Tunnel->Stop(); s_Tunnel.reset(); }
	if (s_Share)  { s_Share->Stop();  s_Share.reset();  }
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

// ─── Modern UI primitives ─────────────────────────────────────────────────────

namespace {
	constexpr ImU32 kAccent    = IM_COL32(124, 157, 255, 255);  // #7C9DFF
	constexpr ImU32 kAccentSub = IM_COL32(124, 157, 255,  60);
	constexpr ImU32 kTxt       = IM_COL32(230, 233, 240, 255);
	constexpr ImU32 kTxtDim    = IM_COL32(107, 118, 132, 255);
	constexpr ImU32 kTxtMute   = IM_COL32( 80,  88,  99, 255);
	constexpr ImU32 kDanger    = IM_COL32(255,  95,  95, 255);
	constexpr ImU32 kWarn      = IM_COL32(255, 180,  65, 255);
	constexpr ImU32 kGood      = IM_COL32( 85, 215, 130, 255);
	constexpr ImU32 kTColor    = IM_COL32(255, 180,  65, 255);
	constexpr ImU32 kCTColor   = IM_COL32( 90, 170, 255, 255);

	// Thin left accent bar + small-caps title. Used between sections to give
	// the panel the Linear/Raycast "section rail" look without any borders.
	void SectionTitle(const char* label, ImU32 color = kAccent) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		const float rail_h = ImGui::GetTextLineHeight() * 0.85f;
		const float rail_w = 2.5f;
		const float rail_y = p.y + (ImGui::GetTextLineHeight() - rail_h) * 0.5f;
		dl->AddRectFilled({ p.x, rail_y }, { p.x + rail_w, rail_y + rail_h }, color, 1.5f);

		ImGui::SetCursorScreenPos({ p.x + rail_w + 8.0f, p.y });
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.46f, 0.52f, 1.0f));
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();
		ImGui::Spacing();
	}

	// Pill chip — rounded little badge. Draws at cursor, returns advance.
	void PillBadge(const char* text, ImU32 fg, ImU32 bg) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 ts = ImGui::CalcTextSize(text);
		ImVec2 p  = ImGui::GetCursorScreenPos();
		const float padX = 7.0f, padY = 2.0f;
		ImVec2 rmax = { p.x + ts.x + padX*2, p.y + ts.y + padY*2 };
		dl->AddRectFilled(p, rmax, bg, (ts.y + padY*2) * 0.5f);
		dl->AddText({ p.x + padX, p.y + padY }, fg, text);
		ImGui::Dummy({ ts.x + padX*2, ts.y + padY*2 });
	}

	// URL display: subtle inset frame with inline Copy chip at the right edge.
	void UrlRow(const char* url) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float avail = ImGui::GetContentRegionAvail().x;
		float h = ImGui::GetTextLineHeight() + 10.0f;
		ImVec2 p = ImGui::GetCursorScreenPos();
		dl->AddRectFilled(p, { p.x + avail, p.y + h },
		                  IM_COL32(255, 255, 255, 10), 6.0f);
		dl->AddRect(p, { p.x + avail, p.y + h },
		            IM_COL32(255, 255, 255, 14), 6.0f);

		ImVec2 tpos = { p.x + 10.0f, p.y + 5.0f };
		ImVec2 clip_max = { p.x + avail - 54.0f, p.y + h };
		dl->PushClipRect(tpos, clip_max, true);
		dl->AddText(tpos, kTxt, url);
		dl->PopClipRect();

		ImGui::PushID(url);
		ImGui::SetCursorScreenPos({ p.x + avail - 46.0f, p.y + 4.0f });
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 2.0f));
		ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(124, 157, 255, 30));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(124, 157, 255, 70));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(124, 157, 255, 110));
		ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.486f, 0.616f, 1.0f, 1.0f));
		if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(url);
		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar();
		ImGui::PopID();

		ImGui::SetCursorScreenPos({ p.x, p.y + h + 4.0f });
	}
}

void gui::RenderControlPanel() {
	ImGuiIO& io = ImGui::GetIO();
	constexpr float panelW = 240.0f;

	// FirstUseEver on the anchor lets the user drag the panel and have ImGui
	// persist the chosen position via its built-in [Window][##panel] section.
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panelW - 14.0f, 14.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 0.0f), ImVec2(panelW, FLT_MAX));
	ImGui::Begin("##panel", nullptr,
		ImGuiWindowFlags_NoTitleBar        |
		ImGuiWindowFlags_NoResize          |
		ImGuiWindowFlags_NoScrollbar       |
		ImGuiWindowFlags_AlwaysAutoResize
	);

	// ── Brand row: accent dot + wordmark on the left, live FPS on the right.
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float lineH = ImGui::GetTextLineHeight();
		// Pulsing accent dot — tiny, left of the wordmark
		float t = (float)ImGui::GetTime();
		float pulse = 0.65f + 0.35f * sinf(t * 2.0f);
		ImU32 dotCol = ImGui::ColorConvertFloat4ToU32(ImVec4(0.486f, 0.616f, 1.0f, pulse));
		dl->AddCircleFilled({ p.x + 4.0f, p.y + lineH*0.5f + 1.0f }, 3.5f, dotCol);

		ImGui::SetCursorScreenPos({ p.x + 14.0f, p.y });
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.91f, 0.94f, 1.0f));
		ImGui::TextUnformatted("CS2Radar");
		ImGui::PopStyleColor();

		char fpsBuf[16];
		snprintf(fpsBuf, sizeof(fpsBuf), "%.0f", io.Framerate);
		float fpsW = ImGui::CalcTextSize(fpsBuf).x;
		float labelW = ImGui::CalcTextSize("FPS").x;
		ImGui::SameLine(panelW - fpsW - labelW - 28.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.91f, 0.94f, 1.0f));
		ImGui::TextUnformatted(fpsBuf);
		ImGui::PopStyleColor();
		ImGui::SameLine(0, 4.0f);
		ImGui::TextDisabled("FPS");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// ── Exit — ghost button with red hover accent.
	ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(255, 255, 255,  8));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255,  95,  95, 40));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(255,  95,  95, 80));
	if (ImGui::Button("Exit", ImVec2(-1.0f, 28.0f))) {
		Log::Info("Exit: button clicked");
		exitRequested = true;
		bRunning.store(false, std::memory_order_release);
	}
	ImGui::PopStyleColor(3);

	// ── Share
	ImGui::Spacing();
	SectionTitle("SHARE");

	ImGui::Checkbox("Enable sharing", &settings::shareEnabled);
	if (settings::shareEnabled) {
		ImGui::Spacing();
		// OPSEC — this URL broadcasts live DMA state. One red pill makes it
		// visible without a wall of text.
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			float avail = ImGui::GetContentRegionAvail().x;
			ImVec2 p = ImGui::GetCursorScreenPos();
			dl->AddRectFilled(p, { p.x + avail, p.y + 44.0f },
			                  IM_COL32(255,  95,  95,  18), 6.0f);
			dl->AddRect(p, { p.x + avail, p.y + 44.0f },
			            IM_COL32(255,  95,  95,  45), 6.0f);
			dl->AddText({ p.x + 10.0f, p.y + 6.0f },
			            IM_COL32(255, 160, 160, 255), "LIVE EXPOSURE");
			ImGui::SetCursorScreenPos({ p.x + 10.0f, p.y + 22.0f });
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.55f, 0.55f, 1.0f));
			ImGui::PushTextWrapPos(p.x + avail - 10.0f);
			ImGui::TextUnformatted("Anyone with the link sees your match.");
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::SetCursorScreenPos({ p.x, p.y + 50.0f });
		}

		if (s_Share) {
			for (const auto& url : s_Share->Urls())
				UrlRow(url.c_str());
		}

		ImGui::Spacing();
		ImGui::Checkbox("Public tunnel (cloudflared)", &settings::sharePublic);
		if (settings::sharePublic && s_Tunnel) {
			std::string url = s_Tunnel->Url();
			std::string err = s_Tunnel->LastError();
			if (!err.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
				ImGui::TextWrapped("%s", err.c_str());
				ImGui::PopStyleColor();
			} else if (url.empty()) {
				// Tiny loading dot + label
				float t = (float)ImGui::GetTime();
				float pulse = 0.4f + 0.6f * sinf(t * 3.0f);
				ImU32 loadCol = ImGui::ColorConvertFloat4ToU32(ImVec4(0.42f, 0.46f, 0.52f, pulse));
				ImDrawList* dl = ImGui::GetWindowDrawList();
				ImVec2 p = ImGui::GetCursorScreenPos();
				float lineH = ImGui::GetTextLineHeight();
				dl->AddCircleFilled({ p.x + 4.0f, p.y + lineH*0.5f + 1.0f }, 3.0f, loadCol);
				ImGui::SetCursorScreenPos({ p.x + 14.0f, p.y });
				ImGui::TextDisabled("waiting for cloudflared");
			} else {
				UrlRow(url.c_str());
			}
		}
	}

	// ── Settings
	ImGui::Spacing();
	ImGui::Spacing();
	SectionTitle("OVERLAY");

	if (ImGui::BeginTable("##overlay", 3,
	    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
	{
		ImGui::TableSetupColumn("");
		ImGui::TableSetupColumn("Enemies");
		ImGui::TableSetupColumn("Allies");
		ImGui::TableHeadersRow();

		auto row = [](const char* label, bool* enem, bool* friendly,
		              const char* idEnem, const char* idFriend) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.46f, 0.52f, 1.0f));
			ImGui::TextUnformatted(label);
			ImGui::PopStyleColor();
			ImGui::TableNextColumn(); ImGui::Checkbox(idEnem,   enem);
			ImGui::TableNextColumn(); ImGui::Checkbox(idFriend, friendly);
		};

		row("Aim",    &settings::showAimLinesEnemies,    &settings::showAimLinesFriendlies,    "##aim_e",  "##aim_f");
		row("Weapon", &settings::showWeaponIconsEnemies, &settings::showWeaponIconsFriendlies, "##wpn_e",  "##wpn_f");
		row("Health", &settings::showHealthBarsEnemies,  &settings::showHealthBarsFriendlies,  "##hp_e",   "##hp_f");
		row("Name",   &settings::showPlayerNamesEnemies, &settings::showPlayerNamesFriendlies, "##name_e", "##name_f");
		ImGui::EndTable();
	}

	ImGui::Spacing();
	ImGui::Checkbox("Team panels",  &settings::showTeamPanels);
	ImGui::Checkbox("Rotate radar", &settings::rotateRadar);

	ImGui::Spacing();
	ImGui::Spacing();
	SectionTitle("TUNING");

	ImGui::SetNextItemWidth(-1.0f);
	ImGui::SliderFloat("##iconscale", &settings::iconScale,    0.05f, 0.50f,  "Icons  %.2fx");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::SliderFloat("##aimlength", &settings::aimLineLength, 10.0f, 100.0f, "Aim    %.0fpx");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::SliderFloat("##dotradius", &settings::dotRadius,     3.0f,  14.0f,  "Dot    %.1fpx");
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::SliderFloat("##zoom",      &settings::radarZoom,    0.5f,  3.0f,   "Zoom   %.2fx");

	ImGui::Spacing();

	ImGui::End();
}

// ─── Team Panels ──────────────────────────────────────────────────────────────

static void renderPlayerEntry(const CPlayer& p, const CGame& /*game*/, bool isEnemy, bool isCarrier = false) {
	constexpr float kDotR   = 4.5f;
	constexpr float kIndent = kDotR * 2.0f + 12.0f;
	constexpr float kBarH   = 2.5f;
	constexpr float kIconH  = 14.0f;

	bool        alive  = p.pawn.lifeState == 0;
	float       alpha  = alive ? 1.0f : 0.35f;
	ImDrawList* dl     = ImGui::GetWindowDrawList();
	ImVec2      origin = ImGui::GetCursorScreenPos();
	float       lineH  = ImGui::GetTextLineHeight();
	float       winW   = ImGui::GetWindowWidth();

	// Team-coloured dot — team colour for friendlies via ctrl.color; for enemies
	// the team itself (T=orange, CT=blue) instead of a blanket red, so the row
	// reads at a glance which team the enemy is on.
	ImU32 dotCol;
	if (!isEnemy) {
		dotCol = gui::setColor(p.ctrl.color, alpha * 255.0f);
	} else {
		const ImU32 base = (p.ctrl.teamID == 2) ? kTColor : kCTColor;
		dotCol = (base & 0x00FFFFFF) | ((uint32_t)(alpha * 255.0f) << 24);
	}

	// Soft glow halo under the dot (alive only) so the row has a tiny spark.
	if (alive) {
		for (int i = 3; i >= 1; --i) {
			ImU32 halo = (dotCol & 0x00FFFFFF) | ((uint32_t)(18 / i) << 24);
			dl->AddCircleFilled({ origin.x + kDotR + 2.0f, origin.y + lineH * 0.5f + 1.0f },
			                    kDotR + i * 1.2f, halo);
		}
	}
	dl->AddCircleFilled({ origin.x + kDotR + 2.0f, origin.y + lineH * 0.5f + 1.0f }, kDotR, dotCol);

	// Name (left) + HP number (right, tabular numerals)
	char hpBuf[8];
	if (alive) snprintf(hpBuf, sizeof(hpBuf), "%d", p.pawn.health);
	else       snprintf(hpBuf, sizeof(hpBuf), "---");

	float hpTextW  = ImGui::CalcTextSize(hpBuf).x;
	float nameMaxW = winW - kIndent - hpTextW - 20.0f;

	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kIndent);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.902f, 0.914f, 0.941f, alpha));

	ImVec2 nameScreen = ImGui::GetCursorScreenPos();
	ImGui::PushClipRect(nameScreen, { nameScreen.x + nameMaxW, nameScreen.y + lineH + 2.0f }, true);
	ImGui::TextUnformatted(p.ctrl.name[0] ? p.ctrl.name : "...");
	ImGui::PopClipRect();

	ImGui::SameLine(winW - hpTextW - 14.0f);
	if (alive) ImGui::Text("%d", p.pawn.health);
	else       ImGui::TextDisabled("---");
	ImGui::PopStyleColor();

	// Health bar — slimmer, rounded pill, more subtle track.
	float barStartX = origin.x + kIndent;
	float barW      = winW - kIndent - 14.0f;
	ImVec2 barTL    = { barStartX, ImGui::GetCursorScreenPos().y + 1.0f };
	float filled    = alive ? (float)p.pawn.health / 100.0f : 0.0f;
	if (filled > 1.0f) filled = 1.0f;
	// Low-HP → red, high-HP → green via HSL-like hue sweep.
	float r = 255.0f * (1.0f - filled * 0.85f);
	float g = 180.0f + 75.0f * filled;
	ImU32 barBg = IM_COL32(255, 255, 255, (int)(alpha * 20));
	ImU32 barFg = IM_COL32((int)r, (int)g, 80, (int)(alpha * 230));
	dl->AddRectFilled(barTL, { barTL.x + barW,          barTL.y + kBarH }, barBg, kBarH * 0.5f);
	if (filled > 0.001f)
		dl->AddRectFilled(barTL, { barTL.x + barW * filled, barTL.y + kBarH }, barFg, kBarH * 0.5f);
	ImGui::Dummy({ 0.0f, kBarH + 4.0f });

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
		float labelW = ImGui::CalcTextSize(kLabel).x + 14.0f;
		ImGui::SameLine(winW - labelW - 14.0f);
		PillBadge(kLabel, IM_COL32(255, 180, 65, 255), IM_COL32(255, 180, 65, 30));
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

	ImGui::Dummy({ 0.0f, 4.0f });
	ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.045f));
	ImGui::Separator();
	ImGui::PopStyleColor();
	ImGui::Spacing();
}

void gui::RenderTeamPanels(const CGame& game) {
	constexpr float kPanelW = 260.0f;
	constexpr float kPad    = 14.0f;
	constexpr ImGuiWindowFlags kFlags =
		ImGuiWindowFlags_NoTitleBar        |
		ImGuiWindowFlags_NoResize          |
		ImGuiWindowFlags_NoScrollbar       |
		ImGuiWindowFlags_AlwaysAutoResize;

	TeamID myTeam = game.localPlayer.ctrl.teamID;

	// Count alive enemies for the header pill.
	int aliveCount = 0, totalCount = 0;
	for (int i = 0; i < MAX_ENTITIES; i++) {
		const CPlayer& p = game.players[i];
		if (!p.controllerBase || p.ctrl.teamID < TEAM_T || p.ctrl.teamID == myTeam) continue;
		totalCount++;
		if (p.pawn.health > 0) aliveCount++;
	}

	ImGui::SetNextWindowPos(ImVec2(kPad, kPad), ImGuiCond_FirstUseEver);
	// Pin width so AlwaysAutoResize can only grow the panel vertically.
	// Without this, a transient wide item on first frame (or anything that
	// accidentally extends the content region) latches the window open at
	// full-screen width and never shrinks back.
	ImGui::SetNextWindowSizeConstraints(ImVec2(kPanelW, 0.0f), ImVec2(kPanelW, FLT_MAX));
	ImGui::Begin("##enemies", nullptr, kFlags);

	// Header row — accent rail + title + alive/total counter on the right.
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float lineH = ImGui::GetTextLineHeight();
		dl->AddRectFilled({ p.x, p.y + 2.0f }, { p.x + 2.5f, p.y + lineH - 1.0f },
		                  IM_COL32(255, 95, 95, 255), 1.5f);
		ImGui::SetCursorScreenPos({ p.x + 11.0f, p.y });
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.46f, 0.52f, 1.0f));
		ImGui::TextUnformatted("ENEMIES");
		ImGui::PopStyleColor();

		char buf[16];
		snprintf(buf, sizeof(buf), "%d / %d", aliveCount, totalCount);
		float bw = ImGui::CalcTextSize(buf).x;
		ImGui::SameLine(kPanelW - bw - 32.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.902f, 0.914f, 0.941f, 1.0f));
		ImGui::TextUnformatted(buf);
		ImGui::PopStyleColor();
	}

	ImGui::Dummy({ 0.0f, 6.0f });
	ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.045f));
	ImGui::Separator();
	ImGui::PopStyleColor();
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

	constexpr float kPanelW = 220.0f;
	constexpr float kPad    = 14.0f;
	constexpr ImGuiWindowFlags kFlags =
		ImGuiWindowFlags_NoTitleBar        |
		ImGuiWindowFlags_NoResize          |
		ImGuiWindowFlags_NoScrollbar       |
		ImGuiWindowFlags_AlwaysAutoResize;

	// Initial anchor at bottom-left (FirstUseEver — once dragged, ImGui
	// persists the chosen position to the ini file).
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(kPad, io.DisplaySize.y - kPad), ImGuiCond_FirstUseEver, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(kPanelW, 0.0f), ImVec2(kPanelW, FLT_MAX));
	ImGui::Begin("##bomb", nullptr, kFlags);

	// Section header: accent rail + "BOMB" + site pill on the right.
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		float lineH = ImGui::GetTextLineHeight();
		dl->AddRectFilled({ p.x, p.y + 2.0f }, { p.x + 2.5f, p.y + lineH - 1.0f },
		                  IM_COL32(255, 180, 65, 255), 1.5f);
		ImGui::SetCursorScreenPos({ p.x + 11.0f, p.y });
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.46f, 0.52f, 1.0f));
		ImGui::TextUnformatted("BOMB");
		ImGui::PopStyleColor();

		if (b.entity && !b.hasDefused && !b.hasExploded) {
			const char* siteStr = (b.site == 0) ? "SITE A" : (b.site == 1) ? "SITE B" : "SITE ?";
			ImVec2 ts = ImGui::CalcTextSize(siteStr);
			ImGui::SameLine(kPanelW - ts.x - 32.0f);
			PillBadge(siteStr, IM_COL32(255, 180, 65, 255), IM_COL32(255, 180, 65, 30));
		}
	}

	ImGui::Dummy({ 0.0f, 6.0f });
	ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.045f));
	ImGui::Separator();
	ImGui::PopStyleColor();
	ImGui::Spacing();

	if (b.hasDefused) {
		ImGui::SetCursorPosX((kPanelW - ImGui::CalcTextSize("DEFUSED").x) * 0.5f);
		ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.5f, 1.0f), "DEFUSED");

	} else if (b.hasExploded) {
		ImGui::SetCursorPosX((kPanelW - ImGui::CalcTextSize("EXPLODED").x) * 0.5f);
		ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "EXPLODED");

	} else if (b.entity) {
		float   rem    = b.timeRemaining();
		bool    danger = rem <= 10.0f;
		ImVec4  timerCol = danger
			? ImVec4(1.0f, 0.37f, 0.37f, 1.0f)
			: ImVec4(1.0f, 0.82f, 0.30f, 1.0f);

		// Big countdown — centered, no pulse on the text itself so it stays
		// readable; low-time emphasis comes from the colour and the bar below.
		char timerBuf[12];
		snprintf(timerBuf, sizeof(timerBuf), "%.1fs", rem);
		ImVec2 ts = ImGui::CalcTextSize(timerBuf);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		// subtle glow halo behind the number
		ImU32 halo = ImGui::ColorConvertFloat4ToU32(ImVec4(timerCol.x, timerCol.y, timerCol.z, 0.14f));
		dl->AddRectFilled({ p.x + 4.0f, p.y + 2.0f },
		                  { p.x + kPanelW - 36.0f, p.y + ts.y + 10.0f },
		                  halo, 10.0f);
		ImGui::SetCursorScreenPos({ p.x + (kPanelW - ts.x) * 0.5f - 16.0f, p.y + 5.0f });
		ImGui::PushStyleColor(ImGuiCol_Text, timerCol);
		ImGui::TextUnformatted(timerBuf);
		ImGui::PopStyleColor();
		ImGui::Dummy({ 0.0f, 6.0f });

		// Thin countdown bar — proportional to remaining / 40s (full plant).
		constexpr float kBombMax = 40.0f;
		float progress = rem / kBombMax;
		if (progress < 0.0f) progress = 0.0f;
		if (progress > 1.0f) progress = 1.0f;
		ImVec2 bp = ImGui::GetCursorScreenPos();
		float bw = kPanelW - 32.0f;
		dl->AddRectFilled(bp, { bp.x + bw, bp.y + 3.0f },
		                  IM_COL32(255, 255, 255, 18), 1.5f);
		dl->AddRectFilled(bp, { bp.x + bw * progress, bp.y + 3.0f },
		                  ImGui::ColorConvertFloat4ToU32(timerCol), 1.5f);
		ImGui::Dummy({ 0.0f, 7.0f });

		if (b.isBeingDefused) {
			constexpr const char* kDefusing = "DEFUSING";
			float pw = ImGui::CalcTextSize(kDefusing).x + 14.0f;
			ImGui::SetCursorPosX((kPanelW - pw) * 0.5f);
			PillBadge(kDefusing, IM_COL32(110, 220, 255, 255), IM_COL32(110, 220, 255, 30));
		}
	}

	ImGui::Spacing();
	ImGui::End();
}
