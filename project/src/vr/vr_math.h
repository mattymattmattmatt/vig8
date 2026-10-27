// View, projection, and shader-constant helpers for the VR camera.
// Row-major matrices, translation in row 3: a row vector multiplies on the left
// (p' = p * M), which is the game's convention (sub_821302A0).
//
// OpenXR is x-right, y-up, -z-forward. This game's view space is x-right,
// y-down, +z-forward (camera findings, section 2). Converting a pose negates
// y and z. The projection written for sub_8212B760 is the pre-aspect matrix
// from findings section 6; the game applies aspect and the reversed-Z
// conversion itself.

#pragma once

#include <cstdint>

namespace vig8::vr {

struct Mat4 {
  float m[16] = {};
};

struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

// OpenXR XrFovf, radians. angle_left and angle_down are negative.
struct Fov {
  float angle_left = 0, angle_right = 0, angle_up = 0, angle_down = 0;
};

struct Rigid {
  Quat q{};
  Vec3 t{};
};

Mat4 Identity();
Mat4 Mul(const Mat4& a, const Mat4& b);
Mat4 Transpose(const Mat4& a);

Quat QuatNormalize(Quat q);
Quat QuatConjugate(Quat q);
Quat QuatMul(Quat a, Quat b);
Vec3 QuatRotate(Quat q, Vec3 v);
// Yaw about OpenXR's up axis. Pitch and roll are dropped.
Quat YawOnly(Quat q);

// Row-major view matrix of the inverse of `pose` (pose is camera-from-parent).
Mat4 InverseRigid(const Rigid& pose);

// `q` / `t` are an OpenXR pose in meters. `translation_scale` converts meters
// to game units (1 / world_scale). The result is V's right-hand factor:
// V_eye = V * EyeOffsetMatrix(...).
Mat4 EyeOffsetMatrix(const Quat& q, const Vec3& t_meters, float translation_scale);

// Pre-aspect projection consumed by sub_820BE790. `aspect_a` is the game's f29.
// `y_sign` is +1 as analysed, or -1 if the image is vertically shifted.
Mat4 AsymmetricProjection(const Fov& fov, float aspect_a, float near_plane, float y_sign);

// Final reversed-Z matrix, the form stored at global 0x8247CB20. Used for the
// HUD globals and for the shader-constant fallback. [1][1] is negated, and the
// vertical offset is the pre-aspect offset after the game's column-1 multiply
// by -aspect, so one y_sign serves both matrices.
Mat4 FinalProjection(const Fov& fov, float near_plane, float y_sign);

// The untracked camera's final matrix: horizontal 90 degrees, vertical scale
// = -aspect, reversed infinite Z. This is what 1280x720 produces today.
Mat4 GameFinalProjection(float aspect, float near_plane);

enum class MatrixClass { kNone, kView, kPerspective, kOrtho };

struct MatrixHit {
  MatrixClass kind = MatrixClass::kNone;
  int reg = -1;
  float m[16] = {};
};

// `packed` is tightly packed float4s. `regs[i]` is the guest constant index of
// packed float4 i. Writes up to max_out hits of 4 consecutive registers.
int DetectMatrices(const float* packed, const uint16_t* regs, uint32_t count, MatrixHit* out,
                   int max_out);

// Replace the x/y scale and offset of a final projection, keeping its Z row.
void PatchFinalProjection(float m[16], const Fov& fov, float y_sign);

float Dot3(const float* a, const float* b);

}  // namespace vig8::vr
