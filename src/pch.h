#pragma once

// Windows. winsock2.h must be included BEFORE Windows.h — otherwise
// Windows.h pulls in the ancient winsock.h (v1) via WinSock.h and the two
// collide everywhere src/share needs the v2 API. We include it here (with
// its lib pragma) so every TU gets the consistent order via the PCH.
#define NOMINMAX
#include <winsock2.h>
#include <Windows.h>
#pragma comment(lib, "ws2_32.lib")

// DirectX
#include <d3d11.h>
#pragma comment(lib, "d3d11")

// STL
#include <string>
#include <print>
#include <fstream>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <limits>
#include <functional>
#include <array>
#include <vector>
#include <memory>

// VMM (must follow Windows.h)
#include "vmmdll.h"
#pragma comment(lib, "vmm")

// Tracy profiler (on-demand: zero overhead until a profiler connects)
#include <tracy/Tracy.hpp>

// DMA layer — include order is mandatory: Log → DMA → Memory/ScatterRead → Memory/Process.
#include "DMA/Logging/Log.h"
#include "DMA/DMA.h"
#include "DMA/Memory/ScatterRead.h"
#include "DMA/Memory/Process.h"