#pragma once

namespace Bootstrap
{
	bool EnsureRuntimeDlls();
	bool EnsureTextures();
	bool EnsureTracyTools();
	bool RunTracySession(int durationSec = 30);
}
