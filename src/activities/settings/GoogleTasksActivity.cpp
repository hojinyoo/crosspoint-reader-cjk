#include "GoogleTasksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

void GoogleTasksActivity::step(const char* msg) {
  LOG_INF("GTASK", "%s", msg);
  {
    RenderLock lock;
    statusMsg = msg;
  }
  requestUpdateAndWait();
}

void GoogleTasksActivity::fail(const char* msg) {
  LOG_ERR("GTASK", "FAILED: %s", msg);
  {
    RenderLock lock;
    statusMsg = msg;
    state = FAILED;
    fetchedOk = false;
  }
  requestUpdate();
}

// Map a client Result to a distinct, user-facing message. The client's own
// lastError() is a developer string; non-technical users get a clear hint.
static const char* messageForResult(GoogleTasksClient::Result r) {
  switch (r) {
    case GoogleTasksClient::Result::NO_CREDS:
      return "No Google creds in /fridge.conf";
    case GoogleTasksClient::Result::PERMISSION:
      return "Re-consent needed";
    case GoogleTasksClient::Result::AUTH_FAILED:
      return "Sign-in failed";
    case GoogleTasksClient::Result::PARSE_ERROR:
      return "Bad response from Google";
    case GoogleTasksClient::Result::HTTP_ERROR:
    default:
      return "Couldn't fetch tasks";
  }
}

void GoogleTasksActivity::doFetch() {
  step("Fetching tasks...");

  std::vector<TaskList> lists;
  const GoogleTasksClient::Result lr = client.listTaskLists(lists);
  if (lr != GoogleTasksClient::Result::OK) {
    return fail(messageForResult(lr));  // can't even enumerate lists -> global failure
  }

  // Aggregate every list's tasks. A per-list TRANSIENT error skips that list and
  // keeps going (partial results beat a total lockout); an account-wide error
  // (auth / permission / no-creds) aborts. Cap the total to bound memory.
  std::vector<Task> aggregated;
  std::vector<TaskList> kept;  // lists that contributed >=1 task, in order
  int failed = 0;
  bool capped = false;
  for (const TaskList& l : lists) {
    if (static_cast<int>(aggregated.size()) >= kMaxTotalTasks) {
      capped = true;
      break;
    }
    std::vector<Task> tmp;
    const GoogleTasksClient::Result r = client.listTasks(l.id, tmp);
    if (r != GoogleTasksClient::Result::OK) {
      if (r == GoogleTasksClient::Result::AUTH_FAILED || r == GoogleTasksClient::Result::NO_CREDS ||
          r == GoogleTasksClient::Result::PERMISSION) {
        return fail(messageForResult(r));  // account-wide -> abort the whole view
      }
      LOG_ERR("GTASK", "list '%s' failed: %s", l.title.c_str(), client.lastError().c_str());
      failed++;
      continue;  // transient -> skip this list, keep the rest
    }
    if (tmp.empty()) continue;  // drop lists with no titled tasks
    LOG_INF("GTASK", "list '%s': %u task(s)", l.title.c_str(), static_cast<unsigned>(tmp.size()));
    kept.push_back(l);
    for (Task& t : tmp) {
      if (static_cast<int>(aggregated.size()) >= kMaxTotalTasks) {
        capped = true;
        break;
      }
      aggregated.push_back(std::move(t));
    }
  }
  if (capped) LOG_INF("GTASK", "task list truncated at %d", kMaxTotalTasks);

  {
    RenderLock lock;
    tasks = std::move(aggregated);
    listsForHeaders = std::move(kept);
    failedLists = failed;
    fetchedOk = true;
    rebuildRows();  // builds `rows` and seats selectedIndex on the first task row
    state = SHOW;
  }
  requestUpdate();
}

// Rebuild the rendered row model from `tasks`, grouped under one header per list
// (using listsForHeaders for titles + order). Seats selectedIndex on the first
// task row so the cursor never starts on a header.
void GoogleTasksActivity::rebuildRows() {
  rows.clear();
  for (const TaskList& l : listsForHeaders) {
    bool headerEmitted = false;
    for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
      if (tasks[i].listId != l.id) continue;
      if (!headerEmitted) {
        Row h;
        h.isHeader = true;
        h.headerText = l.title;
        rows.push_back(std::move(h));
        headerEmitted = true;
      }
      Row r;
      r.taskIndex = i;
      rows.push_back(std::move(r));
    }
  }
  // Surface partial results: if some lists errored but others loaded, prepend a
  // non-selectable notice so the shorter list isn't silently misleading. Only
  // when there is at least one task row (keeps tasks.empty() == rows.empty()).
  if (!rows.empty() && failedLists > 0) {
    Row warn;
    warn.isHeader = true;
    warn.headerText = "! " + std::to_string(failedLists) + " list(s) unavailable";
    rows.insert(rows.begin(), std::move(warn));
  }
  selectedIndex = 0;
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (!rows[i].isHeader) {
      selectedIndex = i;
      break;
    }
  }
}

