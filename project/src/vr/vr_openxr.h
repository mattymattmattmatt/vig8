// OpenXR session. The guest thread and the present callback never call xr*
// functions; the session thread does. Present copies the guest image and
// returns without waiting on the GPU.

#pragma once

#include "vr_camera.h"

#include <cstdint>

namespace vig8::vr {

enum class StartResult { kOk, kNoLoader, kRuntimeFailed };

struct SubmittedFrame {
  RawStereo raw{};
  uint32_t layout = 0;
  uint32_t eye = 0;
  bool pose_valid = false;
  void* device = nullptr;
  void* queue = nullptr;
  void* fence = nullptr;
  void* resource = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t submission = 0;
};

StartResult OpenXrStart(uint32_t* luid_low, int32_t* luid_high, uint32_t* min_feature_level);
void OpenXrOnDevice(void* device, void* queue);
void OpenXrShutdown();
bool OpenXrRunning();
bool OpenXrReadPose(RawStereo& out);
void OpenXrPresent(const SubmittedFrame& frame);

}  // namespace vig8::vr
