#include "vr_openxr.h"

#include <rex/cvar.h>
#include <rex/graphics/vr_bridge.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DECLARE(double, vr_screen_distance);
REXCVAR_DECLARE(double, vr_screen_width);
REXCVAR_DECLARE(bool, vr_show_mirror_quad);
REXCVAR_DECLARE(std::string, vr_quad_rect);
REXCVAR_DECLARE(std::string, vr_pace);

#if !VIG8_HAS_OPENXR

namespace vig8::vr {

StartResult OpenXrStart(uint32_t*, int32_t*, uint32_t*) { return StartResult::kNoLoader; }
void OpenXrOnDevice(void*, void*) {}
void OpenXrShutdown() {}
bool OpenXrRunning() { return false; }
bool OpenXrReadPose(RawStereo&) { return false; }
void OpenXrPresent(const SubmittedFrame&) {}

}  // namespace vig8::vr

#else

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#ifndef XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace vig8::vr {
namespace {

using Microsoft::WRL::ComPtr;

struct Fns {
  PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
  PFN_xrCreateInstance CreateInstance = nullptr;
  PFN_xrDestroyInstance DestroyInstance = nullptr;
  PFN_xrGetSystem GetSystem = nullptr;
  PFN_xrGetSystemProperties GetSystemProperties = nullptr;
  PFN_xrEnumerateEnvironmentBlendModes EnumerateEnvironmentBlendModes = nullptr;
  PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews = nullptr;
  PFN_xrCreateSession CreateSession = nullptr;
  PFN_xrDestroySession DestroySession = nullptr;
  PFN_xrBeginSession BeginSession = nullptr;
  PFN_xrEndSession EndSession = nullptr;
  PFN_xrPollEvent PollEvent = nullptr;
  PFN_xrWaitFrame WaitFrame = nullptr;
  PFN_xrBeginFrame BeginFrame = nullptr;
  PFN_xrEndFrame EndFrame = nullptr;
  PFN_xrLocateViews LocateViews = nullptr;
  PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
  PFN_xrDestroySpace DestroySpace = nullptr;
  PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
  PFN_xrCreateSwapchain CreateSwapchain = nullptr;
  PFN_xrDestroySwapchain DestroySwapchain = nullptr;
  PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
  PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
  PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
  PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
  PFN_xrGetD3D12GraphicsRequirementsKHR GetD3D12GraphicsRequirementsKHR = nullptr;
};

struct EyeSwap {
  XrSwapchain handle = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageD3D12KHR> images;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t acquired = 0;
  bool have_image = false;
  std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
};

struct GpuFrame {
  RawStereo raw{};
  uint32_t layout = 3;
  uint32_t eye = 0;
  bool pose_valid = false;
  int slot = 0;
  uint64_t copy_value = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

Fns g;
HMODULE g_dll = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
XrEnvironmentBlendMode g_blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
EyeSwap g_eye_sc[2]{};
EyeSwap g_quad_sc{};
int64_t g_format = 0;
bool g_session_begun = false;
std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_accept{false};
std::atomic<int> g_in_present{0};
std::thread g_thread;

std::mutex g_pose_mu;
RawStereo g_latest{};

std::mutex g_frame_mu;
std::vector<GpuFrame> g_frames;
std::condition_variable g_frame_cv;

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12Fence> g_copy_fence;
ComPtr<ID3D12Resource> g_staging[3];
ComPtr<ID3D12CommandAllocator> g_copy_alloc[3];
ComPtr<ID3D12GraphicsCommandList> g_copy_list[3];
uint64_t g_copy_value[3]{};
uint64_t g_copy_serial = 0;
uint32_t g_staging_w = 0;
uint32_t g_staging_h = 0;
bool g_staging_is_srv[3]{};

ComPtr<ID3D12RootSignature> g_root;
ComPtr<ID3D12PipelineState> g_pso;
ComPtr<ID3D12DescriptorHeap> g_heap;
ComPtr<ID3D12DescriptorHeap> g_rtv_heap;
uint32_t g_rtv_used = 0;
std::recursive_mutex g_d3d_mu;
std::atomic<int> g_slot_state[3]{};
uint64_t g_slot_fence[3]{};
ComPtr<ID3D12CommandAllocator> g_blit_alloc[2];
ComPtr<ID3D12GraphicsCommandList> g_blit_list[2];
ComPtr<ID3D12Fence> g_blit_fence;
uint64_t g_blit_value = 0;
int g_blit_which = 0;
DXGI_FORMAT g_rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
bool g_pso_ready = false;
bool g_copy_ready = false;
int g_last_slot = 0;

constexpr int64_t kFormatPref[] = {
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
};

bool Ok(XrResult r, const char* what) {
  if (XR_SUCCEEDED(r)) return true;
  REXLOG_ERROR("VR: {} failed ({})", what, static_cast<int>(r));
  return false;
}

template <typename Fn>
bool LoadFn(XrInstance instance, const char* name, Fn& out) {
  out = nullptr;
  XrResult r = g.GetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&out));
  if (XR_FAILED(r) || !out) {
    REXLOG_ERROR("VR: loader is missing {}", name);
    return false;
  }
  return true;
}

