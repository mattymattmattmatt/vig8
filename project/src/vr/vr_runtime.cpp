#include "vr_runtime.h"

#include "vr_math.h"
#include "vr_openxr.h"

#include <rex/cvar.h>
#include <rex/graphics/vr_bridge.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <utility>

REXCVAR_DEFINE_BOOL(vr, false, "VR", "Render to a headset (OpenXR). Restart to change.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(vr_mode, "stereo", "VR", "stereo or theater (the whole frame on a virtual screen)");
REXCVAR_DEFINE_STRING(vr_render, "auto", "VR",
                      "auto, stereo, sbs, alternate, or constants. auto uses side-by-side until a resolve target is known");
REXCVAR_DEFINE_STRING(vr_camera, "chase", "VR", "chase (head position and orientation) or seated (orientation only)");
REXCVAR_DEFINE_DOUBLE(vr_world_scale, 1.0, "VR", "How big the world feels. 2 doubles every distance, including IPD");
REXCVAR_DEFINE_DOUBLE(vr_ipd_scale, 1.0, "VR", "Scales the runtime eye separation on top of world scale");
REXCVAR_DEFINE_DOUBLE(vr_near, 0.1, "VR", "Near plane in game units. The game uses 0.1");
REXCVAR_DEFINE_DOUBLE(vr_proj_y_sign, 1.0, "VR", "Flip to -1 if the view is shifted vertically");
REXCVAR_DEFINE_STRING(vr_hud, "quad", "VR", "quad (once, for a headset layer), baked (into each eye), or skip");
REXCVAR_DEFINE_DOUBLE(vr_hud_basis, 1080.0, "VR", "HUD scalar is the full-frame aspect times this. 1080 or 1440");
REXCVAR_DEFINE_BOOL(vr_menu_theater, true, "VR", "Show an open menu on the virtual screen instead of in stereo");
REXCVAR_DEFINE_STRING(vr_menu_mode, "pointer", "VR", "pointer (this+0x670 holds a pointer) or address (the object is there)");
REXCVAR_DEFINE_STRING(vr_widescreen, "auto", "VR", "auto, on, or off. Selects the game's 16:9 device factor");
REXCVAR_DEFINE_BOOL(vr_viewport_floats, false, "VR", "Pass viewport x/y/w/h as float bits. Default is integers");
REXCVAR_DEFINE_BOOL(vr_swap_eyes, false, "VR", "Swap the eyes");
REXCVAR_DEFINE_BOOL(vr_constant_fallback, false, "VR",
                    "Patch view and projection in shader constants. Stays off while a CPU stereo path is active");
REXCVAR_DEFINE_STRING(vr_pace, "game", "VR", "game submits whenever a frame is ready; headset waits briefly for one");
REXCVAR_DEFINE_DOUBLE(vr_screen_distance, 2.0, "VR", "Virtual screen distance in meters");
REXCVAR_DEFINE_DOUBLE(vr_screen_width, 2.5, "VR", "Virtual screen width in meters");
REXCVAR_DEFINE_BOOL(vr_show_mirror_quad, false, "VR", "Also show the whole presented image on the virtual screen");
REXCVAR_DEFINE_STRING(vr_quad_rect, "", "VR", "Optional crop of the quad image, as four normalized numbers: x y w h");
REXCVAR_DEFINE_STRING(vr_color_rt, "", "VR", "Guest color-target address in hex, if the learned one is wrong");
REXCVAR_DEFINE_INT32(vr_force_camera_mode, -1, "VR",
                     "Write this to cam+0x12C each frame. -1 leaves the game's mode alone");

namespace vig8::vr {
namespace {

rex::graphics::vr::Bridge g_bridge{};
uint32_t g_luid_low = 0;
int32_t g_luid_high = 0;
uint32_t g_feature_level = 0;
bool g_bridge_on = false;

std::mutex g_pose_mu;
GameEye g_eye[2]{};
RawStereo g_raw{};
bool g_pose_valid = false;
float g_aspect = 1.0f;
float g_y_sign = 1.0f;
RecenterState g_recenter{};
bool g_auto_recentered = false;

// Guest thread only.
bool g_offset = false;
bool g_replace = false;
bool g_suppress = false;
bool g_skip_resolve = false;
bool g_explicit = false;
int g_eye_index = 0;
int g_alternate = 0;
uint32_t g_hud_slot = 0;

struct CpuFrame {
  RawStereo raw{};
  uint32_t layout = 0;
  uint32_t eye = 0;
  bool pose_valid = false;
};
std::mutex g_queue_mu;
std::deque<CpuFrame> g_queue;
CpuFrame g_last{};
bool g_have_last = false;

struct ResolveNote {
  uint32_t dev = 0;
  uint32_t rt = 0;
  uint32_t x = 0;
  uint32_t y = 0;
};
ResolveNote g_resolves[16]{};
int g_resolve_count = 0;
uint32_t g_learned_dev = 0;
uint32_t g_learned_rt = 0;

bool g_logged_players = false;
bool g_logged_path = false;
bool g_rt_warned = false;
RenderPath g_last_path = RenderPath::kOriginal;
std::atomic<bool> g_recenter_request{false};

bool Same(const std::string& v, const char* s) { return v == s; }

int HudModeOf(const std::string& v) {
  if (Same(v, "baked")) return 1;
  if (Same(v, "skip")) return 2;
  return 0;
}

float AspectOf(float device_w, float device_h, float view_w, float view_h) {
  if (device_h < 1.0f || view_h < 1.0f || device_w < 1.0f) return 1.0f;
  const std::string& wide = REXCVAR_GET(vr_widescreen);
  bool widescreen = device_w / device_h > 1.5f;
  if (Same(wide, "on")) widescreen = true;
  if (Same(wide, "off")) widescreen = false;
  float factor = 1.0f;
  if (widescreen) factor = (16.0f * device_h) / (9.0f * device_w);
  return factor * view_w / view_h;
}

void SamplePose() {
  RawStereo raw{};
  bool ok = OpenXrReadPose(raw) && raw.valid;
  if (ok && REXCVAR_GET(vr_swap_eyes)) std::swap(raw.eye[0], raw.eye[1]);
  bool seated = Same(REXCVAR_GET(vr_camera), "seated");
  float ipd = static_cast<float>(REXCVAR_GET(vr_ipd_scale));
  float world = static_cast<float>(REXCVAR_GET(vr_world_scale));
  if (world == 0.0f) world = 1.0f;
  Fov fallback = FallbackFov();
  if (ok) {
    for (RawEye& e : raw.eye) {
      if (e.fov.angle_right <= e.fov.angle_left) e.fov = fallback;
    }
  } else {
    raw.eye[0].fov = raw.eye[1].fov = fallback;
    raw.eye[0].q = raw.eye[1].q = Quat{0, 0, 0, 1};
  }

  std::lock_guard<std::mutex> lock(g_pose_mu);
  if (ok && !g_auto_recentered) {
    CaptureRecenter(raw.eye[0], raw.eye[1], g_recenter);
    g_auto_recentered = true;
    REXLOG_INFO("VR: recentered (yaw only)");
  }
  if (g_recenter_request.exchange(false)) {
    if (ok) {
      CaptureRecenter(raw.eye[0], raw.eye[1], g_recenter);
      REXLOG_INFO("VR: recentered (yaw only)");
    }
  }
  g_raw = raw;
  g_pose_valid = ok;
  g_y_sign = static_cast<float>(REXCVAR_GET(vr_proj_y_sign));
  if (g_y_sign == 0.0f) g_y_sign = 1.0f;
  if (ok) {
    g_eye[0] = MakeGameEye(raw.eye[0], raw.eye[1], g_recenter, seated, ipd, world);
    g_eye[1] = MakeGameEye(raw.eye[1], raw.eye[0], g_recenter, seated, ipd, world);
  } else {
    g_eye[0] = GameEye{Identity(), fallback};
    g_eye[1] = g_eye[0];
  }
}

bool SelectAdapter(uint32_t* luid_low, int32_t* luid_high, uint32_t* min_feature_level, void*) {
  if (!luid_low || !luid_high || !min_feature_level) return false;
  *luid_low = g_luid_low;
  *luid_high = g_luid_high;
  *min_feature_level = g_feature_level;
  return true;
}

void OnDevice(void* device, void* queue, void*) { OpenXrOnDevice(device, queue); }

bool PatchConstants(rex::graphics::vr::VertexConstantSpan span, uint32_t eye, void*) {
  if (!span.values || !span.indices || span.count < 4) return false;
  GameEye eyes[2];
  float y_sign;
  {
    std::lock_guard<std::mutex> lock(g_pose_mu);
    eyes[0] = g_eye[0];
    eyes[1] = g_eye[1];
    y_sign = g_y_sign;
  }
  if (eye > 1) eye = 0;
  bool changed = false;
  bool did_persp = false;
  bool did_view = false;
  for (uint32_t i = 0; i + 3 < span.count; ++i) {
    if (span.indices[i + 1] != span.indices[i] + 1 || span.indices[i + 2] != span.indices[i] + 2 ||
        span.indices[i + 3] != span.indices[i] + 3) {
      continue;
    }
    float* m = span.values + i * 4;
    MatrixHit hit{};
    uint16_t regs[4] = {0, 1, 2, 3};
    if (DetectMatrices(m, regs, 4, &hit, 1) != 1) continue;
    if (hit.kind == MatrixClass::kPerspective && !did_persp) {
      // Shadow frusta in this game sit at a much larger near plane than 0.1.
      if (std::fabs(m[14]) <= 2.0f && std::fabs(m[0]) <= 4.0f) {
        PatchFinalProjection(m, eyes[eye].fov, y_sign);
        did_persp = true;
        changed = true;
      }
    } else if (hit.kind == MatrixClass::kView && !did_view) {
      Mat4 v{};
      std::memcpy(v.m, m, sizeof(v.m));
      Mat4 out = Mul(v, eyes[eye].offset);
      std::memcpy(m, out.m, sizeof(out.m));
      did_view = true;
      changed = true;
    }
    if (did_persp && did_view) break;
  }
  return changed;
}

void OnPresent(const rex::graphics::vr::PresentInfo& info, void*) {
  if (!OpenXrRunning()) return;
  SubmittedFrame frame{};
  frame.device = info.device;
  frame.queue = info.command_queue;
  frame.fence = info.fence;
  frame.resource = info.resource;
  frame.width = info.width;
  frame.height = info.height;
  frame.submission = info.submission;
  frame.layout = static_cast<uint32_t>(rex::graphics::vr::FrameLayout::kTheater);
  {
    std::lock_guard<std::mutex> lock(g_queue_mu);
    if (!g_queue.empty()) {
      CpuFrame cpu = g_queue.front();
      g_queue.pop_front();
      g_last = cpu;
      g_have_last = true;
      frame.raw = cpu.raw;
      frame.layout = cpu.layout;
      frame.eye = cpu.eye;
      frame.pose_valid = cpu.pose_valid;
    } else if (g_have_last) {
      frame.raw = g_last.raw;
      frame.layout = g_last.layout;
      frame.eye = g_last.eye;
      frame.pose_valid = g_last.pose_valid;
    }
  }
  OpenXrPresent(frame);
}

uint32_t ParseHex(const std::string& text) {
  std::string_view v(text);
  if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v.remove_prefix(2);
  uint32_t value = 0;
  auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), value, 16);
  if (ec != std::errc() || ptr != v.data() + v.size()) return 0;
  return value;
}

}  // namespace