// Optimistically flip the selected row's done state, then PATCH it. On any
// failure (403 / offline / transport) restore the snapshot and surface the
// error. Button presses are ignored while the write is in flight (debounce).
void GoogleTasksActivity::toggleSelected() {
  if (writeInFlight) return;
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(rows.size())) return;
  const Row& row = rows[selectedIndex];
  if (row.isHeader) return;  // selection should never rest on a header; guard anyway
  const int ti = row.taskIndex;
  if (ti < 0 || ti >= static_cast<int>(tasks.size())) return;

  const std::string listId = tasks[ti].listId;
  const std::string taskId = tasks[ti].id;
  const bool priorDone = tasks[ti].done;  // snapshot for revert
  const bool newDone = !priorDone;

  // Optimistic flip + lock out further presses, then redraw immediately. The
  // label callback reads tasks[ti].done live, so the row reflects the flip.
  {
    RenderLock lock;
    tasks[ti].done = newDone;
    writeInFlight = true;
  }
  requestUpdateAndWait();

  const GoogleTasksClient::Result r = client.setTaskDone(listId, taskId, newDone);

  if (r == GoogleTasksClient::Result::OK) {
    RenderLock lock;
    writeInFlight = false;
    requestUpdate();
    return;
  }

  // Failure: revert the optimistic flip and show the matching error message.
  LOG_ERR("GTASK", "setTaskDone failed: %s", client.lastError().c_str());
  {
    RenderLock lock;
    // List is stable during an in-flight write (no refetch), so index/id still match.
    if (ti < static_cast<int>(tasks.size()) && tasks[ti].id == taskId) {
      tasks[ti].done = priorDone;
    }
    statusMsg = messageForResult(r);
    state = FAILED;
    fetchedOk = false;
    writeInFlight = false;
  }
  requestUpdate();
}

void GoogleTasksActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    // WiFi join failed (or user cancelled the picker): surface a distinct
    // offline state instead of silently finish()ing.
    fail("WiFi join failed");
    return;
  }
  {
    RenderLock lock;
    state = FETCHING;
  }
  requestUpdateAndWait();
  doFetch();
}

void GoogleTasksActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void GoogleTasksActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void GoogleTasksActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GOOGLE_TASKS));

  const auto lineH = renderer.getLineHeight(UI_10_FONT_ID);

  if (state == FETCHING) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - lineH) / 2, statusMsg.c_str());
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - lineH) / 2, statusMsg.c_str(), true, EpdFontFamily::BOLD);
    // Confirm re-fetches (retry / refresh from the error screen).
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // state == SHOW. tasks.empty() == rows.empty() by construction — a list header
  // (and the partial-results notice) is only emitted alongside >=1 task row.
  if (tasks.empty()) {
    // fetchedOk distinguishes a genuinely empty list from a soft state.
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - lineH) / 2, fetchedOk ? "No tasks" : "No tasks to show");
    // No rows to select; Left button still refreshes (see loop()).
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "Refresh", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const Rect contentRect = GUI.getContentRect(renderer, contentTop, metrics.verticalSpacing * 2);

  // drawList() truncates each row title with renderer.truncatedText, draws the
  // selection highlight, and pages the scroll window by selectedIndex. Korean
  // titles render via drawText's external-UI-font fallback.
  // Rows are list headers + task rows. Header rows render their list title;
  // selection never lands on them (see loop()). Task rows read the LIVE task
  // done-state so an optimistic check-off / revert shows without a row rebuild.
  GUI.drawList(renderer, contentRect, static_cast<int>(rows.size()), selectedIndex, [this](int index) {
    const Row& row = rows[index];
    if (row.isHeader) return row.headerText;
    const Task& t = tasks[row.taskIndex];
    return std::string(t.done ? "[x] " : "[ ] ") + t.title;
  });

  // Back / Select(check-off) / Up / Down. Left button = refresh (see loop()).
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void GoogleTasksActivity::loop() {
  // While a check-off PATCH is in flight, swallow all input (debounce).
  if (writeInFlight) return;

  // Back always exits.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state == FAILED) {
    // Confirm retries the fetch.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock;
        state = FETCHING;
      }
      requestUpdateAndWait();
      doFetch();
    }
    return;
  }

  if (state != SHOW) return;

  // Refresh re-fetches the list (Left button is otherwise unused in a vertical
  // Up/Down list). Also reload creds from /fridge.conf in case they changed.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    client.invalidateToken();
    {
      RenderLock lock;
      rows.clear();  // drop the stale model before re-fetching (selectedIndex re-seated in doFetch)
      selectedIndex = 0;
      state = FETCHING;
    }
    requestUpdateAndWait();
    doFetch();
    return;
  }

  if (rows.empty()) return;

  // Move, then skip any header rows in the same direction. The `!= startIdx`
  // guard bounds the loop so a degenerate all-header model can never spin forever.
  const int rowCount = static_cast<int>(rows.size());
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    RenderLock lock;
    const int startIdx = selectedIndex;
    do {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rowCount);
    } while (rows[selectedIndex].isHeader && selectedIndex != startIdx);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    RenderLock lock;
    const int startIdx = selectedIndex;
    do {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rowCount);
    } while (rows[selectedIndex].isHeader && selectedIndex != startIdx);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleSelected();
  }
}
