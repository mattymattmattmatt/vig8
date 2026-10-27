// Vigilante 8 Arcade - the ReXGlue app. Startup, overlays (F3 stats, F4
// settings, ` console) and config persistence come from rex::ReXApp; this sets
// vig8's defaults and starts networking. Game and kernel overrides are in
// stubs.cpp and net.cpp.

#include "vig8_init.h"
#include "vr/vr_runtime.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>

void NetInit();
void NetShutdown();

class Vig8App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Vig8App>(new Vig8App(ctx, "vig8", PPCImageConfig));
  }

 protected:
  // Defaults, each applied only while the cvar still has the SDK's default, so a
  // value from the command line or the config file wins.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    auto set_default = [](const char* name, const char* sdk_default, const char* ours) {
      if (rex::cvar::GetFlagByName(name) == sdk_default) rex::cvar::SetFlagByName(name, ours);
    };
    // The full game rather than the XBLA trial.
    set_default("license_mask", "0", "4294967295");
    // The keyboard also plays as controller 1, so no pad is needed.
    set_default("mnk_mode", "false", "true");
    // Before the D3D12 device exists, so an OpenXR runtime can name the adapter.
    vig8::vr::EarlyInit();
  }

  // Game files live in <repo>/extracted, beside the build tree.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.game_data_root.empty())
      paths.game_data_root = rex::filesystem::GetExecutableFolder() / ".." / ".." / "extracted";
  }

  void OnPostSetup() override {
    NetInit();
    vig8::vr::BindKeys();
    rex::ui::RegisterBind("bind_fullscreen", "F11", "Toggle fullscreen", [] {
      bool on = rex::cvar::GetFlagByName("fullscreen") == "true";
      rex::cvar::SetFlagByName("fullscreen", on ? "false" : "true");
    });
  }

  void OnShutdown() override {
    vig8::vr::Shutdown();
    rex::ui::UnregisterBind("bind_fullscreen");
    NetShutdown();
  }
};

REX_DEFINE_APP(vig8, Vig8App::Create)