bool LoadDispatch(XrInstance instance, bool with_session) {
  bool ok = true;
  if (!with_session) {
    ok &= LoadFn(instance, "xrEnumerateInstanceExtensionProperties", g.EnumerateInstanceExtensionProperties);
    ok &= LoadFn(instance, "xrCreateInstance", g.CreateInstance);
    ok &= LoadFn(instance, "xrDestroyInstance", g.DestroyInstance);
  } else {
    ok &= LoadFn(instance, "xrGetSystem", g.GetSystem);
    ok &= LoadFn(instance, "xrGetSystemProperties", g.GetSystemProperties);
    ok &= LoadFn(instance, "xrEnumerateEnvironmentBlendModes", g.EnumerateEnvironmentBlendModes);
    ok &= LoadFn(instance, "xrEnumerateViewConfigurationViews", g.EnumerateViewConfigurationViews);
    ok &= LoadFn(instance, "xrCreateSession", g.CreateSession);
    ok &= LoadFn(instance, "xrDestroySession", g.DestroySession);
    ok &= LoadFn(instance, "xrBeginSession", g.BeginSession);
    ok &= LoadFn(instance, "xrEndSession", g.EndSession);
    ok &= LoadFn(instance, "xrPollEvent", g.PollEvent);
    ok &= LoadFn(instance, "xrWaitFrame", g.WaitFrame);
    ok &= LoadFn(instance, "xrBeginFrame", g.BeginFrame);
    ok &= LoadFn(instance, "xrEndFrame", g.EndFrame);
    ok &= LoadFn(instance, "xrLocateViews", g.LocateViews);
    ok &= LoadFn(instance, "xrCreateReferenceSpace", g.CreateReferenceSpace);
    ok &= LoadFn(instance, "xrDestroySpace", g.DestroySpace);
    ok &= LoadFn(instance, "xrEnumerateSwapchainFormats", g.EnumerateSwapchainFormats);
    ok &= LoadFn(instance, "xrCreateSwapchain", g.CreateSwapchain);
    ok &= LoadFn(instance, "xrDestroySwapchain", g.DestroySwapchain);
    ok &= LoadFn(instance, "xrEnumerateSwapchainImages", g.EnumerateSwapchainImages);
    ok &= LoadFn(instance, "xrAcquireSwapchainImage", g.AcquireSwapchainImage);
    ok &= LoadFn(instance, "xrWaitSwapchainImage", g.WaitSwapchainImage);
    ok &= LoadFn(instance, "xrReleaseSwapchainImage", g.ReleaseSwapchainImage);
    ok &= LoadFn(instance, "xrGetD3D12GraphicsRequirementsKHR", g.GetD3D12GraphicsRequirementsKHR);
  }
  return ok;
}

bool LoadLoader() {
  wchar_t path[MAX_PATH]{};
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) {
      wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"openxr_loader.dll");
      g_dll = LoadLibraryW(path);
    }
  }
  if (!g_dll) g_dll = LoadLibraryW(L"openxr_loader.dll");
  if (!g_dll) {
    REXLOG_ERROR("VR: openxr_loader.dll was not next to vig8.exe or on PATH");
    return false;
  }
  auto* sym = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_dll, "xrGetInstanceProcAddr"));
  if (!sym) {
    REXLOG_ERROR("VR: openxr_loader.dll has no xrGetInstanceProcAddr");
    return false;
  }
  g.GetInstanceProcAddr = sym;
  return LoadDispatch(XR_NULL_HANDLE, false);
}

bool HasD3D12() {
  uint32_t n = 0;
  if (!Ok(g.EnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr), "count extensions")) return false;
  std::vector<XrExtensionProperties> ext(n);
  for (auto& e : ext) e = XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES};
  if (!Ok(g.EnumerateInstanceExtensionProperties(nullptr, n, &n, ext.data()), "list extensions")) return false;
  for (uint32_t i = 0; i < n; ++i) {
    if (std::strcmp(ext[i].extensionName, "XR_KHR_D3D12_enable") == 0) return true;
  }
  REXLOG_ERROR("VR: the active OpenXR runtime has no XR_KHR_D3D12_enable. Set Virtual Desktop's VDXR, or SteamVR, as the active runtime.");
  return false;
}

void Publish(const RawStereo& raw) {
  std::lock_guard<std::mutex> lock(g_pose_mu);
  g_latest = raw;
}