void EarlyInit() {
  if (!REXCVAR_GET(vr)) return;
  StartResult started = OpenXrStart(&g_luid_low, &g_luid_high, &g_feature_level);
  if (started == StartResult::kNoLoader) {
    REXLOG_ERROR("VR: this build has no OpenXR loader. Run Setup.cmd -Vr and rebuild.");
    return;
  }
  if (started != StartResult::kOk) {
    REXLOG_ERROR("VR: OpenXR did not start. The game will stay flat. See the VR: lines above.");
    return;
  }
  g_bridge.select_adapter = SelectAdapter;
  g_bridge.on_d3d12_device = OnDevice;
  g_bridge.patch_vertex_constants = PatchConstants;
  g_bridge.on_present = OnPresent;
  rex::graphics::vr::SetBridge(&g_bridge);
  g_bridge_on = true;
  REXLOG_INFO("VR: OpenXR instance is up. Adapter LUID {:08X}:{:08X}",
              static_cast<uint32_t>(g_luid_high), g_luid_low);
}

void BindKeys() {
  if (!REXCVAR_GET(vr)) return;
  rex::ui::RegisterBind("bind_vr_recenter", "F9", "Recenter the VR view (yaw only)", RequestRecenter);
}

void Shutdown() {
  if (g_bridge_on) {
    rex::graphics::vr::SetBridge(nullptr);
    rex::graphics::vr::SetPatchEveryDraw(false);
    g_bridge_on = false;
  }
  OpenXrShutdown();
  if (REXCVAR_GET(vr)) rex::ui::UnregisterBind("bind_vr_recenter");
}

