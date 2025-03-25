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

static BOOL CALLBACK EnumWindowsCallback (HWND hwnd, LPARAM lParam) 
{
    Window* Data = reinterpret_cast<Window*>(lParam);

    Data->IsVisible = FALSE;
    Data->Handle = NULL;
    
    if (!IsWindowVisible (hwnd)) {
        return TRUE;
    }
    
    WCHAR WindowTitle [256] = { 0 };

    GetWindowTextW (hwnd, WindowTitle, 256);

    if (wcslen (WindowTitle) == 0) {
        return TRUE;
    }
    
    DWORD ProcessId;

    GetWindowThreadProcessId (hwnd, &ProcessId);
    
    HANDLE ProcessHandle = OpenProcess (
        PROCESS_QUERY_LIMITED_INFORMATION, 
        FALSE, 
        ProcessId
    );

    if (!ProcessHandle) {
        return TRUE;
    }
    
    WCHAR ProcessPath [MAX_PATH];
    DWORD PathSize = MAX_PATH;

    BOOL Success = QueryFullProcessImageNameW (
        ProcessHandle, 
        0, 
        ProcessPath, 
        &PathSize
    );

    CloseHandle (ProcessHandle);
    
    if (!Success) {
        return TRUE;
    }
    
    WCHAR* Executable = wcsrchr (ProcessPath, L'\\');
    std::wstring ExeName;
    
    if (Executable == nullptr) {
        ExeName = ProcessPath;
    } else {
        ExeName = Executable + 1;
    }
    
    std::string Utf8ExeName = WideToUTF8 (ExeName);
    
    std::transform (
        Utf8ExeName.begin (), 
        Utf8ExeName.end (), 
        Utf8ExeName.begin (), 
        ::tolower
    );
    
    if (Utf8ExeName == Data->Executable) {
        Data->IsVisible = TRUE;
        Data->Handle = hwnd;
        
        GetWindowRect (hwnd, &Data->Bounds);  
        
        return FALSE;
    }
    
    return TRUE;
}

std::optional<Window> GetWindowBounds (const std::string& ProcessName) 
{
    std::string LowerProcessName = ProcessName;

    std::transform (
        LowerProcessName.begin (), 
        LowerProcessName.end (), 
        LowerProcessName.begin (), 
        ::tolower
    );
    
    Window Data;

    Data.Executable = LowerProcessName;
    
    EnumWindows (EnumWindowsCallback, reinterpret_cast<LPARAM> (&Data));

    if (!Data.Handle) {
        return std::nullopt;
    }

    return Data;
}
