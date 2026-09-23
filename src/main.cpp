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

int main(int argc, char* argv[])
{
	bool tracyMode = false;
	int  tracyDuration = 30;

	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--tracy") == 0)
		{
			tracyMode = true;
			if (i + 1 < argc)
			{
				int d = atoi(argv[i + 1]);
				if (d > 0) { tracyDuration = d; ++i; }
			}
		}
	}

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

	if (tracyMode)
	{
		Log::Info("Tracy profiling enabled ({}s capture)", tracyDuration);
		if (!Bootstrap::EnsureTracyTools()) {
			Log::Error("Could not obtain Tracy tools; continuing without profiling");
			tracyMode = false;
		}
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

	if (!Bootstrap::EnsureTextures()) {
		Log::Warn("Texture bootstrap failed; maps/icons may be missing");
	}

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

	HANDLE hTracyProcess = nullptr;
	HANDLE hTracyStderrRead = nullptr;
	if (tracyMode)
	{
		namespace fs = std::filesystem;
		wchar_t exePath[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		fs::path exeDir = fs::path(exePath).parent_path();
		fs::path traceFile = exeDir / L"profile.tracy";

		std::error_code ec;
		fs::remove(traceFile, ec);

		std::wstring cmd = L"\"" + (exeDir / L"tracy-capture.exe").wstring()
			+ L"\" -a 127.0.0.1 -o \"" + traceFile.wstring()
			+ L"\" -s " + std::to_wstring(tracyDuration);

		SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
		HANDLE hStderrWrite = nullptr;
		CreatePipe(&hTracyStderrRead, &hStderrWrite, &sa, 0);
		SetHandleInformation(hTracyStderrRead, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW si{ sizeof(si) };
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdOutput = hStderrWrite;
		si.hStdError = hStderrWrite;
		PROCESS_INFORMATION pi{};
		std::wstring mutableCmd = cmd;
		if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		{
			hTracyProcess = pi.hProcess;
			CloseHandle(pi.hThread);
			Log::Info("[Tracy] Capture process started (PID {})", pi.dwProcessId);
		}
		else
		{
			Log::Error("[Tracy] Failed to start tracy-capture");
			tracyMode = false;
		}
		CloseHandle(hStderrWrite);
	}

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

	if (tracyMode && hTracyProcess)
	{
		Log::Info("[Tracy] Waiting for capture to finish...");
		WaitForSingleObject(hTracyProcess, 60000);

		DWORD exitCode = 0;
		GetExitCodeProcess(hTracyProcess, &exitCode);
		CloseHandle(hTracyProcess);

		if (hTracyStderrRead)
		{
			std::string captureOutput;
			char buf[1024];
			DWORD bytesRead;
			while (ReadFile(hTracyStderrRead, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0)
				captureOutput.append(buf, bytesRead);
			CloseHandle(hTracyStderrRead);
			hTracyStderrRead = nullptr;

			if (!captureOutput.empty())
			{
				while (!captureOutput.empty() && (captureOutput.back() == '\n' || captureOutput.back() == '\r'))
					captureOutput.pop_back();
				Log::Info("[Tracy] capture output: {}", captureOutput);
			}
		}

		if (exitCode != 0)
			Log::Warn("[Tracy] tracy-capture exited with code {}", exitCode);

		Bootstrap::RunTracySession(tracyDuration);
	}
	if (hTracyStderrRead) CloseHandle(hTracyStderrRead);

	delete g_GameContext;
	g_GameContext = nullptr;

	gui::Cleanup();

	Log::Info("Exited cleanly");

	return 0;
}