#include "logger.h"
#include "screen.h"
#include "util.h"
#include <d3d11_1.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <DirectXTex.h>
#include <directxtk/ScreenGrab.h>
#include <directxtk/WICTextureLoader.h>
#include <dxgi1_2.h>
#include <filesystem>
#include <iostream>
#include <opencv2/dnn.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include <optional>
#include <windows.h>
#include <winrt/base.h>

namespace winrt 
{
    using namespace Windows::Foundation;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

std::string Screen::TesseractPath = "";
std::string Screen::OnnxFile = "";

Screen::~Screen () 
{
    Cleanup ();
}

Screen::Screen ()
{
}

bool Screen::Initialize (std::string CaptureMethod) 
{
    if (TesseractPath.empty () || OnnxFile.empty ()) {
        Logger::log (
            Logger::Level::E_ERROR,
            "Paths not set: TesseractPath and OnnxFile must be set before screen initialization"
        );

        return false;
    }

    Cleanup ();

    HRESULT hr = S_OK;

    Logger::log (
        Logger::Level::E_INFO, 
        "Initializing screen"
    );

    try {
        COMState = ComInitState::NOT_INITIALIZED;

        UsingWGC = false;
        UsingD3D = false;

        if (CaptureMethod == "wgc") {
            if (InitializeWGC ()) {
                UsingWGC = true;
            }
        }

        if (CaptureMethod == "d3d") {
            if (InitializeD3D ()) {
                UsingD3D = true;
            }
        }

        if (UsingWGC) {
            Logger::log (
                Logger::Level::E_INFO,
                "Using Windows Graphic Capture API (WGC)"
            );
        } else if (UsingD3D) {
            Logger::log (
                Logger::Level::E_INFO,
                "Using DirectX Desktop Duplication API (D3D)"
            );
        } else {
            Logger::log (
                Logger::Level::E_INFO,
                "Using GDI fallback"
            );
        }
        
        if (!Tesseract) {
            Logger::log (
                Logger::Level::E_INFO, 
                "Initializing Tesseract"
            );
            
            Tesseract = std::make_unique<tesseract::TessBaseAPI> ();

            if (Tesseract->Init (TesseractPath.c_str (), "eng", tesseract::OEM_LSTM_ONLY) != 0) {
                Logger::log (
                    Logger::Level::E_ERROR,
                    "Failed to initialize tesseract using datapath: " + TesseractPath
                );

                Cleanup ();
                return false;
            }

            Tesseract->SetPageSegMode (tesseract::PSM_SINGLE_BLOCK);
            Tesseract->SetVariable ("debug_file", "/dev/null");
            Tesseract->SetVariable ("user_defined_dpi", "70");
        }

        if (!Net) {
            Net = std::make_unique<cv::dnn::Net> (
                cv::dnn::readNetFromONNX (OnnxFile)
            );

            if (Net->empty ()) {
                Logger::log (
                    Logger::Level::E_ERROR, 
                    "Failed to load tooltip recognition model from: " + OnnxFile
                );
                
                Cleanup ();
                return false;
            }

            if (cv::cuda::getCudaEnabledDeviceCount () > 0) {
                Logger::log (
                    Logger::Level::E_INFO,
                    "CUDA is available, enabling GPU acceleration"
                );

                Net->setPreferableBackend (cv::dnn::DNN_BACKEND_CUDA);
                Net->setPreferableTarget (cv::dnn::DNN_TARGET_CUDA);
            } else {
                Logger::log (
                    Logger::Level::E_INFO, 
                    "CUDA not available, using CPU"
                );
            }
        }

        Logger::log (
            Logger::Level::E_INFO, 
            "All modules successfully initialized"
        );

        IsInitialized = true;

        return true;
    } catch (std::exception& e) {
        Logger::log (
            Logger::Level::E_ERROR,
            std::string ("Exception during initialization: ") + e.what ()
        );

        Cleanup ();
        return false;
    } catch (...) {
        Logger::log (
            Logger::Level::E_ERROR,
            std::string ("Unknown exception during initialization")
        );

        Cleanup ();
        return false;
    }
}

bool Screen::InitializeWGC ()
{
    Logger::log (
        Logger::Level::E_INFO,
        "Initializing WGC"
    );

    try {
        // winrt::init_apartment ();

        if (!winrt::GraphicsCaptureSession::IsSupported ()) {
            Logger::log (
                Logger::Level::E_WARNING,
                "WGC is not supported"
            );

            return false;
        }

        if (!InitializeD3D ()) {
            Logger::log (
                Logger::Level::E_ERROR,
                "Failed to initialize D3D device needed for Graphics Capture"
            );

            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> DxgiDevice;
        HRESULT hr = Device.As (&DxgiDevice);

        winrt::com_ptr<IDXGIDevice> WinrtDxgiDevice;
        WinrtDxgiDevice.copy_from (DxgiDevice.Get ());

        if (FAILED (hr)) {
            Logger::log (hr, "Failed to get DXGI device");
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
    } catch (const winrt::hresult_error& e) {
        Logger::log (
            Logger::Level::E_ERROR,
            "WinRT error in WGC initialization: " + 
            winrt::to_string (e.message ())
        );

        return false;
    } catch (const std::exception& e) {
        Logger::log (
            Logger::Level::E_ERROR,
            std::string ("Exception in WGC initialization: ") + e.what ()
        );

        return false;
    } catch (...) {
        Logger::log (
            Logger::Level::E_ERROR,
            "Unknown exception in WGC initialization"
        );

        return false;
    }

    Logger::log (
        Logger::Level::E_INFO,
        "WGC initialized and device presumed stable"
    );

    return true;
}

bool Screen::InitializeD3D () 
{
    CleanupD3D ();

    Logger::log (
        Logger::Level::E_INFO,
        "Initializing D3D11 device"
    );

    // Create DXGI Factory to enumerate adapters
    IDXGIFactory1* Factory = nullptr;
    
    HRESULT hr = CreateDXGIFactory1 (
        __uuidof (IDXGIFactory1), 
        (void**) &Factory
    );
    
    if (FAILED (hr)) {
        Logger::log (hr, "Failed to create DXGI Factory");
        return false;
    }

    // Find the adapter with the most video memory (typically the most powerful)
    IDXGIAdapter1* SelectedAdapter = nullptr;
    SIZE_T MaxDedicatedVideoMemory = 0;
    
    for (UINT i = 0; ; i++) {
        IDXGIAdapter1* Adapter = nullptr;
        
        if (Factory->EnumAdapters1 (i, &Adapter) == DXGI_ERROR_NOT_FOUND) {
            // No more adapters to enumerate
            break;
        }
        
        DXGI_ADAPTER_DESC1 Desc;
        Adapter->GetDesc1 (&Desc);
        
        // Skip Microsoft Basic Render Driver (software adapter)
        if (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            Adapter->Release ();
            continue;
        }
        
        std::wstring wDesc (Desc.Description);
        std::string AdapterDesc (wDesc.begin (), wDesc.end ());
        
        std::ostringstream oss;

        oss << "Found adapter " << i << ": " 
            << AdapterDesc << " with " 
            << (Desc.DedicatedVideoMemory / (1024 * 1024)) << " MB VRAM";
        
        Logger::log (
            Logger::Level::E_INFO, 
            oss.str ()
        );
        
        // Select this adapter if it has more dedicated video memory
        if (Desc.DedicatedVideoMemory > MaxDedicatedVideoMemory) {
            if (SelectedAdapter) {
                SelectedAdapter->Release ();
            }
            
            SelectedAdapter = Adapter;
            SelectedAdapter->AddRef ();
            MaxDedicatedVideoMemory = Desc.DedicatedVideoMemory;
        }
        
        Adapter->Release ();
    }
    
    if (!SelectedAdapter) {
        Logger::log (
            Logger::Level::E_ERROR,
            "No suitable graphics adapter found"
        );

        Factory->Release ();
        return false;
    }
    
    // Log which adapter we selected
    DXGI_ADAPTER_DESC1 SelectedDesc;
    SelectedAdapter->GetDesc1 (&SelectedDesc);

    std::wstring wDesc (SelectedDesc.Description);
    std::string AdapterDesc (wDesc.begin (), wDesc.end ());
    
    std::ostringstream oss;
    oss << "Selected adapter: " << AdapterDesc << " with " 
        << (SelectedDesc.DedicatedVideoMemory / (1024 * 1024)) << " MB VRAM";
    
    Logger::log (
        Logger::Level::E_INFO, 
        oss.str ()
    );

    D3D_FEATURE_LEVEL FeatureLevels [] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_1
    };

    UINT NumFeatureLevels = ARRAYSIZE (FeatureLevels);

    D3D_FEATURE_LEVEL FeatureLevel;

    hr = D3D11CreateDevice (
        SelectedAdapter, 
        D3D_DRIVER_TYPE_UNKNOWN, 
        nullptr, 
        0,
        FeatureLevels, 
        NumFeatureLevels, 
        D3D11_SDK_VERSION,
        &Device, 
        &FeatureLevel, 
        &Context
    );

    if (FAILED (hr)) {
        Logger::log (hr, "Failed to create D3D11 device");
        return false;
    }

    Logger::log (
        Logger::Level::E_INFO,
        "D3D11 device created with feature level: " + std::to_string (((FeatureLevel >> 12) & 0xF)) + "." + std::to_string (((FeatureLevel >> 8) & 0xF))
    );

    // Ensure device stability
    std::this_thread::sleep_for (std::chrono::milliseconds (100));
    
    Logger::log (
        Logger::Level::E_INFO,
        "D3D11 initialized and device presumed stable"
    );

    return true;
}

void Screen::Cleanup () 
{
    Logger::log (
        Logger::Level::E_INFO, 
        "Cleaning up all screen resources"
    );

    if (COMState == ComInitState::INITIALIZED_BY_US) {
        CoUninitialize ();
    }

    CleanupWGC ();
    CleanupD3D ();
    
    if (Tesseract) {
        Tesseract->End ();
        Tesseract.reset ();
    }
    
    if (Net) {
        Net.reset ();
    }

    IsInitialized = false;
}

void Screen::CleanupWGC ()
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
        
        Logger::log (
            Logger::Level::E_INFO, 
            "WGC resources cleaned up"
        );
    } catch (const std::exception& e) {
        Logger::log (
            Logger::Level::E_WARNING,
            std::string ("Exception in WGC cleanup: ") + e.what ()
        );

        return;
    } catch (...) {
        Logger::log (
            Logger::Level::E_WARNING,
            "Unknown exception in WGC cleanup"
        );

        return;
    }

