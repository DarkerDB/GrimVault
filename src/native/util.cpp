#include "logger.h"
#include "util.h"

std::string WideToUTF8 (const std::wstring& wide) {
    if (wide.empty ()) {
        return "";
    }

    int utf8Length = WideCharToMultiByte (
        CP_UTF8, 
        0, 
        wide.c_str (), 
        wide.length (), 
        nullptr, 
        0, 
        nullptr, 
        nullptr
    );

    std::string utf8 (utf8Length, 0);
    
    WideCharToMultiByte (
        CP_UTF8, 
        0, 
        wide.c_str (), 
        wide.length (), 
        &utf8 [0], 
        utf8Length, 
        nullptr, 
        nullptr
    );

    return utf8;
}

std::wstring UTF8ToWide (const std::string& utf8) {
    if (utf8.empty()) {
        return L"";
    }

    int wideLength = MultiByteToWideChar (
        CP_UTF8, 
        0, 
        utf8.c_str (), 
        utf8.length (), 
        nullptr, 
        0
    );

    std::wstring wide (wideLength, 0);

    MultiByteToWideChar (
        CP_UTF8, 
        0, 
        utf8.c_str (), 
        utf8.length (), 
        &wide [0], 
        wideLength
    );

    return wide;
}

float GetScalingFactorForMonitor (HWND hwnd) 
{
    if (!hwnd) {
        return 1.0f; // fallback
    }

    HMONITOR Monitor = MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST);
    
    if (!Monitor) {
        return 1.0f;
    }

    // Try using GetDpiForMonitor (Win8.1+)
    UINT dpiX, dpiY;

    if (SUCCEEDED (GetDpiForMonitor (Monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        return static_cast<float> (dpiX) / 96.0f;
    }

    // Fallback (not DPI-aware context, legacy systems)
    HDC Screen = GetDC (nullptr);
    int dpi = GetDeviceCaps (Screen, LOGPIXELSX);
    ReleaseDC(nullptr, Screen);

    return static_cast<float> (dpi) / 96.0f;
}

HWND FindGameWindow ()
{
    const wchar_t* TARGET_PROCESS = L"DungeonCrawler.exe";
    HWND Result = nullptr;

    struct FindParams {
        const wchar_t* TargetProcess;
        HWND Result;
    };

    static FindParams Params = {
        TARGET_PROCESS,
        nullptr
    };

    Params.Result = nullptr;

    EnumWindows ([] (HWND hwnd, LPARAM lParam) -> BOOL {
        auto* Params = reinterpret_cast<FindParams*> (lParam);
        
        if (!IsWindowVisible (hwnd)) {
            return TRUE;
        }

        DWORD ProcessId;
        GetWindowThreadProcessId (hwnd, &ProcessId);
        
        HANDLE ProcessHandle = OpenProcess (
            PROCESS_QUERY_LIMITED_INFORMATION, 
            FALSE, 
            ProcessId
        );
        
        if (ProcessHandle) {
            wchar_t ProcessPath [MAX_PATH];
            DWORD Size = MAX_PATH;
            
            if (QueryFullProcessImageNameW (ProcessHandle, 0, ProcessPath, &Size)) {
                const wchar_t* ProcessName = ProcessPath;
                
                for (const wchar_t* p = ProcessPath; *p != L'\0'; ++p) {
                    if (*p == L'\\' || *p == L'/') {
                        ProcessName = p + 1;
                    }
                }
                
                if (_wcsicmp (ProcessName, Params->TargetProcess) == 0) {
                    Params->Result = hwnd;
                    CloseHandle (ProcessHandle);
                    return FALSE;
                }
            }

            CloseHandle (ProcessHandle);
        }
        
        return true;
    }, reinterpret_cast<LPARAM> (&Params));

    if (!Params.Result) {
        Logger::log (
            Logger::Level::E_WARNING,
            "Game window not found for DungeonCrawler.exe"
        );
    }

    return Params.Result;
}

