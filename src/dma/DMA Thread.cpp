#include "pch.h"

#include "DMA Thread.h"
#include "Input/Input Manager.h"
#include "IGameContext.h"

#pragma comment(lib, "Winmm.lib")

IGameContext* g_GameContext = nullptr;

extern std::atomic<bool> bRunning;

void DMA_Thread_Main()
{
	Log::Info("[AcqThread]: Started.");

	DMA_Connection* conn = DMA_Connection::GetInstance();

	c_keys::InitKeyboard(conn);

	if (!g_GameContext)
	{
		Log::Error("[AcqThread]: No game context, requesting exit.");
		bRunning = false;
		return;
	}

	timeBeginPeriod(1);

	// Outer loop: attach → tick → detach cycle. Lets the DMA thread survive
	// a game restart without exiting — the overlay stays live and reattaches
	// to the new PID automatically.
	using clock = std::chrono::steady_clock;
	constexpr auto kAliveCheckInterval = std::chrono::seconds(2);

	while (bRunning)
	{
		if (!g_GameContext->Initialize(conn))
		{
			g_GameContext->Shutdown();
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}

		auto lastAliveCheck = clock::now();
		while (bRunning)
		{
			ZoneNamedN(__tick, "DMA_Tick", true);
			const auto now = clock::now();

			if (now - lastAliveCheck >= kAliveCheckInterval)
			{
				if (!g_GameContext->IsAlive(conn))
				{
					Log::Info("[AcqThread]: game process gone — detaching");
					break;
				}
				lastAliveCheck = now;
			}

			if (!g_GameContext->Tick(conn, now))
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		g_GameContext->Shutdown();
		if (bRunning)
			Log::Info("[AcqThread]: waiting for game to come back up...");
	}
	timeEndPeriod(1);

	conn->EndConnection();
}