    Logger::log (Logger::Level::E_INFO, "WGC resources cleaned up");
}

void Screen::CleanupD3D () 
{
    if (DesktopDuplication) {
        try {
            DesktopDuplication->ReleaseFrame ();
        } catch (const std::exception& e) {
            Logger::log (
                Logger::Level::E_WARNING, 
                std::string ("Exception releasing frame during cleanup: ") + e.what ()
            );
        } catch (...) {
            Logger::log (
                Logger::Level::E_WARNING, 
                "Unknown exception releasing frame during cleanup"
            );
        }

        DesktopDuplication.Reset ();
    }

    if (Context) Context.Reset ();
    if (Device) Device.Reset ();
    
    Logger::log (Logger::Level::E_INFO, "DirectX resources cleaned up");
}

std::optional<cv::Mat> Screen::Capture () 
{
    if (!IsInitialized) {
        throw std::runtime_error ("Cannot capture screen before initialization");
    }

    std::lock_guard<std::mutex> Lock (CaptureLock);

    if (UsingWGC) {
        auto Result = CaptureUsingWGC ();
        
        if (Result) {
            return Result;
        }

        Logger::log (
            Logger::Level::E_WARNING,
            "WGC capture failed, trying D3D"  
        );
    }

    if (UsingD3D) {
        auto Result = CaptureUsingD3D ();

        if (Result) {
            return Result;
        }

        Logger::log (
            Logger::Level::E_WARNING,
            "D3D capture failed, trying GDI"  
        );
    }

    auto Result = CaptureUsingGDI ();

    if (Result) {
        return Result;
    }

    Logger::log (
        Logger::Level::E_WARNING,
        "GDI capture failed"  
    );

    Logger::log (
        Logger::Level::E_ERROR,
        "All capture methods failed"
    );

    return std::nullopt;
}

