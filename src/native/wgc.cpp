#include "wgc.h"
#include "logger.h"
#include <thread>
#include <chrono>
#include <functional>
#include <memory>
#include <combaseapi.h>

// Include Windows.Graphics.Capture.Interop.h
extern "C"
{
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice (IDXGIDevice* dxgiDevice, IInspectable** d3dDevice);
}

// IGraphicsCaptureItemInterop
struct __declspec(uuid("3628e81b-3cac-4c60-b7f4-23ce0e0c3356")) IGraphicsCaptureItemInterop : IUnknown
{
    virtual HRESULT __stdcall CreateForWindow (
        HWND window,
        REFIID riid,
        void** result
    ) = 0;
    
    virtual HRESULT __stdcall CreateForMonitor (
        HMONITOR monitor,
        REFIID riid,
        void** result
    ) = 0;
};

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess : IUnknown
{
    virtual HRESULT __stdcall GetInterface(REFIID riid, void** ppvObject) = 0;
};

WindowsGraphicsCapture::WindowsGraphicsCapture ()
{
}

WindowsGraphicsCapture::~WindowsGraphicsCapture ()
{
    Cleanup ();
}

bool WindowsGraphicsCapture::Initialize ()
{
    std::lock_guard<std::mutex> Lock (CaptureLock);
    
    if (IsInitialized) {
        return true;
    }
    
    // Ensure COM is initialized on this thread
    HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        Logger::log (
            Logger::Level::E_ERROR,
            "Failed to initialize COM for WGC"
        );
        return false;
    }
    
    Logger::log (
        Logger::Level::E_INFO,
        "Initializing Windows Graphics Capture"
    );
    
    try {
        if (!winrt::GraphicsCaptureSession::IsSupported ()) {
            Logger::log (
                Logger::Level::E_ERROR,
                "Windows Graphics Capture is not supported on this system"
            );
            return false;
        }
        
        if (!InitializeD3D ()) {
            Logger::log (
                Logger::Level::E_ERROR,
                "Failed to initialize D3D11 device for WGC"
            );
            return false;
        }
        
        Microsoft::WRL::ComPtr<IDXGIDevice> DxgiDevice;
        HRESULT hr = Device.As (&DxgiDevice);
        
        if (FAILED (hr)) {
            Logger::log (hr, "Failed to get DXGI device from D3D11 device");
            return false;
        }
        
        WinRTDevice = CreateDirect3DDevice (DxgiDevice.Get ());
        
        if (!WinRTDevice) {
            Logger::log (
                Logger::Level::E_ERROR,
                "Failed to create WinRT Direct3D device"
            );
            return false;
        }
        
        // Store the creating thread ID for safety checks
        CreatingThreadId = std::this_thread::get_id ();
        
        IsInitialized = true;
        
        Logger::log (
            Logger::Level::E_INFO,
            "Windows Graphics Capture initialized successfully"
        );
        
        return true;
    } catch (const winrt::hresult_error& e) {
        Logger::log (
            Logger::Level::E_ERROR,
            "WinRT error during WGC initialization: " + 
            winrt::to_string (e.message ())
        );
        
        return false;
    } catch (const std::exception& e) {
        Logger::log (
            Logger::Level::E_ERROR,
            std::string ("Exception during WGC initialization: ") + e.what ()
        );

        return false;
    } catch (...) {
        Logger::log (
            Logger::Level::E_ERROR,
            "Unknown exception during WGC initialization"
        );
        
        return false;
    }
}

void WindowsGraphicsCapture::Cleanup ()
{
    std::lock_guard<std::mutex> Lock (CaptureLock);
    
    CleanupWGC ();
    CleanupD3D ();
    
    IsInitialized = false;
    
    Logger::log (
        Logger::Level::E_INFO,
        "Windows Graphics Capture cleaned up"
    );
}

bool WindowsGraphicsCapture::InitializeD3D ()
{
    CleanupD3D ();
    
    Logger::log (
        Logger::Level::E_INFO,
        "Initializing D3D11 device for WGC"
    );
    
    D3D_FEATURE_LEVEL FeatureLevels [] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    
    UINT NumFeatureLevels = ARRAYSIZE (FeatureLevels);
    D3D_FEATURE_LEVEL FeatureLevel;
    
    HRESULT hr = D3D11CreateDevice (
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        FeatureLevels,
        NumFeatureLevels,
        D3D11_SDK_VERSION,
        &Device,
        &FeatureLevel,
        &Context
    );
    
    if (FAILED (hr)) {
        Logger::log (hr, "Failed to create D3D11 device for WGC");
        return false;
    }
    
    Logger::log (
        Logger::Level::E_INFO,
        "D3D11 device created successfully for WGC"
    );
    
    return true;
}

void WindowsGraphicsCapture::CleanupD3D ()
{
    if (Context) Context.Reset ();
    if (Device) Device.Reset ();
}

void WindowsGraphicsCapture::CleanupWGC ()
{
    try {
        if (CaptureSession) {
            CaptureSession.Close ();
            CaptureSession = nullptr;
        }
        
        if (FramePool) {
            FramePool.Close ();
            FramePool = nullptr;
        }
        
        CaptureItem = nullptr;
        WinRTDevice = nullptr;
    } catch (const std::exception& e) {
        Logger::log (
            Logger::Level::E_WARNING,
            std::string ("Exception during WGC cleanup: ") + e.what ()
        );
    } catch (...) {
        Logger::log (
            Logger::Level::E_WARNING,
            "Unknown exception during WGC cleanup"
        );
    }
}

