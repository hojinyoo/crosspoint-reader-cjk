#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// Fetch the user's Google Tasks and show them on-device.
//
// OAuth: user-consent refresh-token flow (Tasks is personal-account data, so a
// service account does not apply). Credentials live on the SD in /fridge.conf:
//   {"google":{"client_id":"...","client_secret":"...","refresh_token":"..."}}
// The refresh token is obtained once on a computer (see docs); the device only
// exchanges it for a short-lived access token (plain HTTPS POST — no JWT/RSA,
// no heavy OAuth library) and then calls the Tasks REST API.
class GoogleTasksActivity : public Activity {
  enum State { WIFI_SELECTION, FETCHING, SHOW, FAILED };

  struct Task {
    std::string title;
    bool done = false;
  };

  static constexpr int kMaxTasks = 20;

  State state = WIFI_SELECTION;
  std::string statusMsg;
  std::vector<Task> tasks;

  void onWifiSelectionComplete(bool success);
  void doFetch();
  void step(const char* msg);
  void fail(const char* msg);

  // Returns access token on success, empty on failure (sets statusMsg).
  std::string refreshAccessToken(const std::string& clientId, const std::string& clientSecret,
                                 const std::string& refreshToken);
  bool fetchTasks(const std::string& accessToken);

 public:
  explicit GoogleTasksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GoogleTasks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == FETCHING; }
  bool skipLoopDelay() override { return true; }
};
