#include "pch.h"
#include "Bootstrap.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

namespace
{
	constexpr const wchar_t* kGithubApiHost    = L"api.github.com";
	constexpr const wchar_t* kMemProcFSApiPath = L"/repos/ufrisk/MemProcFS/releases/latest";
	constexpr const char*    kAssetNeedle      = "win_x64-latest.zip";

	constexpr const wchar_t* kRepoArchiveHost = L"github.com";
	constexpr const wchar_t* kRepoArchivePath = L"/affectioned/CS2_DMA_RADAR/archive/refs/heads/main.zip";

	constexpr const wchar_t* kTracyApiPath   = L"/repos/wolfpld/tracy/releases/tags/v0.11.1";
	constexpr const char*    kTracyNeedle    = "windows-";
	const std::vector<std::wstring> kTracyTools = { L"tracy-capture.exe", L"tracy-csvexport.exe", L"tracy-profiler.exe" };

	const std::vector<std::wstring> kRequiredDlls = {
		L"vmm.dll",
		L"leechcore.dll",
		L"leechcore_driver.dll",
		L"FTD3XX.dll",
		L"FTD3XXWU.dll",
	};

	fs::path ExeDir()
	{
		wchar_t buf[MAX_PATH]{};
		GetModuleFileNameW(nullptr, buf, MAX_PATH);
		return fs::path(buf).parent_path();
	}

	bool AnyMissing(const fs::path& dir)
	{
		for (const auto& dll : kRequiredDlls)
		{
			if (!fs::exists(dir / dll)) return true;
		}
		return false;
	}

