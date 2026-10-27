// Host-side checks for the VR math. Not part of the game build.
//   g++ -std=c++20 project/src/vr/vr_math.cpp project/src/vr/vr_camera.cpp project/src/vr/vr_math_test.cpp -o /tmp/vr_math_test

#include "vr_camera.h"
#include "vr_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int g_fails = 0;

void Expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s\n", msg);
    ++g_fails;
  }
}

bool Near(float a, float b, float eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }

}  // namespace

int main() {
  using vig8::vr::AsymmetricProjection;
  using vig8::vr::DetectMatrices;
  using vig8::vr::EyeOffsetMatrix;
  using vig8::vr::FinalProjection;
  using vig8::vr::Fov;
  using vig8::vr::Identity;
  using vig8::vr::Mat4;
  using vig8::vr::MatrixClass;
  using vig8::vr::MatrixHit;
  using vig8::vr::Mul;
  using vig8::vr::Quat;

  // Symmetric 90 degree frustum, aspect factor 1, near 0.1.
  // tan(45 deg) = 1, so the scale terms are 1 and the offsets are 0.
  Fov fov{-0.78539816f, 0.78539816f, 0.78539816f, -0.78539816f};
  Mat4 proj = AsymmetricProjection(fov, 1.0f, 0.1f, 1.0f);
  Expect(Near(proj.m[0], 1.0f), "proj sx");
  Expect(Near(proj.m[5], 1.0f), "proj sy");
  Expect(Near(proj.m[8], 0.0f), "proj ox");
  Expect(Near(proj.m[9], 0.0f), "proj oy");
  Expect(Near(proj.m[11], 1.0f), "proj w");
  Expect(Near(proj.m[14], -0.2f), "proj near term");

  // y_sign flips only the vertical offset, which is 0 for a symmetric fov.
  // Use an asymmetric vertical fov so the sign is visible.
  Fov asym = fov;
  asym.angle_up = 0.6f;
  asym.angle_down = -0.4f;
  Mat4 up = AsymmetricProjection(asym, 1.0f, 0.1f, 1.0f);
  Mat4 down = AsymmetricProjection(asym, 1.0f, 0.1f, -1.0f);
  Expect(Near(up.m[9], -down.m[9]) && std::fabs(up.m[9]) > 0.01f, "y sign flips oy");

  // After the game multiplies column 1 by -aspect, the final offset is the
  // negation of the pre-aspect offset (aspect 1, so the scale matches).
  Mat4 fin_asym = FinalProjection(asym, 0.1f, 1.0f);
  Expect(Near(fin_asym.m[9], -up.m[9]), "final oy is the flipped pre oy");

  // Final reversed-Z form negates the vertical scale.
  Mat4 final_p = FinalProjection(fov, 0.1f, 1.0f);
  Expect(Near(final_p.m[0], 1.0f), "final sx");
  Expect(Near(final_p.m[5], -1.0f), "final sy negated");
  Expect(Near(final_p.m[11], 1.0f), "final w");
  Expect(Near(final_p.m[14], 0.1f), "final near");

  // 1280x720's stored projection: sx 1, sy -16/9, reversed near 0.1.
  Mat4 native = vig8::vr::GameFinalProjection(16.0f / 9.0f, 0.1f);
  Expect(Near(native.m[0], 1.0f), "native sx");
  Expect(Near(native.m[5], -1.7777778f, 1.0e-5f), "native sy");
  Expect(Near(native.m[11], 1.0f), "native w");
  Expect(Near(native.m[14], 0.1f), "native near");
  Expect(Near(native.m[10], 0.0f) && Near(native.m[15], 0.0f), "native z row");

  // Right eye at +3.2 cm. The view offset is the inverse, so the translation
  // row shifts points by -3.2 cm on x. Y and Z stay put.
  Quat id{0, 0, 0, 1};
  Mat4 eye = EyeOffsetMatrix(id, vig8::vr::Vec3{0.032f, 0, 0}, 1.0f);
  Expect(Near(eye.m[12], -0.032f), "right eye translates -x");
  Expect(Near(eye.m[13], 0.0f), "right eye y");
  Expect(Near(eye.m[14], 0.0f), "right eye z");
  Mat4 left = EyeOffsetMatrix(id, vig8::vr::Vec3{-0.032f, 0, 0}, 1.0f);
  Expect(Near(left.m[12] - eye.m[12], 0.064f), "ipd baseline");

  // World scale 2 halves the baseline (the world feels larger).
  Mat4 scaled = EyeOffsetMatrix(id, vig8::vr::Vec3{0.032f, 0, 0}, 0.5f);
  Expect(Near(scaled.m[12], -0.016f), "world scale");

  // OpenXR up (+Y) becomes game up, which is -Y in the y-down view. The
  // inverse then puts +Y in the translation row.
  Mat4 up_pose = EyeOffsetMatrix(id, vig8::vr::Vec3{0, 1.0f, 0}, 1.0f);
  Expect(Near(up_pose.m[13], 1.0f), "openxr up maps to game up");

  // V_eye = V * E leaves an identity view equal to E.
  Mat4 combined = Mul(Identity(), eye);
  Expect(Near(combined.m[12], eye.m[12]), "view times offset");

  // 180 degrees about X. Row-vector (0, 1, 0, 1) must land on (0, -1, 0).
  vig8::vr::Rigid flipx{Quat{1, 0, 0, 0}, vig8::vr::Vec3{0, 0, 0}};
  Mat4 flipped = vig8::vr::InverseRigid(flipx);
  float px = flipped.m[4] + flipped.m[12];
  float py = flipped.m[5] + flipped.m[13];
  float pz = flipped.m[6] + flipped.m[14];
  Expect(Near(px, 0.0f) && Near(py, -1.0f) && Near(pz, 0.0f), "180 deg about x");

  // Same rotation with the camera one unit up the parent Y axis.
  flipx.t = vig8::vr::Vec3{0, 1, 0};
  Mat4 flipped_t = vig8::vr::InverseRigid(flipx);
  Expect(Near(flipped_t.m[12], 0.0f) && Near(flipped_t.m[13], 1.0f) && Near(flipped_t.m[14], 0.0f),
         "180 deg translation row");

  // Detector: the final 1280x720 projection from the findings, then a view.
  float packed[8 * 4] = {};
  uint16_t regs[8] = {0, 1, 2, 3, 10, 11, 12, 13};
  // proj at c0: [1,0,0,0] [0,-1.7778,0,0] [0,0,0,1] [0,0,0.1,0]
  packed[0] = 1;
  packed[5] = -1.7778f;
  packed[11] = 1;
  packed[14] = 0.1f;
  // view at c10: identity rotation, translation (1, 2, 3)
  packed[16] = 1;
  packed[21] = 1;
  packed[26] = 1;
  packed[28] = 1;
  packed[29] = 2;
  packed[30] = 3;
  packed[31] = 1;
  MatrixHit hits[4];
  int n = DetectMatrices(packed, regs, 8, hits, 4);
  Expect(n == 2, "two matrices");
  Expect(n >= 1 && hits[0].kind == MatrixClass::kPerspective && hits[0].reg == 0, "proj reg");
  Expect(n >= 2 && hits[1].kind == MatrixClass::kView && hits[1].reg == 10, "view reg");

  // An ortho HUD matrix is not a view.
  float ortho[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0.1f, 0, 0, 0, 0, 1};
  uint16_t oregs[4] = {4, 5, 6, 7};
  MatrixHit ohit[2];
  int on = DetectMatrices(ortho, oregs, 4, ohit, 2);
  Expect(on == 1 && ohit[0].kind == MatrixClass::kOrtho, "ortho hud");

  // Recenter, seated, and world scale. Eyes are 64 mm apart on OpenXR X.
  vig8::vr::RawEye left_eye{id, vig8::vr::Vec3{-0.032f, 0, 0}, fov};
  vig8::vr::RawEye right_eye{id, vig8::vr::Vec3{0.032f, 0, 0}, fov};
  vig8::vr::RecenterState ref;
  vig8::vr::CaptureRecenter(left_eye, right_eye, ref);
  vig8::vr::GameEye right = vig8::vr::MakeGameEye(right_eye, left_eye, ref, false, 1.0f, 1.0f);
  Expect(Near(right.offset.m[12], -0.032f), "recenter keeps the right eye");
  vig8::vr::RawEye left_moved = left_eye;
  vig8::vr::RawEye right_moved = right_eye;
  left_moved.p.x += 1.0f;
  right_moved.p.x += 1.0f;
  vig8::vr::GameEye seated = vig8::vr::MakeGameEye(right_moved, left_moved, ref, true, 1.0f, 1.0f);
  vig8::vr::GameEye chase = vig8::vr::MakeGameEye(right_moved, left_moved, ref, false, 1.0f, 1.0f);
  Expect(Near(seated.offset.m[12], -0.032f), "seated drops head translation");
  Expect(Near(chase.offset.m[12], -1.032f), "chase keeps head translation");
  vig8::vr::GameEye scaled_eye =
      vig8::vr::MakeGameEye(right_eye, left_eye, ref, false, 1.0f, 2.0f);
  Expect(Near(scaled_eye.offset.m[12], -0.016f), "world scale on the eye offset");
  vig8::vr::GameEye wide = vig8::vr::MakeGameEye(right_eye, left_eye, ref, false, 2.0f, 1.0f);
  Expect(Near(wide.offset.m[12], -0.064f), "ipd scale");

  if (g_fails) {
    std::fprintf(stderr, "%d failed\n", g_fails);
    return 1;
  }
  std::puts("vr_math_test ok");
  return 0;
}