bool VrEnabled() { return REXCVAR_GET(vr); }

bool SessionActive() { return OpenXrRunning(); }

RenderPlan Plan(uint32_t player_count, bool menu_open) {
  RenderPlan plan;
  if (!SessionActive()) return plan;
  if (player_count != 1) {
    if (!g_logged_players) {
      REXLOG_INFO("VR: {} players. Stereo stays off; the frame is on the virtual screen.",
                  player_count);
      g_logged_players = true;
    }
    plan.path = RenderPath::kTheater;
    return plan;
  }
  if (Same(REXCVAR_GET(vr_mode), "theater") || (menu_open && REXCVAR_GET(vr_menu_theater))) {
    plan.path = RenderPath::kTheater;
    return plan;
  }
  const std::string& render = REXCVAR_GET(vr_render);
  if (Same(render, "alternate")) {
    plan.path = RenderPath::kAlternate;
    return plan;
  }
  if (Same(render, "sbs")) {
    plan.path = RenderPath::kSbs;
    return plan;
  }
  if (Same(render, "stereo")) {
    plan.path = RenderPath::kStereo;
    return plan;
  }
  // auto, empty, or constants. The shader patch is never combined with a CPU eye pass.
  if (Same(render, "constants") || (REXCVAR_GET(vr_constant_fallback) && !Same(render, "sbs") &&
                                    !Same(render, "stereo") && !Same(render, "alternate"))) {
    plan.patch_constants = true;
    plan.path = RenderPath::kOriginal;
    return plan;
  }
  uint32_t dev = 0, rt = 0;
  plan.path = ColorTarget(dev, rt) ? RenderPath::kStereo : RenderPath::kSbs;
  return plan;
}