std::optional<cv::Mat> Screen::CaptureUsingWGC ()
{
    if (!UsingWGC) {
        return std::nullopt;
    }

    try {
        HWND GameWindow = FindGameWindow ();
        
        if (!GameWindow) {
            return std::nullopt;
        }

        auto InteropFactory = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem
        >().as<IGraphicsCaptureItemInterop> ();

        bool NeedsNewSession = false;
        
        if (!CaptureItem) {
            NeedsNewSession = true;
            
            winrt::GraphicsCaptureItem Item { nullptr };
            winrt::check_hresult (InteropFactory->CreateForWindow (
                GameWindow,
                winrt::guid_of<winrt::GraphicsCaptureItem> (),
                winrt::put_abi (Item)));
                
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
            
            auto size = CaptureItem.Size ();

            FramePool = winrt::Direct3D11CaptureFramePool::Create (
                WinRTDevice,
                winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                1, // Number of frames in pool
                size);
                
            CaptureSession = FramePool.CreateCaptureSession (CaptureItem);

            CaptureSession.StartCapture ();
            
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }

        // Discard potentially stale frames
        for (int i = 0; i < 3; i++) {
            auto discardFrame = FramePool.TryGetNextFrame ();

            if (discardFrame) {
                discardFrame.Close ();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        auto Frame = FramePool.TryGetNextFrame();

        if (!Frame) {
            Logger::log (
                Logger::Level::E_WARNING,
                "Failed to get capture frame"
            );

            return std::nullopt;
        }

        auto Surface = Frame.Surface ();

        auto Access = Surface.as <Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> ();
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
        
        Context->CopyResource (
            StagingTexture.get (), 
            Texture.get ()
        );
        
        D3D11_MAPPED_SUBRESOURCE MappedResource;
        hr = Context->Map(
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
        
        cv::Mat Screenshot (
            Desc.Height,
            Desc.Width,
            CV_8UC4,
            MappedResource.pData,
            MappedResource.RowPitch
        );
        
        cv::Mat Result = Screenshot.clone ();

        // cv::imwrite ("screenshot.png", Result);

        Context->Unmap (StagingTexture.get (), 0);
        
        Surface.Close ();
        Frame.Close ();

        return Result;
    } catch (const winrt::hresult_error& e) {
        Logger::log (
            Logger::Level::E_ERROR,
            "WinRT error in Graphics Capture: " + 
            winrt::to_string (e.message ())
        );
        
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
            std::string ("Exception in Graphics Capture: ") + e.what()
        );

        return std::nullopt;
    } catch (...) {
        Logger::log(
            Logger::Level::E_ERROR,
            "Unknown exception in WGC capture"
        );

        return std::nullopt;
    }
}

std::optional<cv::Mat> Screen::CaptureUsingD3D ()
{
    if (!UsingD3D) {
        return std::nullopt;
    }

    HWND GameWindow = FindGameWindow ();

    if (!GameWindow) {
        return std::nullopt;
    }

    auto SafeReleaseFrame = [this] () {
        if (DesktopDuplication) {
            try {
                DesktopDuplication->ReleaseFrame ();
            } catch (...) {
                Logger::log (
                    Logger::Level::E_WARNING, 
                    "Exception caught when releasing frame"
                );
            }
        }
    };

    HMONITOR Monitor = MonitorFromWindow (GameWindow, MONITOR_DEFAULTTONEAREST);

    if (Monitor != CachedMonitor) {
        Logger::log (
            Logger::Level::E_INFO,
            "Game changed monitors"
        );

        auto NewDuplication = GetDuplicationForMonitor (Monitor);

        if (!NewDuplication) {
            Logger::log (
                Logger::Level::E_WARNING, 
                "Failed to get output duplication"
            );

            return std::nullopt;
        }

        DesktopDuplication = NewDuplication;
        CachedMonitor = Monitor;
    }

    using namespace DirectX;

    DXGI_OUTDUPL_FRAME_INFO FrameInfo = {};

    Microsoft::WRL::ComPtr<IDXGIResource> DesktopResource;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> AcquiredDesktopImage;

    HRESULT hr = S_OK;

    D3DDump ();

    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            std::this_thread::sleep_for (std::chrono::milliseconds (500));
        }

        // Logger::log (
        //     Logger::Level::E_INFO,
        //     "Attemping to acquire duplication frame on attempt #" + std::to_string (i + 1)
        // );

        SafeReleaseFrame ();

        hr = DesktopDuplication->AcquireNextFrame (
            500, 
            &FrameInfo, 
            &DesktopResource
        );

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            Logger::log (
                Logger::Level::E_WARNING, 
                "DXGI access lost, reinitializing and trying again"
            );

            if (!InitializeD3D ()) {
                Logger::log (
                    Logger::Level::E_ERROR,
                    "Failed to reinitialize after access lost"
                );

                break;
            }

            auto NewDuplication = GetDuplicationForMonitor (CachedMonitor);

            if (!NewDuplication) {
                Logger::log (
                    Logger::Level::E_ERROR,
                    "Failed to recreate duplication after D3D reinitialization"
                );

                break;
            }

            DesktopDuplication = NewDuplication;
            continue;
        }

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            Logger::log (
                Logger::Level::E_INFO,
                "Frame timeout, trying again"
            );

            continue;
        }

        if (FAILED (hr))  {
            Logger::log (hr, "Failed to acquire frame");
            
            D3DDump ();
            
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
            continue;
        }

        break;
    }

    if (FAILED (hr)) {
        Logger::log (hr, "Failed to acquire frame after all attempts");
        return std::nullopt;
    }
    
    Logger::log (
        Logger::Level::E_DEBUG,
        "Frame successfully acquired"
    );

    DesktopResource.As (&AcquiredDesktopImage);

    DirectX::ScratchImage image;

    hr = DirectX::CaptureTexture (
        Device.Get (), 
        Context.Get (),
        AcquiredDesktopImage.Get (), 
        image
    );

    SafeReleaseFrame ();

    if (FAILED (hr)) {
        Logger::log (hr, "Failed to capture texture");
        return std::nullopt;
    }

    DirectX::Blob blob;

    hr = SaveToWICMemory (
        image.GetImages (), 
        image.GetImageCount (),
        DirectX::WIC_FLAGS_NONE,
        DirectX::GetWICCodec (DirectX::WIC_CODEC_BMP), 
        blob
    );

    if (FAILED (hr)) {
        Logger::log (hr, "Failed to save WIC");
        return std::nullopt;
    }

    const uint8_t *bufferPointer =
            static_cast<const uint8_t *>(blob.GetBufferPointer ());

    std::vector<uint8_t> buffer (
        bufferPointer, 
        bufferPointer + blob.GetBufferSize ()
    );

    return cv::imdecode (buffer, cv::IMREAD_UNCHANGED);
}