void Locate(XrTime time) {
  XrViewState vs{XR_TYPE_VIEW_STATE};
  XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
  info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  info.displayTime = time;
  info.space = g_space;
  XrView views[2]{};
  views[0].type = views[1].type = XR_TYPE_VIEW;
  uint32_t count = 2;
  if (!Ok(g.LocateViews(g_session, &info, &vs, 2, &count, views), "xrLocateViews") || count < 2) return;
  const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
  RawStereo raw{};
  raw.valid = (vs.viewStateFlags & need) == need;
  for (int i = 0; i < 2; ++i) {
    raw.eye[i].q = Quat{views[i].pose.orientation.x, views[i].pose.orientation.y,
                        views[i].pose.orientation.z, views[i].pose.orientation.w};
    raw.eye[i].p = Vec3{views[i].pose.position.x, views[i].pose.position.y, views[i].pose.position.z};
    raw.eye[i].fov = Fov{views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp,
                         views[i].fov.angleDown};
  }
  Publish(raw);
}

bool FillImages(EyeSwap& sc) {
  uint32_t n = 0;
  if (!Ok(g.EnumerateSwapchainImages(sc.handle, 0, &n, nullptr), "count swapchain images")) return false;
  sc.images.resize(n);
  for (auto& im : sc.images) im = XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
  return Ok(g.EnumerateSwapchainImages(sc.handle, n, &n,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())),
            "enumerate swapchain images");
}

bool MakeSwapchain(EyeSwap& sc, uint32_t w, uint32_t h) {
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  ci.format = g_format;
  ci.sampleCount = 1;
  ci.width = w;
  ci.height = h;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  if (!Ok(g.CreateSwapchain(g_session, &ci, &sc.handle), "xrCreateSwapchain")) return false;
  sc.width = w;
  sc.height = h;
  return FillImages(sc);
}

bool CreateEyeSwapchains() {
  uint32_t n = 0;
  if (!Ok(g.EnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                           &n, nullptr),
          "count views")) {
    return false;
  }
  std::vector<XrViewConfigurationView> views(n);
  for (auto& v : views) v = XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW};
  if (!Ok(g.EnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
                                           views.data()),
          "list views") ||
      n < 2) {
    REXLOG_ERROR("VR: the runtime did not offer two stereo views");
    return false;
  }
  uint32_t fn = 0;
  if (!Ok(g.EnumerateSwapchainFormats(g_session, 0, &fn, nullptr), "count formats")) return false;
  std::vector<int64_t> formats(fn);
  if (!Ok(g.EnumerateSwapchainFormats(g_session, fn, &fn, formats.data()), "list formats")) return false;
  g_format = formats.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM : formats[0];
  for (int64_t want : kFormatPref) {
    for (int64_t have : formats) {
      if (have == want) {
        g_format = want;
        want = -1;
        break;
      }
    }
    if (want < 0) break;
  }
  g_rtv_format = static_cast<DXGI_FORMAT>(g_format);
  REXLOG_INFO("VR: swapchain {}x{} format {}", views[0].recommendedImageRectWidth,
              views[0].recommendedImageRectHeight, static_cast<int>(g_format));
  for (int i = 0; i < 2; ++i) {
    if (!MakeSwapchain(g_eye_sc[i], views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight))
      return false;
  }
  return true;
}

bool EnsureQuad(uint32_t w, uint32_t h) {
  if (g_quad_sc.handle && g_quad_sc.width == w && g_quad_sc.height == h) return true;
  if (g_quad_sc.handle) {
    g.DestroySwapchain(g_quad_sc.handle);
    g_quad_sc = EyeSwap{};
  }
  if (w < 2 || h < 2) return false;
  return MakeSwapchain(g_quad_sc, w, h);
}

bool CreateSpace() {
  XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  info.poseInReferenceSpace.orientation.w = 1.0f;
  const XrReferenceSpaceType order[] = {XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE,
                                        XR_REFERENCE_SPACE_TYPE_VIEW};
  const char* names[] = {"local", "stage", "view"};
  for (int i = 0; i < 3; ++i) {
    info.referenceSpaceType = order[i];
    if (XR_SUCCEEDED(g.CreateReferenceSpace(g_session, &info, &g_space))) {
      REXLOG_INFO("VR: reference space {}", names[i]);
      return true;
    }
  }
  REXLOG_ERROR("VR: no local, stage, or view reference space");
  return false;
}

void PickBlend() {
  uint32_t n = 0;
  if (XR_FAILED(g.EnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                                &n, nullptr)) ||
      n == 0) {
    return;
  }
  std::vector<XrEnvironmentBlendMode> modes(n);
  if (XR_FAILED(g.EnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n,
                                                &n, modes.data()))) {
    return;
  }
  g_blend = modes[0];
  for (auto m : modes) {
    if (m == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) g_blend = m;
  }
}

