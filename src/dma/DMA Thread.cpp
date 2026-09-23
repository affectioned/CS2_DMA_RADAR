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

	timeBeginPeriod(1);
	while (bRunning)
	{
		ZoneNamedN(__tick, "DMA_Tick", true);
		if (!g_GameContext->Tick(conn, std::chrono::steady_clock::now()))
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	timeEndPeriod(1);

	conn->EndConnection();
}