std::optional<cv::Mat> Screen::CaptureUsingGDI ()
{
    HWND GameWindow = FindGameWindow ();
    
    if (!GameWindow) {
        return std::nullopt;
    }

    try {
        RECT ClientRect;
        if (!GetClientRect (GameWindow, &ClientRect)) {
            Logger::log (
                Logger::Level::E_WARNING,
                "Failed to get window client rectangle"
            );

            return std::nullopt;
        }

        POINT TopLeft = { ClientRect.left, ClientRect.top };
        POINT BottomRight = { ClientRect.right, ClientRect.bottom };

        ClientToScreen (GameWindow, &TopLeft);
        ClientToScreen (GameWindow, &BottomRight);

        int Width = BottomRight.x - TopLeft.x;
        int Height = BottomRight.y - TopLeft.y;
        
        if (Width <= 0 || Height <= 0) {
            Logger::log (
                Logger::Level::E_WARNING,
                "Invalid window dimensions: " + std::to_string (Width) + "x" + std::to_string (Height)
            );
            return std::nullopt;
        }

        HDC ScreenDC = GetDC (NULL);
        
        if (!ScreenDC) {
            Logger::log (
                Logger::Level::E_WARNING,
                "Failed to get screen DC"
            );

            return std::nullopt;
        }
        
        HDC MemDC = CreateCompatibleDC (ScreenDC);

        if (!MemDC) {
            ReleaseDC (NULL, ScreenDC);

            Logger::log (
                Logger::Level::E_WARNING,
                "Failed to create compatible device context"
            );

            return std::nullopt;
        }

        HBITMAP Bitmap = CreateCompatibleBitmap (ScreenDC, Width, Height);
        
        if (!Bitmap) {
            DeleteDC (MemDC);
            ReleaseDC (NULL, ScreenDC);
            
            Logger::log (
                Logger::Level::E_WARNING,
                "Failed to create compatible bitmap"
            );

            return std::nullopt;
        }

        HBITMAP OldBitmap = (HBITMAP) SelectObject (MemDC, Bitmap);
        
        if (!BitBlt (MemDC, 0, 0, Width, Height, ScreenDC, TopLeft.x, TopLeft.y, SRCCOPY)) {
            SelectObject (MemDC, OldBitmap);
            DeleteObject (Bitmap);
            DeleteDC (MemDC);
            ReleaseDC (NULL, ScreenDC);
            
            Logger::log (
                Logger::Level::E_WARNING,
                "BitBlt failed"
            );
            
            return std::nullopt;
        }

        BITMAPINFOHEADER BI;
        BI.biSize = sizeof (BITMAPINFOHEADER);
        BI.biWidth = Width;
        BI.biHeight = -Height;
        BI.biPlanes = 1;
        BI.biBitCount = 32;
        BI.biCompression = BI_RGB;
        BI.biSizeImage = 0;
        BI.biXPelsPerMeter = 0;
        BI.biYPelsPerMeter = 0;
        BI.biClrUsed = 0;
        BI.biClrImportant = 0;

        cv::Mat Screenshot (Height, Width, CV_8UC4);
        
        GetDIBits (MemDC, Bitmap, 0, Height, Screenshot.data, (BITMAPINFO*)&BI, DIB_RGB_COLORS);

        SelectObject (MemDC, OldBitmap);
        DeleteObject (Bitmap);
        DeleteDC (MemDC);
        ReleaseDC (NULL, ScreenDC);

        return Screenshot;
    } catch (const std::exception& e) {
        Logger::log (
            Logger::Level::E_ERROR,
            std::string ("Exception capturing using GDI: ") + e.what()
        );
        
        return std::nullopt;
    } catch (...) {
        Logger::log (
            Logger::Level::E_ERROR,
            "Unknown exception while trying to capture using GDI"
        );

        return std::nullopt;
    }
}

