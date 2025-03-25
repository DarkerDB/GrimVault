#ifndef _UTIL_H
#define _UTIL_H

#include <algorithm>
#include <napi.h>
#include <optional>
#include <string>
#include <windows.h>

struct Window {
    std::string Executable;
    BOOL IsVisible;
    HWND Handle;
    RECT Bounds;
};

std::string WideToUTF8 (const std::wstring& Wide);
std::wstring UTF8ToWide (const std::string& Utf8);

std::optional<Window> GetWindowBounds (const std::string& ProcessName);

#endif