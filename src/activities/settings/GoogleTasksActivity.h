#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "network/GoogleTasksClient.h"

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

  using Task = GoogleTasksClient::Task;
  using TaskList = GoogleTasksClient::TaskList;

  // A rendered row: a list header or a task. Task rows store only the index into
  // `tasks`, so the label is computed from the live task state (no stale copy).
  struct Row {
    bool isHeader = false;
    std::string headerText;  // list title (header rows only)
    int taskIndex = -1;      // index into `tasks` (task rows only)
  };

  static constexpr int kMaxTotalTasks = 50;  // safety cap across all lists

  State state = WIFI_SELECTION;
  std::string statusMsg;
  std::vector<Task> tasks;                 // aggregated across lists, grouped by list
  std::vector<TaskList> listsForHeaders;   // lists that contributed >=1 task, in order
  std::vector<Row> rows;                   // header + task rows for render/navigation
  int failedLists = 0;                     // lists that errored during the last fetch
  GoogleTasksClient client;

  // List navigation: index of the highlighted row. drawList() handles the
  // scroll window (it pages by selectedIndex), so we only track the cursor.
  int selectedIndex = 0;

  // Distinguishes the empty-list state from a fetch/auth/permission failure so
  // render() can show its own message (the list is empty in both cases).
  bool fetchedOk = false;

  // True while a check-off PATCH is in flight; ignore button presses until it
  // resolves (debounce against a double-toggle).
  bool writeInFlight = false;

  void onWifiSelectionComplete(bool success);
  void doFetch();
  void rebuildRows();   // rebuild `rows` from `tasks` and re-seat selectedIndex
  void toggleSelected();
  void step(const char* msg);
  void fail(const char* msg);

 public:
  explicit GoogleTasksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GoogleTasks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == FETCHING || writeInFlight; }
  bool skipLoopDelay() override { return true; }
};
