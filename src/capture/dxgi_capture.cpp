#include "dxgi_capture.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// Capture loop structure adapted from screen_capture_lite's DXFrameProcessor
// (MIT License, Copyright (c) 2017 Scott). This implementation is intentionally
// local: DXGI/D3D objects never leave the producer thread.

namespace gta5::capture::dxgi {
namespace {

bool SameRect(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top &&
         a.right == b.right && a.bottom == b.bottom;
}

struct CpuSnapshot {
  RECT desktop{};
  int width = 0;
  int height = 0;
  LONGLONG captureQpc = 0;
  std::uint64_t sequence = 0;
  std::vector<std::uint32_t> bgra;
};

struct DeliveredRegion {
  bool fullFrame = false;
  RECT rect{};
};

class Producer {
 public:
  ~Producer() { stop(); }

  CaptureStatus latest(GameFrame& frame, const RECT& client,
                       const RECT* screenRegion) {
    ensureRunning(client);

    CpuSnapshot snapshot;
    {
      std::unique_lock<std::mutex> lock(snapshotMutex_);
      const auto isRequestedRegionNew = [&] {
        if (failed_ || !latest_.sequence) return failed_;
        if (deliveredSequence_ != latest_.sequence) return true;
        return std::none_of(
            deliveredRegions_.begin(), deliveredRegions_.end(),
            [&](const DeliveredRegion& delivered) {
              return delivered.fullFrame == (screenRegion == nullptr) &&
                     (!screenRegion || SameRect(delivered.rect, *screenRegion));
            });
      };
      snapshotReady_.wait_for(lock, std::chrono::milliseconds(100),
                              isRequestedRegionNew);
      if (failed_) return CaptureStatus::Error;
      if (!latest_.sequence) return CaptureStatus::NoNewFrame;
      if (deliveredSequence_ != latest_.sequence) {
        deliveredSequence_ = latest_.sequence;
        deliveredRegions_.clear();
      }
      const DeliveredRegion requested{screenRegion == nullptr,
                                      screenRegion ? *screenRegion : RECT{}};
      const auto alreadyDelivered = std::find_if(
          deliveredRegions_.begin(), deliveredRegions_.end(),
          [&](const DeliveredRegion& delivered) {
            return delivered.fullFrame == requested.fullFrame &&
                   (requested.fullFrame || SameRect(delivered.rect, requested.rect));
          });
      if (alreadyDelivered != deliveredRegions_.end()) return CaptureStatus::NoNewFrame;
      deliveredRegions_.push_back(requested);
      snapshot = latest_;
    }

    RECT source = client;
    if (screenRegion && !IntersectRect(&source, &client, screenRegion)) {
      return CaptureStatus::Error;
    }
    if (source.left < snapshot.desktop.left || source.top < snapshot.desktop.top ||
        source.right > snapshot.desktop.right || source.bottom > snapshot.desktop.bottom) {
      return CaptureStatus::NoNewFrame;
    }

    const int clientHeight = client.bottom - client.top;
    const int sourceWidth = source.right - source.left;
    const int sourceHeight = source.bottom - source.top;
    const double scale = clientHeight > 1080 ? 1080.0 / clientHeight : 1.0;
    const int width = std::max(1, static_cast<int>(std::lround(sourceWidth * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(sourceHeight * scale)));
    frame.bgra.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
      const int sy = std::min(sourceHeight - 1, static_cast<int>(y / scale)) +
                     source.top - snapshot.desktop.top;
      for (int x = 0; x < width; ++x) {
        const int sx = std::min(sourceWidth - 1, static_cast<int>(x / scale)) +
                       source.left - snapshot.desktop.left;
        frame.bgra[static_cast<size_t>(y) * width + x] =
            snapshot.bgra[static_cast<size_t>(sy) * snapshot.width + sx];
      }
    }
    frame.screenX = source.left;
    frame.screenY = source.top;
    frame.screenW = sourceWidth;
    frame.screenH = sourceHeight;
    frame.width = width;
    frame.height = height;
    frame.clientWidth = client.right - client.left;
    frame.clientHeight = clientHeight;
    frame.captureQpc = snapshot.captureQpc;
    frame.toScreenX = sourceWidth / static_cast<double>(width);
    frame.toScreenY = sourceHeight / static_cast<double>(height);
    return CaptureStatus::NewFrame;
  }

  void stop() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    stopUnlocked();
  }

 private:
  void stopUnlocked() {
    stopRequested_.store(true, std::memory_order_relaxed);
    snapshotReady_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_ = false;
    requestedClient_ = {};
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    latest_ = {};
    deliveredSequence_ = 0;
    deliveredRegions_.clear();
    failed_ = false;
  }

