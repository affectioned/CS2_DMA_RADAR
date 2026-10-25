#pragma once
#include <chrono>

class DMA_Connection;

// Implement this interface in the game layer and assign g_GameContext before
// launching DMA_Thread_Main. The DMA thread calls Initialize once, then Tick
// every millisecond. If the game process dies the supervisor calls Shutdown
// to clear per-process state, then Initialize again against the new process —
// so Initialize and Shutdown must be idempotent / safe to call in a loop.
struct IGameContext
{
    // Called once after the DMA connection is established and keyboard is
    // initialized, and again after Shutdown() every time the game restarts.
    // Return false to abort / retry.
    virtual bool Initialize(DMA_Connection* conn) = 0;

    // Called every tick (nominally 1 ms). Tick your CTimers here.
    // Returns true if any timer fired (caller uses this to decide sleep vs spin).
    virtual bool Tick(DMA_Connection* conn, std::chrono::steady_clock::time_point now) = 0;

    // Fast liveness check — returns true while the attached game process is
    // still alive. The supervisor calls this every ~2s; false triggers a
    // teardown + reattach cycle without exiting the DMA thread.
    virtual bool IsAlive(DMA_Connection* conn) = 0;

    // Teardown of all per-process state (cached pointers, scatter handle,
    // timers, local snapshot). After Shutdown() a subsequent Initialize()
    // against a new game process must leave the context in a usable state.
    virtual void Shutdown() = 0;

    virtual ~IGameContext() = default;
};

// Defined in DMA Thread.cpp. Set by main() before launching DMA_Thread_Main.
extern IGameContext* g_GameContext;