std::optional<std::vector<cv::Rect>> Screen::FindTooltips (cv::Mat screenshot) 
{
    if (!IsInitialized) {
        throw std::runtime_error ("Cannot find tooltip before initialization");
    }

    std::lock_guard<std::mutex> Lock (DNNLock);

    if (screenshot.channels () == 4) {
        cv::cvtColor (screenshot, screenshot, cv::COLOR_BGRA2BGR);
    }

    int max = std::max (screenshot.cols, screenshot.rows);

    cv::Mat resized;

    resized = cv::Mat::zeros (max, max, CV_8UC3);
    screenshot.copyTo (resized (cv::Rect (0, 0, screenshot.cols, screenshot.rows)));

    cv::Mat frame;

    cv::dnn::blobFromImage (
        resized, 
        frame, 
        1 / 255.0,
        cv::Size (MODEL_WIDTH, MODEL_HEIGHT), 
        cv::Scalar (),
        true, 
        false
    );

    Net->setInput (frame);

    std::vector<cv::Mat> outputs;

    Net->forward (outputs, Net->getUnconnectedOutLayersNames ());

    // -- -- //

    int rows = outputs [0].size [2];
    int dimensions = outputs [0].size [1];

    outputs [0] = outputs [0].reshape (1, dimensions);
    cv::transpose (outputs [0], outputs [0]);

    float *data = (float *) outputs [0].data;

    float xScale = (float) resized.cols / MODEL_WIDTH;
    float yScale = (float) resized.rows / MODEL_HEIGHT;

    std::vector<int> classIds;
    std::vector<float> confidences;
    std::vector<cv::Rect> boxes;

    for (int i = 0; i < rows; ++i) {
        float *classesScores = data + 4;

        cv::Mat scores (1, MODEL_OBJECTS.size (), CV_32FC1, classesScores);
        cv::Point classId;

        double maxClassScore;

        cv::minMaxLoc (scores, 0, &maxClassScore, 0, &classId);

        if (maxClassScore > MINIMUM_OBJECT_CONFIDENCE) {
            confidences.push_back (maxClassScore);
            classIds.push_back (classId.x);

            float x = data [0];
            float y = data [1];
            float w = data [2];
            float h = data [3];

            int left = (int) ((x - 0.5 * w) * xScale);
            int top = (int) ((y - 0.5 * h) * yScale);

            int width = (int) (w * xScale);
            int height = (int) (h * yScale);

            boxes.push_back (cv::Rect (left, top, width, height));
        }

        data += dimensions;
    }

    // Non-maximum supression to remove redundant boxes.
    std::vector<int> nms;

    cv::dnn::NMSBoxes (
        boxes, 
        confidences, 
        NMS_SCORE_THRESHOLD, 
        NMS_THRESHOLD,
        nms
    );

    if (nms.size () <= 0) {
        return std::nullopt;
    }

    std::vector<cv::Rect> tooltips;

    for (int idx : nms) {
        tooltips.push_back (boxes [idx]);
    }

    return tooltips;
}

