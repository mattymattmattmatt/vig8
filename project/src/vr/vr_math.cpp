#include "vr_math.h"

#include <cmath>
#include <cstring>

namespace vig8::vr {
namespace {

constexpr float kEps = 1.0e-3f;

float Len3(float x, float y, float z) {
  return std::sqrt(x * x + y * y + z * z);
}

bool Near(float a, float b, float eps = kEps) { return std::fabs(a - b) <= eps; }

void Set(Mat4& m, int r, int c, float v) { m.m[r * 4 + c] = v; }
float At(const Mat4& m, int r, int c) { return m.m[r * 4 + c]; }
float At(const float* m, int r, int c) { return m[r * 4 + c]; }

}  // namespace

Mat4 Identity() {
  Mat4 m;
  m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
  return m;
}

Mat4 Mul(const Mat4& a, const Mat4& b) {
  Mat4 o;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += At(a, r, k) * At(b, k, c);
      Set(o, r, c, s);
    }
  }
  return o;
}

Mat4 Transpose(const Mat4& a) {
  Mat4 o;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) Set(o, r, c, At(a, c, r));
  return o;
}

Quat QuatNormalize(Quat q) {
  float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n < 1.0e-8f) return Quat{0, 0, 0, 1};
  float inv = 1.0f / n;
  return Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Quat QuatConjugate(Quat q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

Quat QuatMul(Quat a, Quat b) {
  return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Vec3 QuatRotate(Quat q, Vec3 v) {
  Quat p{v.x, v.y, v.z, 0};
  Quat r = QuatMul(QuatMul(q, p), QuatConjugate(q));
  return Vec3{r.x, r.y, r.z};
}

Quat YawOnly(Quat q) {
  q = QuatNormalize(q);
  return QuatNormalize(Quat{0, q.y, 0, q.w});
}

Mat4 InverseRigid(const Rigid& pose) {
  // Camera pose takes parent space into the camera: p_parent = R * p_local + t
  // (column vectors). The view matrix is the inverse. For the game's row
  // vectors that is p_local = p_parent * R - t * R, so the upper 3x3 is R and
  // row 3 is -t * R.
  Quat q = QuatNormalize(pose.q);
  float x = q.x, y = q.y, z = q.z, w = q.w;
  float xx = x * x, yy = y * y, zz = z * z;
  float xy = x * y, xz = x * z, yz = y * z;
  float wx = w * x, wy = w * y, wz = w * z;
  float r00 = 1 - 2 * (yy + zz), r01 = 2 * (xy - wz), r02 = 2 * (xz + wy);
  float r10 = 2 * (xy + wz), r11 = 1 - 2 * (xx + zz), r12 = 2 * (yz - wx);
  float r20 = 2 * (xz - wy), r21 = 2 * (yz + wx), r22 = 1 - 2 * (xx + yy);
  Mat4 m = Identity();
  Set(m, 0, 0, r00);
  Set(m, 0, 1, r01);
  Set(m, 0, 2, r02);
  Set(m, 1, 0, r10);
  Set(m, 1, 1, r11);
  Set(m, 1, 2, r12);
  Set(m, 2, 0, r20);
  Set(m, 2, 1, r21);
  Set(m, 2, 2, r22);
  Set(m, 3, 0, -(pose.t.x * r00 + pose.t.y * r10 + pose.t.z * r20));
  Set(m, 3, 1, -(pose.t.x * r01 + pose.t.y * r11 + pose.t.z * r21));
  Set(m, 3, 2, -(pose.t.x * r02 + pose.t.y * r12 + pose.t.z * r22));
  return m;
}

Mat4 EyeOffsetMatrix(const Quat& q_oxr, const Vec3& t_meters, float translation_scale) {
  // Axis flip: OpenXR (x, y, z) -> game view (x, -y, -z).
  Quat q{q_oxr.x, -q_oxr.y, -q_oxr.z, q_oxr.w};
  Vec3 t{t_meters.x * translation_scale, -t_meters.y * translation_scale,
         -t_meters.z * translation_scale};
  return InverseRigid(Rigid{q, t});
}

Mat4 AsymmetricProjection(const Fov& fov, float aspect_a, float near_plane, float y_sign) {
  float t_l = std::tan(fov.angle_left);
  float t_r = std::tan(fov.angle_right);
  float t_u = std::tan(fov.angle_up);
  float t_d = std::tan(fov.angle_down);
  float width = t_r - t_l;
  float height = (t_u - t_d) * (aspect_a == 0.0f ? 1.0f : aspect_a);
  if (std::fabs(width) < 1.0e-6f) width = 1.0f;
  if (std::fabs(height) < 1.0e-6f) height = 1.0f;
  Mat4 m;
  Set(m, 0, 0, 2.0f / width);
  Set(m, 2, 0, -(t_r + t_l) / width);
  Set(m, 1, 1, 2.0f / height);
  Set(m, 2, 1, y_sign * (t_u + t_d) / height);
  Set(m, 2, 3, 1.0f);
  Set(m, 3, 2, -2.0f * near_plane);
  return m;
}

Mat4 FinalProjection(const Fov& fov, float near_plane, float y_sign) {
  float t_l = std::tan(fov.angle_left);
  float t_r = std::tan(fov.angle_right);
  float t_u = std::tan(fov.angle_up);
  float t_d = std::tan(fov.angle_down);
  float width = t_r - t_l;
  float height = t_u - t_d;
  if (std::fabs(width) < 1.0e-6f) width = 1.0f;
  if (std::fabs(height) < 1.0e-6f) height = 1.0f;
  Mat4 m;
  Set(m, 0, 0, 2.0f / width);
  Set(m, 1, 1, -2.0f / height);
  Set(m, 2, 0, -(t_r + t_l) / width);
  // The game multiplies the pre-aspect column 1 by -aspect, which flips this
  // offset. Negate here so vr_proj_y_sign means the same thing on both paths.
  Set(m, 2, 1, -y_sign * (t_u + t_d) / height);
  Set(m, 2, 3, 1.0f);
  Set(m, 3, 2, near_plane);
  return m;
}

Mat4 GameFinalProjection(float aspect, float near_plane) {
  if (aspect == 0.0f) aspect = 1.0f;
  Mat4 m;
  Set(m, 0, 0, 1.0f);
  Set(m, 1, 1, -aspect);
  Set(m, 2, 3, 1.0f);
  Set(m, 3, 2, near_plane);
  return m;
}

void PatchFinalProjection(float m[16], const Fov& fov, float y_sign) {
  Mat4 repl = FinalProjection(fov, m[14], y_sign);
  // Keep the game's Z encoding (reversed infinite far, or whatever it wrote).
  float z0 = m[10], z1 = m[11], z2 = m[14], z3 = m[15];
  std::memcpy(m, repl.m, sizeof(repl.m));
  m[10] = z0;
  m[11] = z1;
  m[14] = z2;
  m[15] = z3;
  // FinalProjection put the perspective 1 in m[11]. If the game's 1 lives
  // there, the copy above just restored it. If a y-flip put the 1 elsewhere,
  // the restored z1 is the game's value, which is what the shaders expect.
  (void)repl;
}

float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

int DetectMatrices(const float* packed, const uint16_t* regs, uint32_t count, MatrixHit* out,
                   int max_out) {
  if (!packed || !regs || !out || max_out <= 0) return 0;
  int hits = 0;
  for (uint32_t i = 0; i + 3 < count && hits < max_out; ++i) {
    int r0 = regs[i];
    if (regs[i + 1] != r0 + 1 || regs[i + 2] != r0 + 2 || regs[i + 3] != r0 + 3) continue;
    float m[16];
    std::memcpy(m, packed + i * 4, sizeof(m));

    auto row_is_scale = [&](int row, int col) {
      for (int c = 0; c < 4; ++c) {
        if (c == col) continue;
        if (!Near(At(m, row, c), 0.0f, 2.0e-3f)) return false;
      }
      return std::fabs(At(m, row, col)) > 0.05f && std::fabs(At(m, row, col)) < 32.0f;
    };
    bool persp_w = Near(std::fabs(At(m, 2, 3)), 1.0f, 2.0e-2f) && Near(At(m, 3, 3), 0.0f, 2.0e-2f);
    if (persp_w && row_is_scale(0, 0) && row_is_scale(1, 1)) {
      out[hits].kind = MatrixClass::kPerspective;
      out[hits].reg = r0;
      std::memcpy(out[hits].m, m, sizeof(m));
      ++hits;
      i += 3;
      continue;
    }

    bool ortho = Near(At(m, 3, 3), 1.0f) && Near(At(m, 2, 3), 0.0f) && row_is_scale(0, 0) &&
                 row_is_scale(1, 1) && Near(At(m, 0, 3), 0.0f) && Near(At(m, 1, 3), 0.0f);
    // A view matrix is also "ortho-shaped" (m33 = 1, m03 = m13 = m23 = 0) but
    // its upper 3x3 is a rotation. Classify that first.
    bool view = Near(At(m, 3, 3), 1.0f) && Near(At(m, 0, 3), 0.0f) && Near(At(m, 1, 3), 0.0f) &&
                Near(At(m, 2, 3), 0.0f);
    if (view) {
      float r[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
      float l0 = Len3(r[0][0], r[0][1], r[0][2]);
      float l1 = Len3(r[1][0], r[1][1], r[1][2]);
      float l2 = Len3(r[2][0], r[2][1], r[2][2]);
      float d01 = r[0][0] * r[1][0] + r[0][1] * r[1][1] + r[0][2] * r[1][2];
      float d02 = r[0][0] * r[2][0] + r[0][1] * r[2][1] + r[0][2] * r[2][2];
      float d12 = r[1][0] * r[2][0] + r[1][1] * r[2][1] + r[1][2] * r[2][2];
      if (Near(l0, 1.0f, 0.05f) && Near(l1, 1.0f, 0.05f) && Near(l2, 1.0f, 0.05f) &&
          std::fabs(d01) < 0.05f && std::fabs(d02) < 0.05f && std::fabs(d12) < 0.05f) {
        out[hits].kind = MatrixClass::kView;
        out[hits].reg = r0;
        std::memcpy(out[hits].m, m, sizeof(m));
        ++hits;
        i += 3;
        continue;
      }
    }
    // `view` is also true for an ortho HUD matrix (m33 = 1, no perspective
    // row). A real view already continued above, so this is the HUD case.
    if (ortho) {
      out[hits].kind = MatrixClass::kOrtho;
      out[hits].reg = r0;
      std::memcpy(out[hits].m, m, sizeof(m));
      ++hits;
      i += 3;
    }
  }
  return hits;
}

}  // namespace vig8::vr
