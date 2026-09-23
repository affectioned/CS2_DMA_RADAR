#pragma once
#include "sdk.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_internal.h"
#include "WICTextureLoader.h"
#include <d3d11.h>
#pragma comment(lib, "DirectXTK")



extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace gui {
	// Data
	extern ID3D11Device* g_pd3dDevice;
	extern ID3D11DeviceContext* g_pd3dDeviceContext;
	extern IDXGISwapChain* g_pSwapChain;
	extern UINT g_ResizeWidth, g_ResizeHeight;
	extern ID3D11RenderTargetView* g_mainRenderTargetView;
	// Border-address sampler used only by the rotating radar so corners that
	// fall outside the radar texture render as transparent black instead of
	// the WRAP-mode tiling that ImGui's default font sampler produces.
	extern ID3D11SamplerState* g_pRadarSampler;
	extern bool exitRequested;
	extern WNDCLASSEXW wc;
	extern HWND hwnd;

	// Lifecycle stages (called from main)
	void CreateAppWindow();
	bool InitD3D();
	void ShowAppWindow();
	void InitImGui();
	void OnFrame(CGame& game, std::mutex& gameMutex);
	void Cleanup();

	// D3D helpers (dx11.cpp)
	bool CreateDeviceD3D(HWND);
	void CleanupDeviceD3D();
	void CreateRenderTarget();
	void CleanupRenderTarget();
	ID3D11ShaderResourceView* LoadImageTexture(ID3D11Device* device, const wchar_t* filename);
	ID3D11ShaderResourceView* LoadImageTexture(ID3D11Device* device, const wchar_t* filename, int* outWidth, int* outHeight);
	LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);

	// Control panel (gui.cpp)
	void RenderControlPanel();
	void RenderTeamPanels(const CGame&);
	void RenderBombPanel(const CGame&);

	// Per-frame radar state. Computed once at the top of gameLoop() so the
	// hot path (worldToRadar called per-entity per-frame) doesn't recompute
	// trig, the map-bounds lookup, or per-frame derived constants.
	struct RadarFrame {
		const struct mapData* bounds;   // null if map not in mapBounds
		bool                  rotates;
		// Player's screen-space center within the map window.
		// Static mode: (radarSize/2, radarSize/2) inside the centered square window.
		// Rotated mode: (display.x/2, display.y/2) — window covers the full display
		// and the local player sits at the center of the screen.
		float                 halfX, halfY;
		// Static: radarSize / (1024 * scale).
		// Rotated: screen diagonal / (1024 * scale) — the smallest zoom that lets
		// a rotating WxH viewport fit inside the square map texture.
		float                 pxPerWorld;
		// yawDeg is the smoothed render-space yaw (not the raw eyeAngles.y),
		// so other render code that needs to align with the rotated map must
		// read this rather than re-fetching eyeAngles.y.
		float                 yawDeg;
		float                 sinYaw, cosYaw;
		float                 localX, localY;

		static RadarFrame compute(const CGame&);
	};

	// Game rendering (render.cpp)
	void gameLoop(const CGame&);
	void renderMap(ID3D11ShaderResourceView*, const RadarFrame&);
	void renderPlayers(const CGame&, const RadarFrame&);
	void renderBomb(const CGame&, const RadarFrame&);

	// Resource loading (resources.cpp)
	void loadMapBounds();
	void loadTextures();

	// Utilities (render.cpp)
	ImU32 setColor(DWORD color, float opacity);
	void worldToRadar(float& x, float& y, const RadarFrame&);
	float setOpacity(float, float, const CGame&);
}

namespace maps {
	extern float radarSize;
	extern std::unordered_map<std::string, ID3D11ShaderResourceView*> mapTextures;
	extern std::unordered_map<std::string, mapData> mapBounds;
}

namespace icons {
	extern std::unordered_map<int, ID3D11ShaderResourceView*> iconTextures;
	extern std::unordered_map<int, int> iconWidths;
	extern std::unordered_map<int, int> iconHeights;
}

namespace settings {
	// Per-team overlay toggles. The 'Enemies' flag controls overlay on players
	// whose teamID differs from the local player's; 'Friendlies' covers the
	// rest (own team + the local player). Each overlay element checks the
	// right flag based on whether it's enemy or friendly.
	extern bool  showAimLinesEnemies,    showAimLinesFriendlies;
	extern bool  showWeaponIconsEnemies, showWeaponIconsFriendlies;
	extern bool  showHealthBarsEnemies,  showHealthBarsFriendlies;
	extern bool  showPlayerNamesEnemies, showPlayerNamesFriendlies;

	extern bool  showTeamPanels;
	extern bool  rotateRadar;
	extern float iconScale;
	extern float aimLineLength;
	extern float dotRadius;
	// Multiplier on rotated-mode pxPerWorld. 1.0 = baseline (map covers screen
	// diagonal, no off-map corners); >1 zooms in tighter; <1 widens the view
	// at the cost of borders re-appearing. No effect in static mode.
	extern float radarZoom;
}