const char* kBlitHlsl = R"(
cbuffer Rect : register(b0) { float4 uv_rect; };
Texture2D g_src : register(t0);
SamplerState g_samp : register(s0);
struct Vs { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Vs MainVs(uint id : SV_VertexID) {
  float2 p = float2(float((id << 1) & 2), float(id & 2));
  Vs o;
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = float2(uv_rect.x + p.x * uv_rect.z, uv_rect.y + p.y * uv_rect.w);
  return o;
}
float4 MainPs(Vs i) : SV_Target { return g_src.Sample(g_samp, i.uv); }
)";

bool EnsurePso() {
  std::lock_guard<std::recursive_mutex> lock(g_d3d_mu);
  if (g_pso_ready) return true;
  if (!g_device) return false;
  ComPtr<ID3DBlob> vs, ps, err;
  if (FAILED(D3DCompile(kBlitHlsl, std::strlen(kBlitHlsl), "vr_blit", nullptr, nullptr, "MainVs", "vs_5_0", 0, 0,
                        &vs, &err))) {
    REXLOG_ERROR("VR: blit vertex shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }
  if (FAILED(D3DCompile(kBlitHlsl, std::strlen(kBlitHlsl), "vr_blit", nullptr, nullptr, "MainPs", "ps_5_0", 0, 0, &ps,
                        &err))) {
    REXLOG_ERROR("VR: blit pixel shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    return false;
  }
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC samp{};
  samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  samp.MaxLOD = D3D12_FLOAT32_MAX;
  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = 2;
  desc.pParameters = params;
  desc.NumStaticSamplers = 1;
  desc.pStaticSamplers = &samp;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  ComPtr<ID3DBlob> sig;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
      FAILED(g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                           IID_PPV_ARGS(&g_root)))) {
    REXLOG_ERROR("VR: blit root signature failed");
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = g_root.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = g_rtv_format;
  pso.SampleDesc.Count = 1;
  if (FAILED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pso)))) {
    REXLOG_ERROR("VR: blit pipeline failed");
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 4;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) return false;
  for (int i = 0; i < 2; ++i) {
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_blit_alloc[i]))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_blit_alloc[i].Get(), nullptr,
                                           IID_PPV_ARGS(&g_blit_list[i])))) {
      return false;
    }
    g_blit_list[i]->Close();
  }
  if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_blit_fence)))) return false;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 32;
  if (FAILED(g_device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_rtv_heap)))) return false;
  g_pso_ready = true;
  return true;
}

bool EnsureCopy(uint32_t w, uint32_t h) {
  std::lock_guard<std::recursive_mutex> lock(g_d3d_mu);
  if (!g_device || !EnsurePso()) return false;
  if (!g_copy_ready) {
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copy_fence)))) return false;
    for (int i = 0; i < 3; ++i) {
      if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_copy_alloc[i]))) ||
          FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_copy_alloc[i].Get(), nullptr,
                                             IID_PPV_ARGS(&g_copy_list[i])))) {
        return false;
      }
      g_copy_list[i]->Close();
    }
    g_copy_ready = true;
  }
  if (g_staging[0] && g_staging_w == w && g_staging_h == h) return true;
  for (int i = 0; i < 3; ++i) {
    if (g_copy_value[i] && g_copy_fence && g_copy_fence->GetCompletedValue() < g_copy_value[i]) return false;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = w;
  td.Height = h;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  td.SampleDesc.Count = 1;
  td.Flags = D3D12_RESOURCE_FLAG_NONE;
  for (int i = 0; i < 3; ++i) {
    g_staging[i].Reset();
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&g_staging[i])))) {
      return false;
    }
    g_staging_is_srv[i] = false;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    auto inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap ? g_heap->GetCPUDescriptorHandleForHeapStart()
                                             : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (!g_heap) return false;
    cpu.ptr += static_cast<SIZE_T>(i) * inc;
    g_device->CreateShaderResourceView(g_staging[i].Get(), &sv, cpu);
  }
  g_staging_w = w;
  g_staging_h = h;
  return true;
}

void QuadUv(float& x, float& y, float& w, float& h) {
  x = 0;
  y = 0;
  w = 1;
  h = 1;
  const std::string& text = REXCVAR_GET(vr_quad_rect);
  if (text.empty()) return;
  float v[4]{};
  const char* p = text.c_str();
  for (float& item : v) {
    char* end = nullptr;
    item = std::strtof(p, &end);
    if (end == p) return;
    p = end;
  }
  x = v[0];
  y = v[1];
  w = v[2];
  h = v[3];
}