std::string Screen::Read (cv::Mat region) 
{
    if (!IsInitialized) {
        throw std::runtime_error ("Cannot run OCR before initialization");
    }

    // cv::imwrite ("tooltip.png", region);

    std::lock_guard<std::mutex> Lock (TesseractLock);

    cv::Mat processed = cv::Mat::zeros (
        region.size (), 
        region.type ()
    );

    // Alpha - contrast control (1.0 - 3.0)
    // Brightness control - 0 (0 - 100)
    region.convertTo (processed, -1, 2, 0);

    // Trim border
    int borderSize = 5;

    cv::Rect roi(
        borderSize, 
        borderSize, 
        processed.cols - 2 * borderSize,
        processed.rows - 2 * borderSize);

    processed = processed (roi);

    cv::Mat grayscale;
    cv::Mat binary;
    cv::Mat sharpened;

    cv::cvtColor (
        processed, 
        grayscale, 
        cv::COLOR_BGR2GRAY
    );

    cv::threshold (
        grayscale, 
        binary, 0, 255,
        cv::THRESH_BINARY_INV | cv::THRESH_OTSU
    );

    cv::bilateralFilter (binary, sharpened, 5, 75, 75);

    // cv::imwrite ("image.png", sharpened);

    Tesseract->SetImage (
        sharpened.data, 
        sharpened.cols, 
        sharpened.rows,
        sharpened.channels (), 
        sharpened.step
    );

    std::unique_ptr<char[]> text (Tesseract->GetUTF8Text ());

    if (text) {
        return std::string (text.get ());
    } else {
        Logger::log (
            Logger::Level::E_ERROR,
            "Failed to extract text from image with Tesseract"
        );
    }

    return std::string ();
}

