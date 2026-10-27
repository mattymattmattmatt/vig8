// Head pose, recenter, and the per-eye view offset.
// OpenXR poses stay in OpenXR space here. EyeOffsetMatrix does the axis flip.

#pragma once

#include "vr_math.h"

namespace vig8::vr {

struct RawEye {
  Quat q{};
  Vec3 p{};
  Fov fov{};
};

struct RawStereo {
  RawEye eye[2]{};
  bool valid = false;
};

struct RecenterState {
  Quat yaw{0, 0, 0, 1};
  Vec3 pos{};
  bool set = false;
};

struct GameEye {
  Mat4 offset{};
  Fov fov{};
};

// Yaw-only reference, taken from the midpoint of the two eyes.
void CaptureRecenter(const RawEye& left, const RawEye& right, RecenterState& ref);

// `seated` keeps orientation and the inter-eye offset, and drops head translation.
// `ipd_scale` multiplies only that inter-eye offset. `world_scale` converts
// meters to game units (larger means the world feels bigger).
GameEye MakeGameEye(const RawEye& eye, const RawEye& other, const RecenterState& ref, bool seated,
                    float ipd_scale, float world_scale);

// Symmetric 90 degree frustum, used until the runtime has located views.
Fov FallbackFov();

}  // namespace vig8::vr