std::optional<cv::Mat> WindowsGraphicsCapture::CaptureWindow (HWND GameWindow)
{
    if (!IsInitialized || !GameWindow) {
        return std::nullopt;
    }
    
    std::lock_guard<std::mutex> Lock (CaptureLock);
    
    try {
        auto InteropFactory = winrt::get_activation_factory<
            winrt::GraphicsCaptureItem
        >().as<IGraphicsCaptureItemInterop> ();
        
        bool NeedsNewSession = false;
        
        if (!CaptureItem) {
            NeedsNewSession = true;
            
            winrt::GraphicsCaptureItem Item { nullptr };
            winrt::check_hresult (InteropFactory->CreateForWindow (
                GameWindow,
                winrt::guid_of<winrt::GraphicsCaptureItem> (),
                winrt::put_abi (Item)
            ));
            
            if (!Item) {
                Logger::log (
                    Logger::Level::E_WARNING,
                    "Failed to create capture item for window"
                );
                return std::nullopt;
            }
            
            CaptureItem = Item;
        }
        
        if (NeedsNewSession || !FramePool || !CaptureSession) {
            if (CaptureSession) {
                CaptureSession.Close ();
                CaptureSession = nullptr;
            }
            
            if (FramePool) {
                FramePool.Close ();
                FramePool = nullptr;
            }
            
            auto Size = CaptureItem.Size ();
            
            FramePool = winrt::Direct3D11CaptureFramePool::Create (
                WinRTDevice,
                winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                1,
                Size
            );
            
            CaptureSession = FramePool.CreateCaptureSession (CaptureItem);
            CaptureSession.StartCapture ();
            
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
        
        // Discard stale frames
        for (int i = 0; i < 3; i++) {
            auto DiscardFrame = FramePool.TryGetNextFrame ();
            if (DiscardFrame) {
                DiscardFrame.Close ();
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        }
        
        auto Frame = FramePool.TryGetNextFrame ();
        
        if (!Frame) {
            Logger::log (
                Logger::Level::E_WARNING,
                "Failed to get capture frame"
            );
            return std::nullopt;
        }
        
        auto Surface = Frame.Surface ();
        auto Access = Surface.as<IDirect3DDxgiInterfaceAccess>();
        
        winrt::com_ptr<ID3D11Texture2D> Texture;
        winrt::check_hresult (Access->GetInterface (IID_PPV_ARGS (&Texture)));
        
        D3D11_TEXTURE2D_DESC Desc;
        Texture->GetDesc (&Desc);
        
        D3D11_TEXTURE2D_DESC StagingDesc = Desc;
        StagingDesc.Usage = D3D11_USAGE_STAGING;
        StagingDesc.BindFlags = 0;
        StagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        StagingDesc.MiscFlags = 0;
        
        winrt::com_ptr<ID3D11Texture2D> StagingTexture;
        HRESULT hr = Device->CreateTexture2D (&StagingDesc, nullptr, StagingTexture.put ());
        
        if (FAILED (hr)) {
            Logger::log (hr, "Failed to create staging texture");
            Surface.Close ();
            Frame.Close ();
            return std::nullopt;
        }
        
        Context->CopyResource (StagingTexture.get (), Texture.get ());
        
        D3D11_MAPPED_SUBRESOURCE MappedResource;
        hr = Context->Map (
            StagingTexture.get (),
            0,
            D3D11_MAP_READ,
            0,
            &MappedResource
        );
        
        if (FAILED (hr)) {
            Logger::log (hr, "Failed to map texture");
            Surface.Close ();
            Frame.Close ();
            return std::nullopt;
        }
        
        // Ensure texture is unmapped in all paths - use safe RAII guard
        struct UnmapGuard {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
            winrt::com_ptr<ID3D11Texture2D> texture;
            bool shouldUnmap;
            
            UnmapGuard (Microsoft::WRL::ComPtr<ID3D11DeviceContext> c, winrt::com_ptr<ID3D11Texture2D> t) 
                : ctx (c), texture (t), shouldUnmap (true) {}
            
            ~UnmapGuard () {
                if (shouldUnmap && ctx && texture) {
                    try {
                        ctx->Unmap (texture.get (), 0);
                    } catch (...) {
                    }
                }
            }
            
            void release () { shouldUnmap = false; }
        };
        
        UnmapGuard unmapGuard (Context, StagingTexture);
        
        cv::Mat Screenshot (
            Desc.Height,
            Desc.Width,
            CV_8UC4,
            MappedResource.pData,
            MappedResource.RowPitch
        );
        
        cv::Mat Result = Screenshot.clone ();
        
        // Unmap is handled automatically by RAII guard
        
        Surface.Close ();
        Frame.Close ();
        
        return Result;
    } catch (const winrt::hresult_error& e) {
        Logger::log (
            Logger::Level::E_ERROR,
            "WinRT error in Graphics Capture: " + 
            winrt::to_string (e.message ())
        );
        
        // Reset capture state on error
        if (CaptureSession) {
            CaptureSession.Close ();
            CaptureSession = nullptr;
        }
        
        if (FramePool) {
            FramePool.Close ();
            FramePool = nullptr;
        }
        
        CaptureItem = nullptr;
        
        return std::nullopt;
    } catch (const std::exception& e) {
        Logger::log (
            Logger::Level::E_ERROR,
            std::string ("Exception in Graphics Capture: ") + e.what ()
        );
        return std::nullopt;
    } catch (...) {
        Logger::log (
            Logger::Level::E_ERROR,
            "Unknown exception in Graphics Capture"
        );
        return std::nullopt;
    }
}

winrt::IDirect3DDevice WindowsGraphicsCapture::CreateDirect3DDevice (IDXGIDevice* DxgiDevice)
{
    winrt::com_ptr<::IInspectable> Device;
    winrt::check_hresult (CreateDirect3D11DeviceFromDXGIDevice (DxgiDevice, Device.put ()));
    return Device.as<winrt::IDirect3DDevice> ();
}