void BeginFrame(RenderPath path, float device_w, float device_h, float view_w, float view_h) {
  SamplePose();
  g_aspect = AspectOf(device_w, device_h, view_w, view_h);
  g_explicit = path == RenderPath::kStereo || path == RenderPath::kAlternate;
  g_offset = path == RenderPath::kSbs || path == RenderPath::kStereo || path == RenderPath::kAlternate;
  g_replace = g_offset;
  int hud = HudModeOf(REXCVAR_GET(vr_hud));
  g_suppress = g_offset && hud != 1;
  g_skip_resolve = false;
  bool patch = path == RenderPath::kOriginal &&
               (Same(REXCVAR_GET(vr_render), "constants") || REXCVAR_GET(vr_constant_fallback));
  if (path == RenderPath::kAlternate || patch) {
    g_eye_index = g_alternate & 1;
    g_alternate++;
  } else {
    g_eye_index = 0;
  }
  rex::graphics::vr::SetEye(static_cast<uint32_t>(g_eye_index));
  rex::graphics::vr::SetPatchEveryDraw(patch);
  if (path != g_last_path || !g_logged_path) {
    static const char* kNames[] = {"flat", "theater", "alternate", "side-by-side", "stereo"};
    auto n = static_cast<unsigned>(path);
    const char* name = patch ? "constants" : (n < 5 ? kNames[n] : "?");
    REXLOG_INFO("VR: render path {} (viewport {:.0f}x{:.0f}, aspect {:.4f})", name, view_w, view_h,
                g_aspect);
    g_last_path = path;
    g_logged_path = true;
  }
}

void SetExplicitEye(int eye) {
  g_eye_index = eye & 1;
  g_explicit = true;
  rex::graphics::vr::SetEye(static_cast<uint32_t>(g_eye_index));
}

void AdvanceEye() {
  if (g_explicit) return;
  g_eye_index = (g_eye_index + 1) & 1;
  rex::graphics::vr::SetEye(static_cast<uint32_t>(g_eye_index));
}

void EndHooks() {
  g_offset = false;
  g_replace = false;
  g_suppress = false;
  g_explicit = false;
}

void SetInactive() {
  EndHooks();
  rex::graphics::vr::SetPatchEveryDraw(false);
  rex::graphics::vr::SetFrameLayout(rex::graphics::vr::FrameLayout::kMono);
}

