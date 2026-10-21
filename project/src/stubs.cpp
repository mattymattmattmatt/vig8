// Vigilante 8 Arcade: overrides of SDK kernel imports and of recompiled game
// functions. Each REX_EXTERN here replaces the SDK's or the generated weak
// definition at link time (/force:multiple). Options are cvars, so they appear
// in the F4 settings overlay and persist in the SDK's config file.

#include "vig8_pch.h"
#include "vig8_init.h"

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <cstring>

REXCVAR_DEFINE_UINT32(signed_in_players, 1, "Vig8",
                      "Local profiles signed in, 1-4. Set 2-4 for split-screen.");
REXCVAR_DEFINE_BOOL(unlock_all_cars, false, "Vig8",
                    "Unlock every vehicle (restart the game after changing)");

// ============================================================================
// Sign-in for up to four local players
// ============================================================================
// The SDK signs in user 0 only. Split-screen needs each player's user index to
// report signed in, or their controller never shows up in player selection.
// The state is 2 (signed in to Live), not 1 (local): with 1, the game's Xbox
// Live menu item does nothing, and System Link goes through that same check.

static bool UserSignedIn(uint32_t user) {
  return user < std::min<uint32_t>(REXCVAR_GET(signed_in_players), 4);
}

static constexpr uint64_t kXuidBase = 0xB13EBABEBABE0001ULL;  // + user index
static const char* const kUserNames[4] = {"Player 1", "Player 2", "Player 3", "Player 4"};
static constexpr uint32_t kInvalidArg = 0x80070057;
static constexpr uint32_t kNoSuchUser = 0x80070490;

REX_EXTERN(__imp__XamUserGetSigninState) {
  (void)base;
  ctx.r3.u64 = UserSignedIn(ctx.r3.u32) ? 2 : 0;
}

// X_USER_SIGNIN_INFO, 40 bytes: xuid +0, signin_state +12, name[16] +24.
REX_EXTERN(__imp__XamUserGetSigninInfo) {
  uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
  if (!info) {
    ctx.r3.u64 = kInvalidArg;
    return;
  }
  std::memset(base + info, 0, 40);
  if (!UserSignedIn(user)) {
    ctx.r3.u64 = kNoSuchUser;
    return;
  }
  REX_STORE_U64(info + 0, kXuidBase + user);
  REX_STORE_U32(info + 12, 2);
  std::memcpy(base + info + 24, kUserNames[user], std::strlen(kUserNames[user]) + 1);
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__XamUserGetXUID) {
  uint32_t user = ctx.r3.u32, xuid = ctx.r5.u32;
  if (!xuid) {
    ctx.r3.u64 = kInvalidArg;
    return;
  }
  bool in = UserSignedIn(user);
  REX_STORE_U64(xuid, in ? kXuidBase + user : 0);
  ctx.r3.u64 = in ? 0 : (user >= 4 ? kInvalidArg : kNoSuchUser);
}

REX_EXTERN(__imp__XamUserGetName) {
  uint32_t user = ctx.r3.u32, buf = ctx.r4.u32, len = ctx.r5.u32;
  if (!UserSignedIn(user)) {
    ctx.r3.u64 = user >= 4 ? kInvalidArg : kNoSuchUser;
    return;
  }
  uint32_t n = std::min<uint32_t>(len, uint32_t(std::strlen(kUserNames[user]) + 1));
  if (buf && n) {
    std::memcpy(base + buf, kUserNames[user], n);
    base[buf + n - 1] = 0;
  }
  ctx.r3.u64 = 0;
}

// Every privilege granted (online play, communications), so System Link and the
// Live menu open.
REX_EXTERN(__imp__XamUserCheckPrivilege) {
  uint32_t user = ctx.r3.u32, out = ctx.r5.u32;
  if (user != 0xFF && user >= 4) {
    ctx.r3.u64 = kInvalidArg;
    return;
  }
  if (out) REX_STORE_U32(out, 1);
  ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__XamUserGetMembershipTier) {
  (void)base;
  ctx.r3.u64 = ctx.r3.u32 < 4 ? 6 /*Gold*/ : kInvalidArg;
}

REX_EXTERN(__imp__XamUserIsOnlineEnabled) {
  (void)base;
  ctx.r3.u64 = 1;
}

