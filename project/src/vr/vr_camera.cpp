#include "vr_camera.h"

namespace vig8::vr {
namespace {

Vec3 Mid(const Vec3& a, const Vec3& b) {
  return Vec3{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
}

Vec3 Sub(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 Add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }

Vec3 Scale(const Vec3& a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }

}  // namespace

void CaptureRecenter(const RawEye& left, const RawEye& right, RecenterState& ref) {
  ref.pos = Mid(left.p, right.p);
  ref.yaw = YawOnly(left.q);
  ref.set = true;
}

GameEye MakeGameEye(const RawEye& eye, const RawEye& other, const RecenterState& ref, bool seated,
                    float ipd_scale, float world_scale) {
  Vec3 mid = Mid(eye.p, other.p);
  Quat inv = QuatConjugate(ref.set ? ref.yaw : Quat{0, 0, 0, 1});
  Vec3 head = QuatRotate(inv, ref.set ? Sub(mid, ref.pos) : mid);
  Vec3 off = Scale(QuatRotate(inv, Sub(eye.p, mid)), ipd_scale);
  Vec3 pos = seated ? off : Add(head, off);
  float translation_scale = world_scale == 0.0f ? 1.0f : 1.0f / world_scale;
  GameEye out;
  out.offset = EyeOffsetMatrix(QuatMul(inv, eye.q), pos, translation_scale);
  out.fov = eye.fov;
  return out;
}

Fov FallbackFov() {
  constexpr float kHalf = 0.78539816f;  // 45 degrees, tan = 1, a 90 degree frustum
  return Fov{-kHalf, kHalf, kHalf, -kHalf};
}

}  // namespace vig8::vr
