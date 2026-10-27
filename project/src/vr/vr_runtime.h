// VR session as the game sees it. OpenXR and D3D12 stay in vr_openxr.cpp.
// Hook flags are touched on the guest thread only. The pose cache is shared
// with the GPU thread, which may patch shader constants.

#pragma once

#include <cstdint>

namespace vig8::vr {

enum class RenderPath : uint32_t {
  kOriginal = 0,
  kTheater = 1,
  kAlternate = 2,
  kSbs = 3,
  kStereo = 4,
};

struct RenderPlan {
  RenderPath path = RenderPath::kOriginal;
  bool patch_constants = false;
};

// Called from Vig8App. EarlyInit runs before the D3D12 device exists.
void EarlyInit();
void BindKeys();
void Shutdown();

bool VrEnabled();
bool SessionActive();

RenderPlan Plan(uint32_t player_count, bool menu_open);

// Samples the head pose and arms the per-eye hooks for this frame.
// `view_w` / `view_h` are the viewport the projection is built for.
void BeginFrame(RenderPath path, float device_w, float device_h, float view_w, float view_h);
void SetInactive();
void SetExplicitEye(int eye);
void AdvanceEye();
void EndHooks();
void SetSuppressHud(bool on);
void SetSkipResolve(bool on);
void SetPlayerHudSlot(uint32_t slot);
int Eye();
bool OffsetViews();
bool ReplaceProjection();
bool SuppressHud();
bool SkipResolve();
uint32_t PlayerHudSlot();

void ViewOffset(float out[16]);
void PreAspectProjection(float out[16]);
void MonoFinalProjection(float full_aspect, float out[16]);
float FullAspect(float device_w, float device_h);
float HudScalar(float device_w, float device_h);
float NearPlane();

void NoteResolve(uint32_t dev, uint32_t rt, uint32_t x, uint32_t y);
bool ColorTarget(uint32_t& dev, uint32_t& rt);

// Queues the pose this frame actually rendered with. `layout` matches
// rex::graphics::vr::FrameLayout.
void PublishFrame(uint32_t layout);

int ForcedCameraMode();
bool MenuIsPointer();
bool ViewportFloats();
// 0 quad (draw once, no per-eye HUD), 1 baked into each eye, 2 skipped.
int HudMode();

void RequestRecenter();

}  // namespace vig8::vr
