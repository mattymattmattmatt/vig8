// Guest addresses for the Vigilante 8 Arcade VR profile.
// From static analysis of the user's own default.xex. The overrides bind by
// symbol (REX_EXTERN(sub_...)); the data addresses below are what those
// functions read and write. Nothing here is game code.

#pragma once

#include <cstdint>

namespace vig8::vr::profile {

constexpr uint32_t kRender = 0x820BFA00;      // sub_820BFA00
constexpr uint32_t kViewport = 0x820BE790;    // sub_820BE790
constexpr uint32_t kBuildView = 0x820B8478;   // sub_820B8478
constexpr uint32_t kBuildProj = 0x8212B760;   // sub_8212B760
constexpr uint32_t kHud = 0x820B5348;         // sub_820B5348
constexpr uint32_t kMenu = 0x820AAA60;        // sub_820AAA60
constexpr uint32_t kResolve = 0x82132078;     // sub_82132078

constexpr uint32_t kDevice = 0x82482D98;      // render-device wrapper object
constexpr uint32_t kViewGlobal = 0x8247CAC0;  // 64 bytes, written by the viewport pass
constexpr uint32_t kProjGlobal = 0x8247CB20;

constexpr uint32_t kDeviceW = 0x38;
constexpr uint32_t kDeviceH = 0x3C;

constexpr uint32_t kOffPlayers = 0x610;
constexpr uint32_t kOffMenu = 0x670;
constexpr uint32_t kOffSlot0 = 0x7C8;  // HUD slot, embedded, not a pointer
constexpr uint32_t kOffCam0 = 0x7CC;   // camera pointer (slot + 4)
constexpr uint32_t kSlotStride = 0xAC;

constexpr uint32_t kCamWorld = 0x40;  // camera world matrix. Read only: physics and audio use it.
constexpr uint32_t kCamMode = 0x12C;  // 0 chase, 1 alternate, 2 debug, 3 frozen, 4 look-at, 5 spline

constexpr float kDefaultNear = 0.1f;
constexpr float kDefaultFocal = 1.0f;

}  // namespace vig8::vr::profile
