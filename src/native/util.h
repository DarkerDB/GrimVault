#ifndef _UTIL_H
#define _UTIL_H

#include <algorithm>
#include <napi.h>
#include <optional>
#include <shellscalingapi.h>
#include <string>
#include <windows.h>

std::string WideToUTF8 (const std::wstring& Wide);
std::wstring UTF8ToWide (const std::string& Utf8);
float GetScalingFactorForMonitor (HWND hwnd);
HWND FindGameWindow ();

#endif