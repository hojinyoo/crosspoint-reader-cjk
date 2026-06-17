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
  std::vector<Task> fetched;
  const GoogleTasksClient::Result r = client.listTasks(fetched);
  if (r != GoogleTasksClient::Result::OK) {
    return fail(messageForResult(r));
  }

  {
    RenderLock lock;
    tasks = std::move(fetched);
    state = SHOW;
    fetchedOk = true;
    if (selectedIndex >= static_cast<int>(tasks.size())) {
      selectedIndex = tasks.empty() ? 0 : static_cast<int>(tasks.size()) - 1;
    }
  }
  requestUpdate();
}

// Optimistically flip the selected row's done state, then PATCH it. On any
// failure (403 / offline / transport) restore the snapshot and surface the
// error. Button presses are ignored while the write is in flight (debounce).
void GoogleTasksActivity::toggleSelected() {
  if (writeInFlight) return;
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(tasks.size())) return;

  const std::string taskId = tasks[selectedIndex].id;
  const bool priorDone = tasks[selectedIndex].done;  // snapshot for revert
  const bool newDone = !priorDone;

  // Optimistic flip + lock out further presses, then redraw immediately.
  {
    RenderLock lock;
    tasks[selectedIndex].done = newDone;
    writeInFlight = true;
  }
  requestUpdateAndWait();

  const GoogleTasksClient::Result r = client.setTaskDone(taskId, newDone);

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
    // Revert only if the row still refers to the same task (list is stable
    // here — no refetch happens during an in-flight write).
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(tasks.size()) &&
        tasks[selectedIndex].id == taskId) {
      tasks[selectedIndex].done = priorDone;
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

  // state == SHOW
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
  GUI.drawList(renderer, contentRect, static_cast<int>(tasks.size()), selectedIndex, [this](int index) {
    const auto& t = tasks[index];
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
      state = FETCHING;
    }
    requestUpdateAndWait();
    doFetch();
    return;
  }

  if (tasks.empty()) return;

  const int count = static_cast<int>(tasks.size());
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    RenderLock lock;
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    RenderLock lock;
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleSelected();
  }
}
