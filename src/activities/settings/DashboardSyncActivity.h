#pragma once

#include <string>

#include "activities/Activity.h"

// "Sync now" for the fridge dashboard.
//
// Flow: connect WiFi -> GET the GitHub pointer (endpoint.json, stable HTTPS URL)
// -> learn the home server's current LAN address -> download its rendered
// sleep.bmp over the LAN (HTTPS self-signed; the shared client is setInsecure)
// -> save to /sleep.bmp -> set Sleep Screen = Custom so the e-ink holds the
// dashboard at zero power. Every step is shown on-screen and appended to
// /fridge.log (USB is locked on this unit, so serial isn't available).
class DashboardSyncActivity : public Activity {
  enum State {
    WIFI_SELECTION,
    SYNCING,
    DONE,
    FAILED,
    SHUTTING_DOWN,
  };

  State state = WIFI_SELECTION;
  std::string statusMsg;  // current step, shown centered
  std::string logBuf;     // accumulated step log, written to /fridge.log

  void onWifiSelectionComplete(bool success);
  void doSync();
  void step(const char* msg);                 // set status + log + repaint
  void fail(const char* msg);                 // set FAILED + log + persist log
  void writeLog() const;

 public:
  explicit DashboardSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DashboardSync", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == SYNCING; }
  bool skipLoopDelay() override { return true; }
};