// The SDK's version reports one user; this reports every signed-in slot.
REX_EXTERN(__imp__XamShowSigninUI) {
  (void)base;
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; i++)
    if (UserSignedIn(i)) mask |= 1u << i;
  REX_KERNEL_STATE()->BroadcastNotification(0x0000000A /*XN_SYS_SIGNINCHANGED*/, mask);
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009 /*XN_SYS_UI off*/, 0);
  ctx.r3.u64 = 0;
}

// ============================================================================
// Don't quit to a dashboard that isn't there
// ============================================================================
// When an Xbox Live session fails, the game calls these to return to the
// dashboard; the SDK's versions end the process. Ignoring them leaves the game
// on its error screen, from which the player can back out.
REX_EXTERN(__imp__XamLoaderTerminateTitle) {
  (void)base;
  ctx.r3.u64 = 0;
}
REX_EXTERN(__imp__XamLoaderLaunchTitle) {
  (void)base;
  ctx.r3.u64 = 0;
}

// ============================================================================
// Unlock all vehicles
// ============================================================================
// sub_821B80F0 reads a vehicle record; bits 16-19 of the word at +196 (mask
// 0xF0000) are its unlock state, and zero means locked. Setting them before the
// original runs makes every vehicle selectable.
REX_EXTERN(sub_821B80F0) {
  if (REXCVAR_GET(unlock_all_cars) && ctx.r3.u32) {
    uint32_t p = ctx.r3.u32 + 196;
    REX_STORE_U32(p, REX_LOAD_U32(p) | 0xF0000);
  }
  __imp__sub_821B80F0(ctx, base);
}

// ============================================================================
// Scripted controller for unattended test runs
// ============================================================================
// VIG8_AUTOPLAY=1 taps A for 200 ms every 3 s (after the first 10 s) on top of
// whatever the real pad reports, which walks the menus into a race with the
// default picks. VIG8_AUTOPLAY_DRIVE=<s> stops the taps after that many seconds
// and drives instead: right trigger held, steering slowly side to side, A
// (fire) every 2 s. Unset, this is a plain pass-through to the SDK. It works
// without window focus, which the SDK otherwise needs before it reports input.
// ponytail: one fixed script; a button@time list when a test needs other menus.
#include <rex/input/input_system.h>
#include <rex/runtime.h>

#include <chrono>
#include <cmath>
#include <cstdlib>

REX_EXTERN(__imp__XamInputGetState) {
  uint32_t user = ctx.r3.u32, flags = ctx.r4.u32, state_ptr = ctx.r5.u32;
  if ((flags & 0xFF) && (flags & 1 /*XINPUT_FLAG_GAMEPAD*/) == 0) {
    ctx.r3.u64 = 0x48F;  // X_ERROR_DEVICE_NOT_CONNECTED
    return;
  }
  if ((user & 0xFF) == 0xFF || (flags & 0x40000000 /*XINPUT_FLAG_ANY_USER*/)) user = 0;
  auto* is = static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
  auto* state = state_ptr ? REX_KERNEL_MEMORY()->TranslateVirtual<rex::input::X_INPUT_STATE*>(state_ptr)
                          : nullptr;
  uint32_t result = is->GetState(user, state);

  static const char* autoplay = std::getenv("VIG8_AUTOPLAY");
  if (autoplay && autoplay[0] == '1' && user == 0 && state) {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    static std::atomic<uint32_t> packet{1};
    static const long long drive_ms = [] {
      const char* v = std::getenv("VIG8_AUTOPLAY_DRIVE");
      return v ? std::atoll(v) * 1000 : -1;
    }();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
    if (result != 0) std::memset(state, 0, sizeof(*state));
    auto& pad = state->gamepad;
    if (drive_ms >= 0 && ms > drive_ms) {
      pad.right_trigger = 255;
      pad.thumb_lx = int16_t(12000 * std::sin(double(ms - drive_ms) / 2500.0));
      if ((ms - drive_ms) % 2000 < 200) pad.buttons = uint16_t(pad.buttons | 0x1000);  // A
    } else if (ms > 10000 && ms % 3000 < 200) {
      pad.buttons = uint16_t(pad.buttons | 0x1000);  // A
    }
    state->packet_number = packet.fetch_add(1);  // the game acts only on a new packet
    result = 0;
  }
  ctx.r3.u64 = result;
}
