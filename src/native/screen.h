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

		std::mutex CaptureLock;
		std::mutex DNNLock;
		std::mutex TesseractLock;

		std::unique_ptr<cv::dnn::Net> Net;
		std::unique_ptr<tesseract::TessBaseAPI> Tesseract;

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
		void CleanupD3D ();
		void D3DDump ();
		void D3DReportLiveObjects ();
		HWND FindGameWindow ();
		
		std::optional<cv::Mat> CaptureUsingD3D ();
		std::optional<cv::Mat> CaptureUsingGDI ();

	    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> GetDuplicationForMonitor (HMONITOR monitor);
		
	public:
		static std::string TesseractPath;
		static std::string OnnxFile;

		~Screen ();
		Screen ();

		bool Initialize (std::string CaptureMethod);
		bool InitializeD3D ();
		std::optional<cv::Mat> Capture ();

		std::optional<std::vector<cv::Rect>> FindTooltips (cv::Mat screenshot);
		std::string Read (cv::Mat region);
};

#endif