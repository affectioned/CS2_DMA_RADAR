#include "pch.h"

#include "DMA/DMA.h"
#include "Process.h"

extern std::atomic<bool> bRunning;

bool Process::GetProcessInfo(const std::string& processName,
                              const std::vector<std::string>& moduleNames,
                              DMA_Connection* conn)
{
	Log::Info("Waiting for process {}..", processName);

	m_PID = 0;
	m_ProcessName = processName;

	auto handle = conn->GetHandle();
	uint32_t iter = 0;

	while (bRunning)
	{
		VMMDLL_PidGetFromName(handle, processName.c_str(), &m_PID);

		if (m_PID)
		{
			Log::Info("Found process `{}` with PID {}", processName, m_PID);
			PopulateModules(moduleNames, conn);
			break;
		}

		// VMM runs with -norefresh so its caches are frozen on connect and a
		// cs2.exe launched after the radar started is invisible until we kick
		// a refresh. REFRESH_FREQ_MEDIUM alone proved unreliable on some VMM
		// builds (82+ s of silence in the field), so we escalate: medium each
		// iteration, plus a full REFRESH_ALL every 5 s as a nuclear fallback.
		VMMDLL_ConfigSet(handle, VMMDLL_OPT_REFRESH_FREQ_MEDIUM, 1);
		if ((iter % 5) == 4)
			VMMDLL_ConfigSet(handle, VMMDLL_OPT_REFRESH_ALL, 1);

		// Heartbeat every 10 s so the log makes it obvious we're alive and
		// still polling, instead of looking stuck like before.
		if (iter && (iter % 10) == 0)
			Log::Info("Still waiting for {}.. ({} s)", processName, iter);

		++iter;
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	return m_PID != 0;
}

bool Process::IsAlive(DMA_Connection* conn) const
{
	if (!m_PID || m_ProcessName.empty()) return false;
	DWORD livePid = 0;
	if (!VMMDLL_PidGetFromName(conn->GetHandle(), m_ProcessName.c_str(), &livePid))
		return false;
	return livePid == m_PID;
}

void Process::Reset()
{
	m_PID = 0;
	m_Modules.clear();
	m_ModuleSizes.clear();
	// m_ProcessName intentionally preserved — IsAlive() keeps probing
	// the same process name across the detach→reattach cycle.
}

uintptr_t Process::GetModuleBase(const std::string& name) const
{
	auto it = m_Modules.find(name);
	return it != m_Modules.end() ? it->second : 0;
}

size_t Process::GetModuleSize(const std::string& name) const
{
	auto it = m_ModuleSizes.find(name);
	return it != m_ModuleSizes.end() ? it->second : 0;
}

DWORD Process::GetPID() const
{
	return m_PID;
}

bool Process::PopulateModules(const std::vector<std::string>& names, DMA_Connection* conn)
{
	auto handle = conn->GetHandle();

	auto allResolved = [&]
	{
		for (const auto& name : names) {
			auto it = m_Modules.find(name);
			if (it == m_Modules.end() || !it->second) return false;
		}
		return true;
	};

	while (bRunning && !allResolved())
	{
		for (const auto& name : names)
		{
			if (!m_Modules[name])
				m_Modules[name] = VMMDLL_ProcessGetModuleBaseU(handle, m_PID, name.c_str());
		}

		if (!allResolved())
		{
			// With -norefresh the per-process module list is cached once per
			// enumeration pass; a cs2.exe that just launched may still be
			// loading client.dll / engine2.dll. Force a module refresh for
			// this specific PID so the next iteration sees newly-loaded DLLs.
			VMMDLL_ConfigSet(handle,
			                 VMMDLL_OPT_REFRESH_SPECIFIC_PROCESS | static_cast<uint64_t>(m_PID),
			                 1);
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}

	for (const auto& name : names)
	{
		PVMMDLL_MAP_MODULEENTRY info = nullptr;
		if (VMMDLL_Map_GetModuleFromNameU(handle, m_PID, const_cast<LPSTR>(name.c_str()), &info, VMMDLL_MODULE_FLAG_NORMAL))
		{
			m_ModuleSizes[name] = info->cbImageSize;
			VMMDLL_MemFree(info);
		}
	}

	for (const auto& [name, addr] : m_Modules) {
		auto sit = m_ModuleSizes.find(name);
		Log::Info("Module `{}` at 0x{:X} size 0x{:X}", name, addr,
		          sit != m_ModuleSizes.end() ? sit->second : 0);
	}

	return true;
}
