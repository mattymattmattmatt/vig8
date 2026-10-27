// Vigilante 8 Arcade VR camera profile.
//
// The overrides follow stubs.cpp: REX_EXTERN replaces the generated weak
// function, and __imp__sub_... is the original. With --vr off, or before the
// OpenXR session is running, each one calls the original and changes nothing.
//
// The default path is the one in the camera notes. sub_820BFA00 owns the frame.
// It draws player 1's camera twice, once per eye, with the head pose applied to
// the view after sub_820B8478 and an OpenXR frustum in place of sub_8212B760.
// cam+0x40 is never written. Until a resolve target has been observed, auto
// mode uses the game's own 2-player side-by-side layout instead, with both
// viewports pointed at player 1's camera.

#include "vig8_pch.h"
#include "vig8_init.h"

#include "vr/vr_profile.h"
#include "vr/vr_runtime.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <bit>
#include <cstring>

namespace {

using vig8::vr::BeginFrame;
using vig8::vr::ColorTarget;
using vig8::vr::EndHooks;
using vig8::vr::ForcedCameraMode;
using vig8::vr::FullAspect;
using vig8::vr::HudMode;
using vig8::vr::HudScalar;
using vig8::vr::MenuIsPointer;
using vig8::vr::MonoFinalProjection;
using vig8::vr::NearPlane;
using vig8::vr::NoteResolve;
using vig8::vr::OffsetViews;
using vig8::vr::Plan;
using vig8::vr::PlayerHudSlot;
using vig8::vr::PreAspectProjection;
using vig8::vr::PublishFrame;
using vig8::vr::RenderPath;
using vig8::vr::ReplaceProjection;
using vig8::vr::SessionActive;
using vig8::vr::SetExplicitEye;
using vig8::vr::SetInactive;
using vig8::vr::SetPlayerHudSlot;
using vig8::vr::SetSkipResolve;
using vig8::vr::SetSuppressHud;
using vig8::vr::SkipResolve;
using vig8::vr::SuppressHud;
using vig8::vr::AdvanceEye;
using vig8::vr::ViewOffset;
using vig8::vr::ViewportFloats;
using vig8::vr::VrEnabled;

namespace profile = vig8::vr::profile;

// Same values as rex::graphics::vr::FrameLayout.
constexpr uint32_t kLayoutSbs = 1;
constexpr uint32_t kLayoutAlternate = 2;
constexpr uint32_t kLayoutTheater = 3;

bool g_size_logged = false;
bool g_forced_logged = false;
bool g_quad_logged = false;
bool g_stereo_fallback_logged = false;

bool DecodeDim(uint32_t bits, float& out) {
  float as_float = std::bit_cast<float>(bits);
  if (as_float >= 64.0f && as_float <= 16384.0f) {
    out = as_float;
    return true;
  }
  if (bits >= 64 && bits <= 16384) {
    out = static_cast<float>(bits);
    return true;
  }
  return false;
}

bool ReadDeviceSize(uint8_t* base, float& w, float& h, uint32_t& wrapper) {
  auto try_at = [&](uint32_t addr) {
    float ww = 0, hh = 0;
    if (!DecodeDim(REX_LOAD_U32(addr + profile::kDeviceW), ww)) return false;
    if (!DecodeDim(REX_LOAD_U32(addr + profile::kDeviceH), hh)) return false;
    w = ww;
    h = hh;
    wrapper = addr;
    return true;
  };
  if (try_at(profile::kDevice)) return true;
  uint32_t pointed = REX_LOAD_U32(profile::kDevice);
  if (pointed > 0x10000 && try_at(pointed)) {
    if (!g_size_logged) {
      REXLOG_INFO("VR: device size is at the object {:08X} points to", profile::kDevice);
      g_size_logged = true;
    }
    return true;
  }
  return false;
}

void StoreMat(uint8_t* base, uint32_t addr, const float m[16]) {
  for (int i = 0; i < 16; ++i) REX_STORE_U32(addr + static_cast<uint32_t>(i) * 4, std::bit_cast<uint32_t>(m[i]));
}

void LoadMat(uint8_t* base, uint32_t addr, float m[16]) {
  for (int i = 0; i < 16; ++i) m[i] = std::bit_cast<float>(REX_LOAD_U32(addr + static_cast<uint32_t>(i) * 4));
}

void CopyGuest(uint8_t* base, uint32_t from, uint32_t to) {
  for (int i = 0; i < 16; ++i) REX_STORE_U32(to + static_cast<uint32_t>(i) * 4, REX_LOAD_U32(from + static_cast<uint32_t>(i) * 4));
}

uint32_t Scratch() {
  static uint32_t addr = 0;
  if (!addr && REX_KERNEL_MEMORY()) addr = REX_KERNEL_MEMORY()->SystemHeapAlloc(256, 64);
  return addr;
}

void SetDim(PPCRegister& reg, float value) {
  if (ViewportFloats()) reg.u32 = std::bit_cast<uint32_t>(value);
  else reg.u32 = static_cast<uint32_t>(value);
}

uint32_t PlayerCount(uint32_t raw) {
  if (raw > 4 && (raw & 0xFF) <= 4) return raw & 0xFF;
  return raw;
}

void ApplyViewOffset(uint8_t* base, uint32_t addr) {
  float view[16], offset[16];
  LoadMat(base, addr, view);
  ViewOffset(offset);
  vig8::vr::Mat4 v{}, e{};
  std::memcpy(v.m, view, sizeof(view));
  std::memcpy(e.m, offset, sizeof(offset));
  vig8::vr::Mat4 out = vig8::vr::Mul(v, e);
  StoreMat(base, addr, out.m);
}

void WriteMonoGlobals(PPCContext& ctx, uint8_t* base, uint32_t cam, float aspect) {
  uint32_t scratch = Scratch();
  if (!scratch || !cam) return;
  ctx.r3.u32 = scratch;
  ctx.r4.u32 = cam;
  __imp__sub_820B8478(ctx, base);
  CopyGuest(base, scratch, profile::kViewGlobal);
  float proj[16];
  MonoFinalProjection(aspect, proj);
  StoreMat(base, profile::kProjGlobal, proj);
}

void DrawHudOnce(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t cam, uint32_t menu_word, float dev_w,
                 float dev_h) {
  EndHooks();
  // quad draws the HUD once for a layer we cannot see yet: the pass has no
  // render target of its own. baked already drew it into each eye. skip omits it.
  if (HudMode() != 0 && !menu_word) return;
  if (HudMode() == 0 && !g_quad_logged) {
    REXLOG_INFO(
        "VR: the HUD is drawn once, but it has no texture of its own, so the headset quad is not shown. "
        "--vr_hud=baked draws it into each eye.");
    g_quad_logged = true;
  }
  SetSkipResolve(true);
  WriteMonoGlobals(ctx, base, cam, FullAspect(dev_w, dev_h));
  double scalar = HudScalar(dev_w, dev_h);
  if (HudMode() == 0) {
    ctx.r3.u32 = self + profile::kOffSlot0;
    ctx.f1.f64 = scalar;
    __imp__sub_820B5348(ctx, base);
  }
  if (menu_word) {
    ctx.r3.u32 = MenuIsPointer() ? menu_word : self + profile::kOffMenu;
    ctx.f1.f64 = scalar;
    __imp__sub_820AAA60(ctx, base);
  }
  SetSkipResolve(false);
}

void ForceCamera(uint8_t* base, uint32_t cam) {
  int mode = ForcedCameraMode();
  if (mode < 0 || !cam) return;
  REX_STORE_U32(cam + profile::kCamMode, static_cast<uint32_t>(mode));
  if (!g_forced_logged) {
    REXLOG_INFO("VR: forcing cam+0x12C to {} (the field might be a byte; -1 to stop)", mode);
    g_forced_logged = true;
  }
}

bool RunSbs(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t cam, uint32_t menu_word, float dev_w, float dev_h) {
  BeginFrame(RenderPath::kSbs, dev_w, dev_h, dev_w * 0.5f, dev_h);
  ForceCamera(base, cam);
  uint32_t count_addr = self + profile::kOffPlayers;
  uint32_t saved_count = REX_LOAD_U32(count_addr);
  bool as_byte = saved_count > 4 && (saved_count & 0xFF) <= 4;
  uint32_t cam2_addr = self + profile::kOffCam0 + profile::kSlotStride;
  uint32_t saved_cam2 = REX_LOAD_U32(cam2_addr);
  if (as_byte) REX_STORE_U8(count_addr, 2);
  else REX_STORE_U32(count_addr, 2);
  REX_STORE_U32(cam2_addr, cam);
  __imp__sub_820BFA00(ctx, base);
  if (as_byte) REX_STORE_U8(count_addr, static_cast<uint8_t>(saved_count));
  else REX_STORE_U32(count_addr, saved_count);
  REX_STORE_U32(cam2_addr, saved_cam2);
  // The original render already drew the menu.
  DrawHudOnce(ctx, base, self, cam, 0, dev_w, dev_h);
  PublishFrame(kLayoutSbs);
  return true;
}

bool RunStereo(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t cam, uint32_t menu_word, float dev_w,
               float dev_h, uint32_t wrapper) {
  uint32_t dev = 0, rt = 0;
  if (!ColorTarget(dev, rt) || !rt) {
    if (!g_stereo_fallback_logged) {
      REXLOG_INFO("VR: no color target yet. Using the side-by-side layout until a resolve is logged.");
      g_stereo_fallback_logged = true;
    }
    return false;
  }
  if (!dev) dev = wrapper;
  uint32_t scratch = Scratch();
  if (!scratch) {
    REXLOG_ERROR("VR: no guest memory for the per-eye matrices. Using side-by-side.");
    return false;
  }
  BeginFrame(RenderPath::kStereo, dev_w, dev_h, dev_w * 0.5f, dev_h);
  ForceCamera(base, cam);
  SetSuppressHud(HudMode() != 1);
  uint32_t half = static_cast<uint32_t>(dev_w) / 2;
  uint32_t height = static_cast<uint32_t>(dev_h);
  uint32_t slot = self + profile::kOffSlot0;
  for (int eye = 0; eye < 2; ++eye) {
    SetExplicitEye(eye);
    uint32_t proj = scratch;
    uint32_t view = scratch + 64;
    ctx.r3.u32 = proj;
    ctx.f1.f64 = profile::kDefaultFocal;
    ctx.f2.f64 = profile::kDefaultFocal;
    ctx.f3.f64 = NearPlane();
    sub_8212B760(ctx, base);
    ctx.r3.u32 = view;
    ctx.r4.u32 = cam;
    sub_820B8478(ctx, base);
    ctx.r3.u32 = self;
    ctx.r4.u32 = proj;
    ctx.r5.u32 = view;
    ctx.r6.u32 = HudMode() == 1 ? slot : 0;
    SetDim(ctx.r7, static_cast<float>(eye * half));
    SetDim(ctx.r8, 0);
    SetDim(ctx.r9, static_cast<float>(half));
    SetDim(ctx.r10, static_cast<float>(height));
    __imp__sub_820BE790(ctx, base);
    ctx.r3.u32 = dev;
    ctx.r4.u32 = rt;
    ctx.r5.u32 = 0;
    SetDim(ctx.r6, static_cast<float>(eye * half));
    SetDim(ctx.r7, 0);
    __imp__sub_82132078(ctx, base);
  }
  DrawHudOnce(ctx, base, self, cam, menu_word, dev_w, dev_h);
  PublishFrame(kLayoutSbs);
  return true;
}

}  // namespace

