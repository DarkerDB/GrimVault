#pragma once

#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <opencv2/core/mat.hpp>
#include <optional>
#include <mutex>
#include <atomic>
#include <thread>

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

class WindowsGraphicsCapture
{
public:
    WindowsGraphicsCapture ();
    ~WindowsGraphicsCapture ();
    
    bool Initialize ();
    void Cleanup ();
    std::optional<cv::Mat> CaptureWindow (HWND GameWindow);
    
private:
    std::mutex CaptureLock;
    std::atomic<bool> IsInitialized = false;
    
    // Thread safety - ensure D3D11 is only used from creating thread
    std::thread::id CreatingThreadId;
    
    // D3D11 resources
    Microsoft::WRL::ComPtr<ID3D11Device> Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context;
    
    // WinRT Direct3D device
    winrt::IDirect3DDevice WinRTDevice;
    
    // Capture resources
    winrt::GraphicsCaptureItem CaptureItem = nullptr;
    winrt::Direct3D11CaptureFramePool FramePool = nullptr;
    winrt::GraphicsCaptureSession CaptureSession = nullptr;
    
    bool InitializeD3D ();
    void CleanupD3D ();
    void CleanupWGC ();
    
    static winrt::IDirect3DDevice CreateDirect3DDevice (IDXGIDevice* DxgiDevice);
};