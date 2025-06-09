#include "logger.h"
#include "util.h"
#include <ScreenCapture.h>




// -- -- //






std::mutex GameMonitorMutex;
std::optional<int> GameMonitorId;
std::chrono::steady_clock::time_point LastMonitorCheck = std::chrono::steady_clock::now ();

HWND FindGameWindow ()
{
   const wchar_t* TargetProcess = L"DungeonCrawler.exe";
   HWND Result = nullptr;
   
   struct FindParams {
      const wchar_t* TargetProcess;
      HWND Result;
   };
   
   static FindParams Params = {
      TargetProcess,
      nullptr
   };
   
   Params.Result = nullptr;
   
   EnumWindows ([] (HWND Hwnd, LPARAM LParam) -> BOOL {
      auto* Params = reinterpret_cast<FindParams*> (LParam);
      
      if (!IsWindowVisible (Hwnd)) {
         return true;
      }
      
      DWORD ProcessId;
      GetWindowThreadProcessId (Hwnd, &ProcessId);
      
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
            
            for (const wchar_t* P = ProcessPath; *P != L'\0'; ++P) {
               if (*P == L'\\' || *P == L'/') {
                  ProcessName = P + 1;
               }
            }
            
            if (_wcsicmp (ProcessName, Params->TargetProcess) == 0) {
               Params->Result = Hwnd;
               CloseHandle (ProcessHandle);
               return false;
            }
         }
         
         CloseHandle (ProcessHandle);
      }
      
      return true;
   }, reinterpret_cast<LPARAM> (&Params));
   
   if (!Params.Result) {
      Logger::log (
         Logger::Level::E_DEBUG,
         "Game window not found for DungeonCrawler.exe"
      );
   }
   
   return Params.Result;
}

std::optional<int> GetGameMonitorId ()
{
   auto Now = std::chrono::steady_clock::now ();
   auto TimeSinceLastCheck = std::chrono::duration_cast<std::chrono::milliseconds> (Now - LastMonitorCheck).count ();
   
   // Only check for game window every 10000ms (1 second) to avoid performance impact
   if (TimeSinceLastCheck < 10000) {
      std::lock_guard<std::mutex> Lock (GameMonitorMutex);
      return GameMonitorId;
   }
   
   LastMonitorCheck = Now;
   
   HWND GameWindow = FindGameWindow ();
   
   if (!GameWindow) {
      std::lock_guard<std::mutex> Lock (GameMonitorMutex);
      GameMonitorId = std::nullopt;
      return std::nullopt;
   }
   
   HMONITOR Monitor = MonitorFromWindow (GameWindow, MONITOR_DEFAULTTONEAREST);
   
   if (!Monitor) {
      std::lock_guard<std::mutex> Lock (GameMonitorMutex);
      GameMonitorId = std::nullopt;
      return std::nullopt;
   }
   
   MONITORINFOEX MonitorInfo = {0};
   MonitorInfo.cbSize = sizeof(MONITORINFOEX);

   if (!GetMonitorInfo (Monitor, &MonitorInfo)) {
      std::lock_guard<std::mutex> Lock (GameMonitorMutex);
      GameMonitorId = std::nullopt;
      return std::nullopt;
   }
   
   auto Monitors = SL::Screen_Capture::GetMonitors ();
   
   for (const auto& M : Monitors) {
      RECT MonitorRect = MonitorInfo.rcMonitor;
      
      int PhysicalWidth = MonitorRect.right - MonitorRect.left;
      int PhysicalHeight = MonitorRect.bottom - MonitorRect.top;
      
      bool IsMatch = (M.OffsetX == MonitorRect.left && 
                      M.OffsetY == MonitorRect.top && 
                       (
                        (M.Width == PhysicalWidth && M.Height == PhysicalHeight) ||
                        (M.OriginalWidth == PhysicalWidth && M.OriginalHeight == PhysicalHeight)
                       ));
      
      if (IsMatch) {
         std::lock_guard<std::mutex> Lock (GameMonitorMutex);
         GameMonitorId = M.Id;
         
         Logger::log (
            Logger::Level::E_INFO,
            "Found game window on monitor " + std::to_string (M.Id) + 
            " (" + std::to_string (M.Width) + "x" + std::to_string (M.Height) + 
            ", Original: " + std::to_string (M.OriginalWidth) + "x" + std::to_string (M.OriginalHeight) + 
            ", Physical: " + std::to_string (PhysicalWidth) + "x" + std::to_string (PhysicalHeight) + 
            ") at (" + std::to_string (M.OffsetX) + "," + std::to_string (M.OffsetY) + 
            "), Scaling: " + std::to_string (M.Scaling)
         );
         
         return M.Id;
      }
   }
   
   std::lock_guard<std::mutex> Lock (GameMonitorMutex);
   GameMonitorId = std::nullopt;
   return std::nullopt;
}

bool IsMonitorHDR (HMONITOR Monitor) 
{
   Microsoft::WRL::ComPtr<IDXGIFactory6> Factory;

   if (FAILED (CreateDXGIFactory2(0, IID_PPV_ARGS (&Factory)))) {
      return false;
   }
   
   Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;

   for (UINT i = 0; Factory->EnumAdapterByGpuPreference (i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS (&Adapter)) == S_OK; ++i) {
       Microsoft::WRL::ComPtr<IDXGIOutput> Output;
       
       for (UINT j = 0; Adapter->EnumOutputs (j, &Output) == S_OK; ++j) {
           Microsoft::WRL::ComPtr<IDXGIOutput6> Output6;
           DXGI_OUTPUT_DESC1 Desc1;

           if (SUCCEEDED (Output->QueryInterface(IID_PPV_ARGS (&Output6))) && 
               SUCCEEDED (Output6->GetDesc1 (&Desc1)) && 
               Desc1.Monitor == Monitor) {
               return Desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || 
                      Desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
           }
       }
   }

   return false;
}