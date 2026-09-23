#pragma once

namespace updater {
	// Resolves module-level RVA pointers (dwEntityList etc.) via byte-pattern
	// matching in client.dll. Called from CS2Context::Initialize() after the
	// process is found.
	bool resolveOffsets(DMA_Connection* conn, Process* proc);

	// Fetches client_dll.hpp from a2x/cs2-dumper, parses the class member
	// offsets we use, and updates the client_dll:: globals in place. Cached
	// to disk next to the exe with an ETag sidecar — subsequent startups
	// send If-None-Match and consume zero bandwidth on a 304. On any failure,
	// falls back to the disk cache, then to the compiled-in defaults. Call
	// once at startup before any reads. Safe to call without an FPGA connected.
	bool fetchClassOffsets();

	// Same caching strategy, applied to cs2-dumper's offsets.hpp — populates
	// dw* RVA pointers not pattern-resolved (e.g. dwWeaponC4) and engine2_dll::
	// fields. Independent ETag cache (offsets.hpp + offsets.hpp.etag).
	bool fetchModuleOffsets();
}
