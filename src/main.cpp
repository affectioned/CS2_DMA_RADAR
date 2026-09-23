#include "pch.h"
#include <filesystem>
#include <future>
#include "gui.h"
#include "sdk.h"
#include "updater.h"
#include "CS2Context.h"
#include "DMA/DMA Thread.h"
#include "DMA/bootstrap/Bootstrap.h"

std::atomic<bool> bRunning{ true };

int main()
{
	{
		wchar_t exePath[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		auto logPath = std::filesystem::path(exePath).parent_path() / "cs2radar.log";
		Log::Init(logPath.wstring());
	}

	Log::Info("Starting CS2 Radar");

	if (!Bootstrap::EnsureRuntimeDlls()) {
		Log::Error("Required runtime DLLs could not be obtained; aborting");
		return 1;
	}

	gui::CreateAppWindow();

	Log::Info("Initializing Direct3D");
	if (!gui::InitD3D()) {
		Log::Error("Failed to initialize Direct3D, aborting");
		gui::Cleanup();
		return 1;
	}

	gui::ShowAppWindow();
	gui::InitImGui();

	Log::Info("Loading map bounds and textures");
	gui::loadMapBounds();
	gui::loadTextures();

	{
		auto classFut  = std::async(std::launch::async, updater::fetchClassOffsets);
		auto moduleFut = std::async(std::launch::async, updater::fetchModuleOffsets);
		classFut.wait();
		moduleFut.wait();
	}

	Log::Info("Starting acquisition thread");
	auto       game = std::make_unique<CGame>();
	std::mutex gameMutex;
	g_GameContext = new CS2Context(*game, gameMutex);
	std::thread DMAThread(DMA_Thread_Main);

	Log::Info("Render loop running - press END to exit");
	while (bRunning)
	{
		if (GetAsyncKeyState(VK_END) & 0x1) {
			Log::Info("Exit: VK_END pressed");
			bRunning = false;
		}
		gui::OnFrame(*game, gameMutex);
	}

	DMAThread.join();

	delete g_GameContext;
	g_GameContext = nullptr;

	gui::Cleanup();

	Log::Info("Exited cleanly");

	return 0;
}