  void ensureRunning(const RECT& client) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (running_ && SameRect(requestedClient_, client)) return;
    stopUnlocked();
    requestedClient_ = client;
    stopRequested_.store(false, std::memory_order_relaxed);
    running_ = true;
    thread_ = std::thread([this, client] { run(client); });
  }

  void setFailed() {
    {
      std::lock_guard<std::mutex> lock(snapshotMutex_);
      failed_ = true;
    }
    snapshotReady_.notify_all();
  }

  void run(const RECT client) {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* staging = nullptr;
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* selectedAdapter = nullptr;
    IDXGIOutput* selectedOutput = nullptr;
    RECT desktop{};

    auto cleanup = [&] {
      if (staging) staging->Release();
      if (duplication) duplication->Release();
      if (context) context->Release();
      if (device) device->Release();
      if (selectedOutput) selectedOutput->Release();
      if (selectedAdapter) selectedAdapter->Release();
      if (factory) factory->Release();
    };

    const HMONITOR monitor = MonitorFromRect(&client, MONITOR_DEFAULTTONEAREST);
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(&factory));
    for (UINT ai = 0; SUCCEEDED(hr) && !selectedOutput; ++ai) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;
      for (UINT oi = 0;; ++oi) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc{};
        output->GetDesc(&desc);
        if (desc.Monitor == monitor) {
          selectedAdapter = adapter;
          selectedAdapter->AddRef();
          selectedOutput = output;
          desktop = desc.DesktopCoordinates;
          break;
        }
        output->Release();
      }
      adapter->Release();
    }
    if (!selectedAdapter || !selectedOutput) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
      hr = D3D11CreateDevice(selectedAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                             D3D11_SDK_VERSION, &device, nullptr, &context);
    }
    IDXGIOutput1* output1 = nullptr;
    if (SUCCEEDED(hr)) {
      hr = selectedOutput->QueryInterface(__uuidof(IDXGIOutput1),
                                          reinterpret_cast<void**>(&output1));
    }
    if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(device, &duplication);
    if (output1) output1->Release();

    const int width = desktop.right - desktop.left;
    const int height = desktop.bottom - desktop.top;
    if (SUCCEEDED(hr)) {
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = width;
      desc.Height = height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      hr = device->CreateTexture2D(&desc, nullptr, &staging);
    }
    if (FAILED(hr)) {
      setFailed();
      cleanup();
      return;
    }

    std::uint64_t sequence = 0;
    while (!stopRequested_.load(std::memory_order_relaxed)) {
      IDXGIResource* resource = nullptr;
      DXGI_OUTDUPL_FRAME_INFO info{};
      hr = duplication->AcquireNextFrame(100, &info, &resource);
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
      if (FAILED(hr)) {
        setFailed();
        break;
      }

      ID3D11Texture2D* texture = nullptr;
      hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&texture));
      resource->Release();
      if (SUCCEEDED(hr) && info.AccumulatedFrames > 0) {
        context->CopyResource(staging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
          CpuSnapshot next;
          next.desktop = desktop;
          next.width = width;
          next.height = height;
          next.sequence = ++sequence;
          next.bgra.resize(static_cast<size_t>(width) * height);
          for (int y = 0; y < height; ++y) {
            std::memcpy(next.bgra.data() + static_cast<size_t>(y) * width,
                        static_cast<const std::uint8_t*>(mapped.pData) +
                            static_cast<size_t>(y) * mapped.RowPitch,
                        static_cast<size_t>(width) * sizeof(std::uint32_t));
          }
          context->Unmap(staging, 0);
          LARGE_INTEGER capturedAt{};
          QueryPerformanceCounter(&capturedAt);
          next.captureQpc = capturedAt.QuadPart;
          {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            latest_ = std::move(next);
            failed_ = false;
          }
          snapshotReady_.notify_all();
        }
      }
      if (texture) texture->Release();
      const HRESULT releaseResult = duplication->ReleaseFrame();
      if (FAILED(hr) || FAILED(releaseResult)) {
        setFailed();
        break;
      }
    }
    cleanup();
  }

  std::mutex controlMutex_;
  std::mutex snapshotMutex_;
  std::condition_variable snapshotReady_;
  std::thread thread_;
  std::atomic<bool> stopRequested_{false};
  bool running_ = false;
  bool failed_ = false;
  RECT requestedClient_{};
  CpuSnapshot latest_;
  std::uint64_t deliveredSequence_ = 0;
  std::vector<DeliveredRegion> deliveredRegions_;
};

Producer gProducer;

}  // namespace

CaptureStatus CaptureLatest(GameFrame& frame, const RECT& client,
                            const RECT* screenRegion) {
  return gProducer.latest(frame, client, screenRegion);
}

void Stop() { gProducer.stop(); }

}  // namespace gta5::capture::dxgi