void Screen::D3DDump () 
{
    Microsoft::WRL::ComPtr<ID3D11Debug> Debug;

    if (SUCCEEDED (Device.As (&Debug))) {
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> InfoQueue;

        if (SUCCEEDED (Debug.As (&InfoQueue))) {
            UINT64 NumMessages = InfoQueue->GetNumStoredMessages ();
            
            for (UINT64 i = 0; i < NumMessages; i++) {
                SIZE_T MessageLength = 0;
                InfoQueue->GetMessage (i, nullptr, &MessageLength);
                
                std::vector<uint8_t> Bytes (MessageLength);
                D3D11_MESSAGE* Message = reinterpret_cast<D3D11_MESSAGE*>(Bytes.data());
                
                if (SUCCEEDED (InfoQueue->GetMessage (i, Message, &MessageLength))) {
                    Logger::log (
                        Logger::Level::E_DEBUG, 
                        std::string ("D3D Debug: ") + Message->pDescription
                    );
                }
            }
            
            InfoQueue->ClearStoredMessages ();
        }
    }
}

void Screen::D3DReportLiveObjects () 
{
    if (Device) {
        Microsoft::WRL::ComPtr<ID3D11Debug> Debug;

        if (SUCCEEDED (Device.As (&Debug))) {
            Debug->ReportLiveDeviceObjects (D3D11_RLDO_DETAIL);
        }
    }
}

