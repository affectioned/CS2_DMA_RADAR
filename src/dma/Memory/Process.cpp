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

	while (bRunning)
	{
		VMMDLL_PidGetFromName(conn->GetHandle(), processName.c_str(), &m_PID);

		if (m_PID)
		{
			Log::Info("Found process `{}` with PID {}", processName, m_PID);
			PopulateModules(moduleNames, conn);
			break;
		}

		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	return m_PID != 0;
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
			std::this_thread::sleep_for(std::chrono::seconds(1));
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