bool Acquire(EyeSwap& sc) {
  if (!sc.handle) return false;
  XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (!Ok(g.AcquireSwapchainImage(sc.handle, &acq, &sc.acquired), "xrAcquireSwapchainImage")) return false;
  auto release = [&] {
    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    g.ReleaseSwapchainImage(sc.handle, &rel);
    sc.have_image = false;
  };
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = 500000000;
  XrResult wr = g.WaitSwapchainImage(sc.handle, &wait);
  if (!XR_SUCCEEDED(wr)) {
    release();
    if (wr != XR_TIMEOUT_EXPIRED) Ok(wr, "xrWaitSwapchainImage");
    return false;
  }
  sc.have_image = sc.acquired < sc.images.size() && sc.images[sc.acquired].texture != nullptr;
  if (!sc.have_image) release();
  return sc.have_image;
}

void Release(EyeSwap& sc) {
  if (!sc.handle || !sc.have_image) return;
  XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  g.ReleaseSwapchainImage(sc.handle, &rel);
  sc.have_image = false;
}

D3D12_CPU_DESCRIPTOR_HANDLE Rtv(EyeSwap& sc) {
  if (sc.rtvs.size() < sc.images.size()) sc.rtvs.resize(sc.images.size());
  D3D12_CPU_DESCRIPTOR_HANDLE& have = sc.rtvs[sc.acquired];
  if (!have.ptr && g_rtv_heap && g_rtv_used < 32 && sc.images[sc.acquired].texture) {
    have = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    auto inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    have.ptr += static_cast<SIZE_T>(g_rtv_used++) * inc;
    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = g_rtv_format;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_device->CreateRenderTargetView(sc.images[sc.acquired].texture, &rd, have);
  }
  return have;
}

struct BlitPass {
  ID3D12GraphicsCommandList* list = nullptr;
  int which = 0;
  uint64_t wait_copy = 0;
};

// One command list for every swapchain this frame, so the two eyes don't wait
// on each other. The copy fence is waited once, at the newest value.
bool BeginBlit(BlitPass& pass) {
  if (!EnsurePso() || !g_blit_fence) return false;
  if (g_blit_value && g_blit_fence->GetCompletedValue() < g_blit_value) return false;
  pass.which = g_blit_which;
  g_blit_which ^= 1;
  g_blit_alloc[pass.which]->Reset();
  pass.list = g_blit_list[pass.which].Get();
  pass.list->Reset(g_blit_alloc[pass.which].Get(), g_pso.Get());
  return true;
}

void DrawBlit(BlitPass& pass, EyeSwap& sc, int slot, float u, float v, float w, float h) {
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(sc);
  if (!pass.list || !rtv.ptr) return;
  if (g_copy_value[slot] > pass.wait_copy) pass.wait_copy = g_copy_value[slot];
  D3D12_VIEWPORT vp{};
  vp.Width = static_cast<float>(sc.width);
  vp.Height = static_cast<float>(sc.height);
  vp.MaxDepth = 1.0f;
  D3D12_RECT scissor{0, 0, static_cast<LONG>(sc.width), static_cast<LONG>(sc.height)};
  pass.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  pass.list->RSSetViewports(1, &vp);
  pass.list->RSSetScissorRects(1, &scissor);
  pass.list->SetGraphicsRootSignature(g_root.Get());
  float rect[4] = {u, v, w, h};
  pass.list->SetGraphicsRoot32BitConstants(0, 4, rect, 0);
  ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
  pass.list->SetDescriptorHeaps(1, heaps);
  auto inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(slot) * inc;
  pass.list->SetGraphicsRootDescriptorTable(1, gpu);
  pass.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  pass.list->DrawInstanced(3, 1, 0, 0);
}

void EndBlit(BlitPass& pass) {
  if (!pass.list) return;
  pass.list->Close();
  if (g_copy_fence && pass.wait_copy) g_queue->Wait(g_copy_fence.Get(), pass.wait_copy);
  ID3D12CommandList* lists[] = {pass.list};
  g_queue->ExecuteCommandLists(1, lists);
  g_queue->Signal(g_blit_fence.Get(), ++g_blit_value);
  pass.list = nullptr;
}

void ReclaimSlots() {
  if (!g_blit_fence) return;
  uint64_t done = g_blit_fence->GetCompletedValue();
  for (int i = 0; i < 3; ++i) {
    if (g_slot_state[i].load() && g_slot_fence[i] != 0 && done >= g_slot_fence[i]) g_slot_state[i].store(0);
  }
}

XrPosef PoseOf(const RawEye& eye, bool valid) {
  XrPosef pose{};
  pose.orientation.w = 1.0f;
  if (!valid) return pose;
  pose.orientation = {eye.q.x, eye.q.y, eye.q.z, eye.q.w};
  pose.position = {eye.p.x, eye.p.y, eye.p.z};
  return pose;
}

XrFovf FovOf(const RawEye& eye, bool valid) {
  Fov f = valid ? eye.fov : FallbackFov();
  return XrFovf{f.angle_left, f.angle_right, f.angle_up, f.angle_down};
}

void EndLayers(XrTime time, const XrCompositionLayerBaseHeader* const* layers, uint32_t count) {
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
  end.displayTime = time;
  end.environmentBlendMode = g_blend;
  end.layerCount = count;
  end.layers = count ? layers : nullptr;
  g.EndFrame(g_session, &end);
}

