#pragma once

#include <optional>
#include <windows.h>
#include <wrl/client.h>
#include <dxgi1_6.h>

#pragma comment(lib, "dxgi.lib")

HWND FindGameWindow ();
std::optional<int> GetGameMonitorId ();
bool IsMonitorHDR (HMONITOR Monitor);