#ifndef SCREEN_H
#define SCREEN_H

#include <atomic>
#include <d3d11.h>
#include <DirectXTex.h>
#include <mutex>
#include <opencv2/dnn.hpp>
#include <opencv2/core/mat.hpp>
#include <optional>
#include <tesseract/baseapi.h>
#include <vector>
#include <winrt/base.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.capture.interop.h>

namespace winrt {
    using namespace Windows::Foundation;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

struct IDXGIAdapter;
struct ID3D11DeviceContext;
struct ID3D11Device;
struct IDXGIOutput;
struct IDXGIOutput1;
struct IDXGIOutputDuplication;

namespace DirectX 
{
	class Blob;
}

class Screen 
{
	private:
		const std::vector<std::string> MODEL_OBJECTS = { "Tooltip" };

		const float MODEL_WIDTH = 640;
		const float MODEL_HEIGHT = 640;

		const double MINIMUM_OBJECT_CONFIDENCE = 0.90;

		const double NMS_SCORE_THRESHOLD = 0.45;
		const double NMS_THRESHOLD = 0.50;

		std::atomic<bool> IsInitialized;

		std::atomic<bool> UsingD3D;
		std::atomic<bool> UsingWGC;

		std::mutex CaptureLock;
		std::mutex DNNLock;
		std::mutex TesseractLock;

		std::unique_ptr<cv::dnn::Net> Net;
		std::unique_ptr<tesseract::TessBaseAPI> Tesseract;

		// WGC

		winrt::Windows::Graphics::Capture::GraphicsCaptureItem CaptureItem { nullptr };
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool FramePool { nullptr };
		winrt::Windows::Graphics::Capture::GraphicsCaptureSession CaptureSession { nullptr };
		winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice WinRTDevice { nullptr };

		// D3D

		Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context;
		Microsoft::WRL::ComPtr<ID3D11Device> Device;
		Microsoft::WRL::ComPtr<IDXGIOutputDuplication> DesktopDuplication;

		HMONITOR CachedMonitor = nullptr;

		enum class ComInitState {
			NOT_INITIALIZED,
			INITIALIZED_BY_US,
			ALREADY_INITIALIZED
		};

		ComInitState COMState = ComInitState::NOT_INITIALIZED;

		void Cleanup ();
		void CleanupWGC ();
		void CleanupD3D ();
		void D3DDump ();
		void D3DReportLiveObjects ();
		
		std::optional<cv::Mat> CaptureUsingD3D ();
		std::optional<cv::Mat> CaptureUsingGDI ();
		std::optional<cv::Mat> CaptureUsingWGC ();

	    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> GetDuplicationForMonitor (HMONITOR monitor);

		winrt::IDirect3DDevice CreateDirect3DDevice (IDXGIDevice* DxgiDevice);
	public:
		static std::string TesseractPath;
		static std::string OnnxFile;

		~Screen ();
		Screen ();

		bool Initialize (std::string CaptureMethod);
		bool InitializeWGC ();
		bool InitializeD3D ();
		std::optional<cv::Mat> Capture ();

		std::optional<std::vector<cv::Rect>> FindTooltips (cv::Mat screenshot);
		std::string Read (cv::Mat region);
};

#endif