Microsoft::WRL::ComPtr<IDXGIOutputDuplication> Screen::GetDuplicationForMonitor (HMONITOR Monitor) 
{
    if (!Monitor) {
        Logger::log (
            Logger::Level::E_WARNING,
            "Passed invalid monitor to GetDuplicationForMonitor"    
        );

        return nullptr;
    }

    HRESULT hr;

    Microsoft::WRL::ComPtr<IDXGIDevice> DxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> Adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> DxgiOutput;
    Microsoft::WRL::ComPtr<IDXGIOutput1> DxgiOutput1;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> Duplication;

    hr = Device.As (&DxgiDevice);

    if (FAILED (hr)) {
        Logger::log (hr, "Failed to get output for device");
        return nullptr;
    }

    hr = DxgiDevice->GetAdapter (&Adapter);

    if (FAILED (hr)) {
        Logger::log (hr, "Failed to get device adapter");
        return nullptr;
    }

    // hr = Adapter->EnumOutputs (0, &DxgiOutput);

    // if (FAILED (hr)) {
    //     Logger::log (hr, "Failed to enumerate device outputs");
    //     return nullptr;
    // }

    // hr = DxgiOutput.As (&DxgiOutput1);

    // if (FAILED (hr)) {
    //     Logger::log (hr, "Failed to get DXGI output1");
    //     return nullptr;
    // }

    // hr = DxgiOutput1->DuplicateOutput (DxgiDevice.Get (), &DesktopDuplication);

    // if (FAILED (hr)) {
    //     Logger::log (hr, "Failed to duplicate output");
    //     return nullptr;
    // }
    
    for (UINT i = 0;; i++) {
        DxgiOutput.Reset (); 

        hr = Adapter->EnumOutputs (i, &DxgiOutput);

        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        if (FAILED (hr)) {
            Logger::log (hr, "Failed to enumerate output device: " + std::to_string (i));
            break;
        }

        DXGI_OUTPUT_DESC Desc;
        hr = DxgiOutput->GetDesc (&Desc);
        
        if (FAILED (hr)) {
            Logger::log (hr, "Failed to get DxgiOutput description");
            continue;
        }

        if (Desc.Monitor == Monitor) {
            hr = DxgiOutput.As (&DxgiOutput1);

            if (FAILED (hr)) {
                Logger::log (hr, "Failed to get DxgiOutput1");
                continue;
            }
            
            hr = DxgiOutput1->DuplicateOutput (
                DxgiDevice.Get (), 
                &Duplication
            );

            if (FAILED (hr)) {
                Logger::log (hr, "Failed to duplicate output");
                continue;
            }

            if (Duplication) {
                Logger::log (
                    Logger::Level::E_INFO,
                    "Duplication successfully created for monitor"    
                );

                return Duplication;
            }
        }
    }
    
    Logger::log (
        Logger::Level::E_ERROR,
        "Failed to find adapter output that matches monitor"
    );
    
    return nullptr;
}

winrt::IDirect3DDevice Screen::CreateDirect3DDevice (IDXGIDevice* DxgiDevice)
{
    winrt::com_ptr<::IInspectable> Device;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice (DxgiDevice, Device.put ()));
    return Device.as <winrt::IDirect3DDevice> ();
}