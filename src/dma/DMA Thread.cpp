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

	if (!g_GameContext || !g_GameContext->Initialize(conn))
	{
		Log::Error("[AcqThread]: Initialization failed, requesting exit.");
		bRunning = false;
		return;
	}

	while (bRunning)
	{
		std::this_thread::yield();
		ZoneNamedN(__tick, "DMA_Tick", true);
		g_GameContext->Tick(conn, std::chrono::steady_clock::now());
	}

	conn->EndConnection();
}
