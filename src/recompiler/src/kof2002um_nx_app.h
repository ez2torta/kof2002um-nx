// kof2002um_nx - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#if REX_PLATFORM_SWITCH
#include <rex/ui/switch_apm.h>
#endif

class Kof2002umNxApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Kof2002umNxApp>(new Kof2002umNxApp(ctx, "kof2002um_nx",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
#if REX_PLATFORM_SWITCH
    // libstdc++ treats "sdmc:/..." as a relative path on Horizon; the runtime
    // then prefixes the executable directory and creates an invalid path.
    // libnx resolves this rooted form on the SD card's default device.
    paths.game_data_root = "/switch/kof2002um-nx/game";
#endif
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnPostSetup() override {
#if REX_PLATFORM_SWITCH
    // Without a request a handheld homebrew stays at GPU 307.2 MHz (stock-1: 17 fps). 460.8 with
    // the RAM at 1331.2 is the highest stock handheld profile retail games use (no overclock).
    RexSwitchApmRequestGpuMhz(460);
    RexSwitchApmApply();
#endif
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