	std::string Narrow(std::wstring_view w)
	{
		if (w.empty()) return {};
		int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
			nullptr, 0, nullptr, nullptr);
		if (needed <= 0) return {};
		std::string s(needed, '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
			s.data(), needed, nullptr, nullptr);
		return s;
	}

	struct Handle
	{
		HINTERNET h = nullptr;
		explicit Handle(HINTERNET x) : h(x) {}
		~Handle() { if (h) WinHttpCloseHandle(h); }
		Handle(const Handle&)            = delete;
		Handle& operator=(const Handle&) = delete;
		operator HINTERNET() const { return h; }
		explicit operator bool() const { return h != nullptr; }
	};

	bool HttpsGet(const std::wstring& host, const std::wstring& path, std::vector<BYTE>& body)
	{
		Handle session(WinHttpOpen(L"CS2Radar-Bootstrap/1.0",
			WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
		if (!session) return false;

		Handle connect(WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
		if (!connect) return false;

		Handle req(WinHttpOpenRequest(connect, L"GET", path.c_str(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
		if (!req) return false;

		DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
		WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

		if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;
		if (!WinHttpReceiveResponse(req, nullptr)) return false;

		DWORD status = 0, statusSize = sizeof(status);
		WinHttpQueryHeaders(req,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
		if (status != 200)
		{
			Log::Error("[Bootstrap] HTTP {} for https://{}{}", status, Narrow(host), Narrow(path));
			return false;
		}

		body.clear();
		DWORD available = 0;
		while (WinHttpQueryDataAvailable(req, &available) && available > 0)
		{
			const size_t off = body.size();
			body.resize(off + available);
			DWORD read = 0;
			if (!WinHttpReadData(req, body.data() + off, available, &read)) return false;
			body.resize(off + read);
		}
		return true;
	}

	std::string FindAssetUrl(std::string_view json, std::string_view needle)
	{
		const size_t namePos = json.find(needle);
		if (namePos == std::string_view::npos) return {};

		constexpr std::string_view urlKey = "\"browser_download_url\":\"";
		const size_t urlPos = json.find(urlKey, namePos);
		if (urlPos == std::string_view::npos) return {};

		const size_t start = urlPos + urlKey.size();
		const size_t end   = json.find('"', start);
		if (end == std::string_view::npos) return {};
		return std::string(json.substr(start, end - start));
	}

	bool SplitHttpsUrl(const std::string& url, std::wstring& host, std::wstring& path)
	{
		constexpr std::string_view prefix = "https://";
		if (url.rfind(prefix, 0) != 0) return false;
		const size_t slash = url.find('/', prefix.size());
		if (slash == std::string::npos) return false;
		const std::string h = url.substr(prefix.size(), slash - prefix.size());
		const std::string p = url.substr(slash);
		host.assign(h.begin(), h.end());
		path.assign(p.begin(), p.end());
		return true;
	}

	bool ExtractZip(const fs::path& zipFile, const fs::path& outDir)
	{
		std::error_code ec;
		fs::create_directories(outDir, ec);

		wchar_t sys[MAX_PATH]{};
		GetSystemDirectoryW(sys, MAX_PATH);

		std::wstring cmd;
		cmd += L"\"";
		cmd += sys;
		cmd += L"\\tar.exe\" -xf \"";
		cmd += zipFile.wstring();
		cmd += L"\" -C \"";
		cmd += outDir.wstring();
		cmd += L"\"";

		STARTUPINFOW si{ sizeof(si) };
		si.dwFlags     = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi{};

		std::wstring mutableCmd = cmd;
		if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;

		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD exitCode = 1;
		GetExitCodeProcess(pi.hProcess, &exitCode);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return exitCode == 0;
	}

	bool TexturesMissing(const fs::path& exeDir)
	{
		std::error_code ec;
		const auto maps  = exeDir / L"textures" / L"maps";
		const auto icons = exeDir / L"textures" / L"icons";
		if (!fs::exists(maps) || !fs::exists(icons)) return true;
		return fs::is_empty(maps, ec) || fs::is_empty(icons, ec);
	}

	fs::path FindTexturesDir(const fs::path& root)
	{
		std::error_code ec;
		for (auto it = fs::recursive_directory_iterator(root, ec);
			it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			if (ec) { ec.clear(); continue; }
			if (!it->is_directory()) continue;
			if (it->path().filename() == L"textures" &&
				fs::exists(it->path() / L"maps") &&
				fs::exists(it->path() / L"icons"))
				return it->path();
		}
		return {};
	}

	void InstallDllsFrom(const fs::path& extractedRoot, const fs::path& exeDir)
	{
		std::error_code ec;
		for (auto it = fs::recursive_directory_iterator(extractedRoot, ec);
			it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			if (ec) { ec.clear(); continue; }
			if (!it->is_regular_file()) continue;

			const auto& p = it->path();
			for (const auto& want : kRequiredDlls)
			{
				if (_wcsicmp(p.filename().c_str(), want.c_str()) != 0) continue;

				fs::copy_file(p, exeDir / want, fs::copy_options::overwrite_existing, ec);
				if (ec)
				{
					Log::Warn("[Bootstrap] Copy failed for {}: {}", Narrow(want), ec.message());
					ec.clear();
				}
				else
				{
					Log::Info("[Bootstrap] Installed {}", Narrow(want));
				}
			}
		}
	}
} // namespace

namespace Bootstrap
{
	bool EnsureRuntimeDlls()
	{
		const fs::path exeDir = ExeDir();
		if (!AnyMissing(exeDir)) return true;

		Log::Info("[Bootstrap] MemProcFS runtime DLLs missing; fetching latest release...");

		std::vector<BYTE> apiBody;
		if (!HttpsGet(kGithubApiHost, kMemProcFSApiPath, apiBody))
		{
			Log::Error("[Bootstrap] Failed to query GitHub releases API");
			return false;
		}

		const std::string_view json(reinterpret_cast<const char*>(apiBody.data()), apiBody.size());
		const std::string zipUrl = FindAssetUrl(json, kAssetNeedle);
		if (zipUrl.empty())
		{
			Log::Error("[Bootstrap] Could not find asset '{}' in release manifest", kAssetNeedle);
			return false;
		}
		Log::Info("[Bootstrap] Downloading {}", zipUrl);

		std::wstring zipHost, zipPath;
		if (!SplitHttpsUrl(zipUrl, zipHost, zipPath))
		{
			Log::Error("[Bootstrap] Malformed asset URL: {}", zipUrl);
			return false;
		}

		std::vector<BYTE> zipBytes;
		if (!HttpsGet(zipHost, zipPath, zipBytes))
		{
			Log::Error("[Bootstrap] Failed to download release zip");
			return false;
		}

		wchar_t tempRoot[MAX_PATH]{};
		GetTempPathW(MAX_PATH, tempRoot);
		const fs::path staging = fs::path(tempRoot) / L"cs2radar_bootstrap";
		std::error_code ec;
		fs::remove_all(staging, ec);
		fs::create_directories(staging, ec);

		const fs::path zipFile = staging / L"memprocfs.zip";
		{
			std::ofstream out(zipFile, std::ios::binary);
			out.write(reinterpret_cast<const char*>(zipBytes.data()), zipBytes.size());
		}

		const fs::path extractDir = staging / L"extracted";
		if (!ExtractZip(zipFile, extractDir))
		{
			Log::Error("[Bootstrap] tar extraction failed");
			return false;
		}

		InstallDllsFrom(extractDir, exeDir);

		if (AnyMissing(exeDir))
		{
			Log::Error("[Bootstrap] One or more required DLLs still missing after install");
			return false;
		}

		fs::remove_all(staging, ec);
		Log::Info("[Bootstrap] MemProcFS runtime DLLs ready");
		return true;
	}
	bool EnsureTextures()
	{
		const fs::path exeDir = ExeDir();
		if (!TexturesMissing(exeDir)) return true;

		Log::Info("[Bootstrap] Textures missing; downloading from repository...");

		std::vector<BYTE> zipBytes;
		if (!HttpsGet(kRepoArchiveHost, kRepoArchivePath, zipBytes))
		{
			Log::Error("[Bootstrap] Failed to download repository archive");
			return false;
		}
		Log::Info("[Bootstrap] Downloaded {:.1f} MB, extracting...",
			zipBytes.size() / (1024.0 * 1024.0));

		wchar_t tempRoot[MAX_PATH]{};
		GetTempPathW(MAX_PATH, tempRoot);
		const fs::path staging = fs::path(tempRoot) / L"cs2radar_textures";
		std::error_code ec;
		fs::remove_all(staging, ec);
		fs::create_directories(staging, ec);

		const fs::path zipFile = staging / L"repo.zip";
		{
			std::ofstream out(zipFile, std::ios::binary);
			out.write(reinterpret_cast<const char*>(zipBytes.data()), zipBytes.size());
		}

		const fs::path extractDir = staging / L"extracted";
		if (!ExtractZip(zipFile, extractDir))
		{
			Log::Error("[Bootstrap] Archive extraction failed");
			fs::remove_all(staging, ec);
			return false;
		}

		const fs::path srcTextures = FindTexturesDir(extractDir);
		if (srcTextures.empty())
		{
			Log::Error("[Bootstrap] Could not locate textures in archive");
			fs::remove_all(staging, ec);
			return false;
		}

		const fs::path dstTextures = exeDir / L"textures";
		fs::create_directories(dstTextures / L"maps", ec);
		fs::create_directories(dstTextures / L"icons", ec);

		fs::copy(srcTextures / L"maps", dstTextures / L"maps",
			fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
		if (ec) { Log::Warn("[Bootstrap] maps copy: {}", ec.message()); ec.clear(); }

		fs::copy(srcTextures / L"icons", dstTextures / L"icons",
			fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
		if (ec) { Log::Warn("[Bootstrap] icons copy: {}", ec.message()); ec.clear(); }

		fs::remove_all(staging, ec);

		if (TexturesMissing(exeDir))
		{
			Log::Error("[Bootstrap] Textures still missing after install");
			return false;
		}

		Log::Info("[Bootstrap] Textures ready");
		return true;
	}
	bool EnsureTracyTools()
	{
		const fs::path exeDir = ExeDir();
		bool allPresent = true;
		for (const auto& tool : kTracyTools)
			if (!fs::exists(exeDir / tool)) { allPresent = false; break; }
		if (allPresent) return true;

		Log::Info("[Bootstrap] Tracy tools missing; fetching latest release...");

		std::vector<BYTE> apiBody;
		if (!HttpsGet(kGithubApiHost, kTracyApiPath, apiBody))
		{
			Log::Error("[Bootstrap] Failed to query Tracy releases API");
			return false;
		}

		const std::string_view json(reinterpret_cast<const char*>(apiBody.data()), apiBody.size());
		const std::string zipUrl = FindAssetUrl(json, kTracyNeedle);
		if (zipUrl.empty())
		{
			Log::Error("[Bootstrap] Could not find Windows asset in Tracy release");
			return false;
		}
		Log::Info("[Bootstrap] Downloading {}", zipUrl);

		std::wstring zipHost, zipPath;
		if (!SplitHttpsUrl(zipUrl, zipHost, zipPath)) return false;

		std::vector<BYTE> zipBytes;
		if (!HttpsGet(zipHost, zipPath, zipBytes)) return false;

		wchar_t tempRoot[MAX_PATH]{};
		GetTempPathW(MAX_PATH, tempRoot);
		const fs::path staging = fs::path(tempRoot) / L"cs2radar_tracy";
		std::error_code ec;
		fs::remove_all(staging, ec);
		fs::create_directories(staging, ec);

		const fs::path zipFile = staging / L"tracy.zip";
		{
			std::ofstream out(zipFile, std::ios::binary);
			out.write(reinterpret_cast<const char*>(zipBytes.data()), zipBytes.size());
		}

		const fs::path extractDir = staging / L"extracted";
		if (!ExtractZip(zipFile, extractDir))
		{
			Log::Error("[Bootstrap] Tracy zip extraction failed");
			fs::remove_all(staging, ec);
			return false;
		}

		for (auto it = fs::recursive_directory_iterator(extractDir, ec);
			it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			if (ec) { ec.clear(); continue; }
			if (!it->is_regular_file()) continue;
			for (const auto& want : kTracyTools)
			{
				if (_wcsicmp(it->path().filename().c_str(), want.c_str()) != 0) continue;
				fs::copy_file(it->path(), exeDir / want, fs::copy_options::overwrite_existing, ec);
				if (!ec) Log::Info("[Bootstrap] Installed {}", Narrow(want));
				ec.clear();
			}
		}

		fs::remove_all(staging, ec);

		for (const auto& tool : kTracyTools)
			if (!fs::exists(exeDir / tool)) {
				Log::Error("[Bootstrap] {} still missing after install", Narrow(tool));
				return false;
			}

		Log::Info("[Bootstrap] Tracy tools ready");
		return true;
	}

	bool RunTracySession(int durationSec)
	{
		const fs::path exeDir = ExeDir();
		const fs::path csvexport = exeDir / L"tracy-csvexport.exe";
		const fs::path traceFile = exeDir / L"profile.tracy";
		const fs::path reportFile = exeDir / L"profile_report.txt";

		if (!fs::exists(csvexport))
		{
			Log::Error("[Tracy] tracy-csvexport.exe not found");
			return false;
		}
		if (!fs::exists(traceFile))
		{
			Log::Error("[Tracy] profile.tracy not found — capture may have failed");
			return false;
		}

		Log::Info("[Tracy] Exporting trace to CSV...");

		std::wstring csvCmd = L"\"" + csvexport.wstring() + L"\" \""
			+ traceFile.wstring() + L"\"";
		std::wstring mutableCsvCmd = csvCmd;

		HANDLE hReadPipe, hWritePipe;
		SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
		CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
		SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW si{ sizeof(si) };
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdOutput = hWritePipe;
		si.hStdError = hWritePipe;
		PROCESS_INFORMATION pi{};

		if (!CreateProcessW(nullptr, mutableCsvCmd.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		{
			Log::Error("[Tracy] Failed to launch tracy-csvexport");
			CloseHandle(hReadPipe);
			CloseHandle(hWritePipe);
			return false;
		}
		CloseHandle(pi.hThread);
		CloseHandle(hWritePipe);

		std::string csvOutput;
		char buf[4096];
		DWORD bytesRead;
		while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0)
			csvOutput.append(buf, bytesRead);
		CloseHandle(hReadPipe);

		WaitForSingleObject(pi.hProcess, INFINITE);
		CloseHandle(pi.hProcess);

		std::ofstream report(reportFile);
		report << "=== Tracy Profile Report ===\n";
		report << "Duration: " << durationSec << "s\n";
		report << "Trace file: profile.tracy\n\n";
		report << "--- Raw CSV Zone Data ---\n";
		report << csvOutput << "\n";
		report << "--- End ---\n\n";
		report << "Paste this into an LLM conversation with the source code.\n";
		report << "Ask: which zones dominate wall time? Are any DMA scatter reads >1ms mean?\n";
		report << "Are timer callbacks (t_*) running longer than their interval?\n";

		Log::Info("[Tracy] Report written to: {}", Narrow(reportFile.wstring()));
		return true;
	}
} // namespace Bootstrap