void Submit(const GpuFrame& frame, XrTime time) {
  const bool projection = frame.layout == static_cast<uint32_t>(rex::graphics::vr::FrameLayout::kSideBySide) ||
                          frame.layout == static_cast<uint32_t>(rex::graphics::vr::FrameLayout::kAlternate);
  const bool theater = frame.layout == static_cast<uint32_t>(rex::graphics::vr::FrameLayout::kTheater) ||
                       frame.layout == static_cast<uint32_t>(rex::graphics::vr::FrameLayout::kMono);
  const bool mirror = REXCVAR_GET(vr_show_mirror_quad);
  BlitPass pass{};
  if (!BeginBlit(pass)) {
    static bool logged = false;
    if (!logged) {
      REXLOG_WARN("VR: dropping a headset frame because the previous blit has not finished");
      logged = true;
    }
    EndLayers(time, nullptr, 0);
    g_slot_state[frame.slot].store(0);
    return;
  }
  bool drew = false;
  if (projection) {
    for (int i = 0; i < 2; ++i) {
      if (!Acquire(g_eye_sc[i])) continue;
      float u = 0, w = 1;
      if (frame.layout == static_cast<uint32_t>(rex::graphics::vr::FrameLayout::kSideBySide)) {
        u = i == 0 ? 0.0f : 0.5f;
        w = 0.5f;
      }
      DrawBlit(pass, g_eye_sc[i], frame.slot, u, 0, w, 1);
      drew = true;
    }
  }
  bool quad = false;
  if ((theater || mirror) && EnsureQuad(frame.width, frame.height) && Acquire(g_quad_sc)) {
    float x, y, w, h;
    QuadUv(x, y, w, h);
    DrawBlit(pass, g_quad_sc, frame.slot, x, y, w, h);
    quad = true;
    drew = true;
  }
  if (drew) EndBlit(pass);
  else if (pass.list) {
    pass.list->Close();
    pass.list = nullptr;
  }
  g_slot_fence[frame.slot] = g_blit_value;
  if (!drew) g_slot_state[frame.slot].store(0);

  XrCompositionLayerProjectionView pv[2]{};
  XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad quad_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
  const XrCompositionLayerBaseHeader* layers[2]{};
  uint32_t n = 0;
  if (projection && g_eye_sc[0].have_image && g_eye_sc[1].have_image) {
    for (int i = 0; i < 2; ++i) {
      pv[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
      pv[i].pose = PoseOf(frame.raw.eye[i], frame.pose_valid);
      pv[i].fov = FovOf(frame.raw.eye[i], frame.pose_valid);
      pv[i].subImage.swapchain = g_eye_sc[i].handle;
      pv[i].subImage.imageRect = {0, 0, static_cast<int32_t>(g_eye_sc[i].width),
                                  static_cast<int32_t>(g_eye_sc[i].height)};
    }
    proj.space = g_space;
    proj.viewCount = 2;
    proj.views = pv;
    layers[n++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
  }
  if (quad && g_quad_sc.have_image) {
    float distance = static_cast<float>(REXCVAR_GET(vr_screen_distance));
    float width = static_cast<float>(REXCVAR_GET(vr_screen_width));
    if (distance < 0.2f) distance = 0.2f;
    if (width < 0.2f) width = 0.2f;
    float aspect = frame.height ? static_cast<float>(frame.width) / static_cast<float>(frame.height) : 1.0f;
    quad_layer.space = g_space;
    quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad_layer.subImage.swapchain = g_quad_sc.handle;
    quad_layer.subImage.imageRect = {0, 0, static_cast<int32_t>(g_quad_sc.width),
                                     static_cast<int32_t>(g_quad_sc.height)};
    quad_layer.pose.orientation.w = 1.0f;
    quad_layer.pose.position.z = -distance;
    quad_layer.size = {width, width / aspect};
    quad_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layers[n++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad_layer);
  }
  for (auto& sc : g_eye_sc) Release(sc);
  Release(g_quad_sc);
  EndLayers(time, layers, n);
}

void OnState(XrSessionState state) {
  REXLOG_INFO("VR: session state {}", static_cast<int>(state));
  if (state == XR_SESSION_STATE_READY && !g_session_begun) {
    if (CreateSpace() && CreateEyeSwapchains()) {
      PickBlend();
      XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
      info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      if (Ok(g.BeginSession(g_session, &info), "xrBeginSession")) {
        g_session_begun = true;
        g_running.store(true);
        REXLOG_INFO("VR: session running");
      }
    }
  } else if (state == XR_SESSION_STATE_STOPPING) {
    g_running.store(false);
    if (g_session_begun) {
      g.EndSession(g_session);
      g_session_begun = false;
    }
  } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
    g_running.store(false);
    g_stop.store(true);
  }
}

void Poll() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (g.PollEvent(g_instance, &ev) == XR_SUCCESS) {
    auto* header = reinterpret_cast<XrEventDataBaseHeader*>(&ev);
    if (header->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
      OnState(changed->state);
    }
    ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
  }
}