void SetSuppressHud(bool on) { g_suppress = on; }
void SetSkipResolve(bool on) { g_skip_resolve = on; }
void SetPlayerHudSlot(uint32_t slot) { g_hud_slot = slot; }
int Eye() { return g_eye_index & 1; }
bool OffsetViews() { return g_offset; }
bool ReplaceProjection() { return g_replace; }
bool SuppressHud() { return g_suppress; }
bool SkipResolve() { return g_skip_resolve; }
uint32_t PlayerHudSlot() { return g_hud_slot; }

void ViewOffset(float out[16]) {
  std::lock_guard<std::mutex> lock(g_pose_mu);
  std::memcpy(out, g_eye[g_eye_index & 1].offset.m, sizeof(g_eye[0].offset.m));
}

void PreAspectProjection(float out[16]) {
  Fov fov;
  float y_sign;
  float aspect;
  {
    std::lock_guard<std::mutex> lock(g_pose_mu);
    fov = g_eye[g_eye_index & 1].fov;
    y_sign = g_y_sign;
  }
  aspect = g_aspect;
  Mat4 m = AsymmetricProjection(fov, aspect, NearPlane(), y_sign);
  std::memcpy(out, m.m, sizeof(m.m));
}

void MonoFinalProjection(float full_aspect, float out[16]) {
  Mat4 m = GameFinalProjection(full_aspect, NearPlane());
  std::memcpy(out, m.m, sizeof(m.m));
}

float FullAspect(float device_w, float device_h) { return AspectOf(device_w, device_h, device_w, device_h); }

float HudScalar(float device_w, float device_h) {
  return FullAspect(device_w, device_h) * static_cast<float>(REXCVAR_GET(vr_hud_basis));
}

float NearPlane() {
  float n = static_cast<float>(REXCVAR_GET(vr_near));
  return n > 0.0f ? n : 0.1f;
}

void NoteResolve(uint32_t dev, uint32_t rt, uint32_t x, uint32_t y) {
  if (!rt) return;
  for (int i = 0; i < g_resolve_count; ++i) {
    if (g_resolves[i].dev == dev && g_resolves[i].rt == rt && g_resolves[i].x == x &&
        g_resolves[i].y == y) {
      return;
    }
  }
  if (g_resolve_count < 16) {
    g_resolves[g_resolve_count++] = ResolveNote{dev, rt, x, y};
    REXLOG_INFO("VR: resolve dev {:08X} target {:08X} at {},{}", dev, rt, x, y);
  }
  if (!g_learned_rt && x == 0) {
    g_learned_dev = dev;
    g_learned_rt = rt;
    REXLOG_INFO("VR: using color target {:08X} for manual stereo", rt);
  }
}

bool ColorTarget(uint32_t& dev, uint32_t& rt) {
  const std::string& text = REXCVAR_GET(vr_color_rt);
  uint32_t over = text.empty() ? 0 : ParseHex(text);
  if (!text.empty() && !over && !g_rt_warned) {
    REXLOG_WARN("VR: vr_color_rt '{}' is not hex", text);
    g_rt_warned = true;
  }
  if (over) {
    dev = g_learned_dev;
    rt = over;
    return true;
  }
  dev = g_learned_dev;
  rt = g_learned_rt;
  return rt != 0;
}

void PublishFrame(uint32_t layout) {
  CpuFrame frame{};
  {
    std::lock_guard<std::mutex> lock(g_pose_mu);
    frame.raw = g_raw;
    frame.pose_valid = g_pose_valid;
    frame.eye = static_cast<uint32_t>(g_eye_index & 1);
  }
  frame.layout = layout;
  std::lock_guard<std::mutex> lock(g_queue_mu);
  if (g_queue.size() >= 4) g_queue.pop_front();
  g_queue.push_back(frame);
  rex::graphics::vr::SetFrameLayout(static_cast<rex::graphics::vr::FrameLayout>(layout));
}

int ForcedCameraMode() { return REXCVAR_GET(vr_force_camera_mode); }

bool MenuIsPointer() { return !Same(REXCVAR_GET(vr_menu_mode), "address"); }

bool ViewportFloats() { return REXCVAR_GET(vr_viewport_floats); }

int HudMode() { return HudModeOf(REXCVAR_GET(vr_hud)); }

void RequestRecenter() { g_recenter_request.store(true); }

}  // namespace vig8::vr