REX_EXTERN(sub_820B8478) {
  uint32_t out = ctx.r3.u32;
  __imp__sub_820B8478(ctx, base);
  if (!OffsetViews() || !out) return;
  ApplyViewOffset(base, out);
  AdvanceEye();
}

REX_EXTERN(sub_8212B760) {
  uint32_t out = ctx.r3.u32;
  if (ReplaceProjection() && out) {
    float m[16];
    PreAspectProjection(m);
    StoreMat(base, out, m);
    return;
  }
  __imp__sub_8212B760(ctx, base);
}

REX_EXTERN(sub_820B5348) {
  if (SuppressHud()) return;
  uint32_t slot = PlayerHudSlot();
  if (OffsetViews() && HudMode() == 1 && slot && ctx.r3.u32) ctx.r3.u32 = slot;
  __imp__sub_820B5348(ctx, base);
}

REX_EXTERN(sub_82132078) {
  NoteResolve(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32);
  if (SkipResolve()) return;
  __imp__sub_82132078(ctx, base);
}

REX_EXTERN(sub_820BFA00) {
  if (!VrEnabled() || !SessionActive()) {
    if (VrEnabled()) SetInactive();
    __imp__sub_820BFA00(ctx, base);
    return;
  }
  uint32_t self = ctx.r3.u32;
  if (!self) {
    __imp__sub_820BFA00(ctx, base);
    return;
  }
  uint32_t players = PlayerCount(REX_LOAD_U32(self + profile::kOffPlayers));
  uint32_t menu_word = REX_LOAD_U32(self + profile::kOffMenu);
  uint32_t cam = REX_LOAD_U32(self + profile::kOffCam0);
  SetPlayerHudSlot(self + profile::kOffSlot0);
  float dev_w = 1280.0f, dev_h = 720.0f;
  uint32_t wrapper = profile::kDevice;
  if (!ReadDeviceSize(base, dev_w, dev_h, wrapper) && !g_size_logged) {
    REXLOG_WARN("VR: device size at {:08X}+38 was not a width. Assuming 1280x720.", profile::kDevice);
    g_size_logged = true;
  }
  auto plan = Plan(players, menu_word != 0);
  if (!cam && plan.path != RenderPath::kTheater && !plan.patch_constants) plan.path = RenderPath::kTheater;

  if (plan.path == RenderPath::kTheater) {
    BeginFrame(plan.path, dev_w, dev_h, dev_w, dev_h);
    EndHooks();
    __imp__sub_820BFA00(ctx, base);
    PublishFrame(kLayoutTheater);
    return;
  }
  if (plan.path == RenderPath::kOriginal) {
    BeginFrame(plan.path, dev_w, dev_h, dev_w, dev_h);
    __imp__sub_820BFA00(ctx, base);
    EndHooks();
    PublishFrame(plan.patch_constants ? kLayoutAlternate : kLayoutTheater);
    return;
  }
  if (plan.path == RenderPath::kAlternate) {
    BeginFrame(plan.path, dev_w, dev_h, dev_w, dev_h);
    ForceCamera(base, cam);
    __imp__sub_820BFA00(ctx, base);
    EndHooks();
    PublishFrame(kLayoutAlternate);
    return;
  }
  if (plan.path == RenderPath::kStereo && RunStereo(ctx, base, self, cam, menu_word, dev_w, dev_h, wrapper)) return;
  RunSbs(ctx, base, self, cam, menu_word, dev_w, dev_h);
}