void ThreadMain() {
  while (!g_stop.load()) {
    if (!g_session) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    Poll();
    if (!g_running.load() || !g_session_begun || g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (!Ok(g.WaitFrame(g_session, &wait, &state), "xrWaitFrame")) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    Locate(state.predictedDisplayTime);
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Ok(g.BeginFrame(g_session, &begin), "xrBeginFrame")) continue;
    ReclaimSlots();
    GpuFrame frame{};
    bool got = false;
    {
      std::unique_lock<std::mutex> lock(g_frame_mu);
      if (REXCVAR_GET(vr_pace) == "headset") {
        g_frame_cv.wait_for(lock, std::chrono::milliseconds(8),
                            [] { return g_stop.load() || !g_frames.empty(); });
      }
      if (!g_frames.empty()) {
        frame = g_frames.back();
        for (size_t i = 0; i + 1 < g_frames.size(); ++i) g_slot_state[g_frames[i].slot].store(0);
        g_frames.clear();
        got = true;
      }
    }
    if (state.shouldRender == XR_TRUE && got) Submit(frame, state.predictedDisplayTime);
    else {
      if (got) g_slot_state[frame.slot].store(0);
      EndLayers(state.predictedDisplayTime, nullptr, 0);
    }
  }
}

void CopyGuest(const SubmittedFrame& frame) {
  auto* resource = static_cast<ID3D12Resource*>(frame.resource);
  if (!resource || !frame.width || !frame.height) return;
  ReclaimSlots();
  if (!EnsureCopy(frame.width, frame.height)) return;
  int slot = -1;
  for (int k = 1; k <= 3; ++k) {
    int s = (g_last_slot + k) % 3;
    int free = 0;
    if (g_slot_state[s].compare_exchange_strong(free, 1)) {
      slot = s;
      break;
    }
  }
  if (slot < 0) {
    static bool logged = false;
    if (!logged) {
      REXLOG_WARN("VR: dropping a presented frame; all three copy slots are still in use");
      logged = true;
    }
    return;
  }
  g_last_slot = slot;
  if (g_copy_value[slot] && g_copy_fence->GetCompletedValue() < g_copy_value[slot]) {
    g_slot_state[slot].store(0);
    return;
  }
  g_copy_alloc[slot]->Reset();
  ID3D12GraphicsCommandList* list = g_copy_list[slot].Get();
  list->Reset(g_copy_alloc[slot].Get(), nullptr);
  D3D12_RESOURCE_BARRIER bars[2]{};
  bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  bars[0].Transition.pResource = resource;
  bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  bars[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  bars[1].Transition.pResource = g_staging[slot].Get();
  bars[1].Transition.StateBefore =
      g_staging_is_srv[slot] ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
  bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  bars[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  if (g_staging_is_srv[slot]) list->ResourceBarrier(2, bars);
  else list->ResourceBarrier(1, &bars[0]);
  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = resource;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst = src;
  dst.pResource = g_staging[slot].Get();
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  list->ResourceBarrier(2, bars);
  list->Close();
  ID3D12CommandList* lists[] = {list};
  g_queue->ExecuteCommandLists(1, lists);
  uint64_t value = ++g_copy_serial;
  g_copy_value[slot] = value;
  g_queue->Signal(g_copy_fence.Get(), value);
  g_staging_is_srv[slot] = true;

  GpuFrame gpu{};
  gpu.raw = frame.raw;
  gpu.layout = frame.layout;
  gpu.eye = frame.eye;
  gpu.pose_valid = frame.pose_valid;
  gpu.slot = slot;
  gpu.copy_value = value;
  gpu.width = frame.width;
  gpu.height = frame.height;
  {
    std::lock_guard<std::mutex> lock(g_frame_mu);
    if (g_frames.size() >= 2) {
      g_slot_state[g_frames.front().slot].store(0);
      g_frames.erase(g_frames.begin());
    }
    g_frames.push_back(gpu);
  }
  g_frame_cv.notify_one();
}

void WaitGpu(ID3D12Fence* fence, uint64_t value) {
  if (!fence || !value || fence->GetCompletedValue() >= value) return;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) return;
  if (SUCCEEDED(fence->SetEventOnCompletion(value, event))) WaitForSingleObject(event, 2000);
  CloseHandle(event);
}

void DestroySwap(EyeSwap& sc) {
  if (sc.handle && g.DestroySwapchain) g.DestroySwapchain(sc.handle);
  sc = EyeSwap{};
}

}  // namespace

StartResult OpenXrStart(uint32_t* luid_low, int32_t* luid_high, uint32_t* min_feature_level) {
  if (!luid_low || !luid_high || !min_feature_level) return StartResult::kRuntimeFailed;
  if (!LoadLoader() || !HasD3D12()) return StartResult::kRuntimeFailed;
  XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(info.applicationInfo.applicationName, "Vigilante 8 Arcade", XR_MAX_APPLICATION_NAME_SIZE - 1);
  info.applicationInfo.applicationVersion = 1;
  std::strncpy(info.applicationInfo.engineName, "vig8", XR_MAX_ENGINE_NAME_SIZE - 1);
  info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  const char* ext = "XR_KHR_D3D12_enable";
  info.enabledExtensionCount = 1;
  info.enabledExtensionNames = &ext;
  if (!Ok(g.CreateInstance(&info, &g_instance), "xrCreateInstance")) return StartResult::kRuntimeFailed;
  if (!LoadDispatch(g_instance, true)) return StartResult::kRuntimeFailed;
  XrSystemGetInfo sys{XR_TYPE_SYSTEM_GET_INFO};
  sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if (!Ok(g.GetSystem(g_instance, &sys, &g_system), "xrGetSystem")) {
    REXLOG_ERROR("VR: no headset. Start Virtual Desktop (or SteamVR) and connect the Quest, then launch again.");
    return StartResult::kRuntimeFailed;
  }
  XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(g.GetSystemProperties(g_instance, g_system, &props))) {
    REXLOG_INFO("VR: runtime system {}", props.systemName);
  }
  XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
  if (!Ok(g.GetD3D12GraphicsRequirementsKHR(g_instance, g_system, &req), "D3D12 requirements"))
    return StartResult::kRuntimeFailed;
  *luid_low = req.adapterLuid.LowPart;
  *luid_high = req.adapterLuid.HighPart;
  *min_feature_level = static_cast<uint32_t>(req.minFeatureLevel);
  return StartResult::kOk;
}

void OpenXrOnDevice(void* device, void* queue) {
  g_device = static_cast<ID3D12Device*>(device);
  g_queue = static_cast<ID3D12CommandQueue*>(queue);
  XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
  binding.device = g_device.Get();
  binding.queue = g_queue.Get();
  XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
  info.next = &binding;
  info.systemId = g_system;
  if (!Ok(g.CreateSession(g_instance, &info, &g_session), "xrCreateSession")) return;
  g_accept.store(true);
  g_stop.store(false);
  g_thread = std::thread(ThreadMain);
  REXLOG_INFO("VR: OpenXR session created");
}

void OpenXrShutdown() {
  g_accept.store(false);
  while (g_in_present.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  g_stop.store(true);
  g_frame_cv.notify_all();
  if (g_thread.joinable()) g_thread.join();
  WaitGpu(g_copy_fence.Get(), g_copy_serial);
  WaitGpu(g_blit_fence.Get(), g_blit_value);
  g_running.store(false);
  if (g_session_begun && g.EndSession) {
    g.EndSession(g_session);
    g_session_begun = false;
  }
  DestroySwap(g_eye_sc[0]);
  DestroySwap(g_eye_sc[1]);
  DestroySwap(g_quad_sc);
  if (g_space && g.DestroySpace) g.DestroySpace(g_space);
  g_space = XR_NULL_HANDLE;
  if (g_session && g.DestroySession) g.DestroySession(g_session);
  g_session = XR_NULL_HANDLE;
  if (g_instance && g.DestroyInstance) g.DestroyInstance(g_instance);
  g_instance = XR_NULL_HANDLE;
  g_pso.Reset();
  g_root.Reset();
  g_heap.Reset();
  g_rtv_heap.Reset();
  g_blit_fence.Reset();
  g_copy_fence.Reset();
  for (int i = 0; i < 3; ++i) {
    g_staging[i].Reset();
    g_copy_alloc[i].Reset();
    g_copy_list[i].Reset();
  }
  for (int i = 0; i < 2; ++i) {
    g_blit_alloc[i].Reset();
    g_blit_list[i].Reset();
  }
  g_pso_ready = false;
  g_copy_ready = false;
  g_queue.Reset();
  g_device.Reset();
  if (g_dll) {
    FreeLibrary(g_dll);
    g_dll = nullptr;
  }
}

bool OpenXrRunning() { return g_running.load(); }

bool OpenXrReadPose(RawStereo& out) {
  std::lock_guard<std::mutex> lock(g_pose_mu);
  if (!g_latest.valid) return false;
  out = g_latest;
  return true;
}

void OpenXrPresent(const SubmittedFrame& frame) {
  if (!g_accept.load()) return;
  g_in_present.fetch_add(1);
  if (g_accept.load()) CopyGuest(frame);
  g_in_present.fetch_sub(1);
}

}  // namespace vig8::vr

#endif
