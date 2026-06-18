#include "GoogleTasksActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace {
constexpr char kCachePath[] = "/gtasks_cache.json";
}  // namespace

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
    offline = false;
    fetchedOk = true;
    rebuildRows();  // builds `rows` and seats selectedIndex on the first task row
    state = SHOW;
  }
  saveCache();  // persist for offline use (SD write while the render task is idle)
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
  // Prepend a non-selectable status notice (offline cache, or partial results) so
  // a stale/short list isn't silently misleading. Only when there is at least one
  // task row (keeps tasks.empty() == rows.empty()).
  if (!rows.empty()) {
    std::string notice;
    if (offline) {
      notice = "! offline - showing saved tasks";
    } else if (failedLists > 0) {
      notice = "! " + std::to_string(failedLists) + " list(s) unavailable";
    }
    if (!notice.empty()) {
      Row warn;
      warn.isHeader = true;
      warn.headerText = std::move(notice);
      rows.insert(rows.begin(), std::move(warn));
    }
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
  if (offline) return;  // cannot check off without WiFi (showing cached tasks)
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
  if (success) {
    offline = false;
    {
      RenderLock lock;
      state = FETCHING;
    }
    requestUpdateAndWait();
    doFetch();  // fetch fresh + save the cache
    return;
  }

  // No WiFi (auto-connect failed or the picker was cancelled): fall back to the
  // last tasks saved on SD instead of failing. loadCache reads SD outside the
  // render lock; the member swap happens under the lock.
  offline = true;
  std::vector<Task> cachedTasks;
  std::vector<TaskList> cachedLists;
  const bool haveCache = loadCache(cachedTasks, cachedLists);
  {
    RenderLock lock;
    if (haveCache) {
      tasks = std::move(cachedTasks);
      listsForHeaders = std::move(cachedLists);
      failedLists = 0;
      fetchedOk = true;
      rebuildRows();
      state = SHOW;
    } else {
      statusMsg = "No WiFi and no saved tasks";
      fetchedOk = false;
      state = FAILED;
    }
  }
  requestUpdate();
}

void GoogleTasksActivity::onEnter() {
  Activity::onEnter();
  startConnect();
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

  if (state == FETCHING || state == WIFI_SELECTION) {
    // WIFI_SELECTION: the WiFi picker child normally covers us, but guard against a
    // transient frame falling through to the SHOW path and flashing "No tasks".
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
    // No rows to select; Confirm re-fetches.
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Refresh", "", "");
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

  // Back / Select(check-off) / Previous / Next (Up|Left and Down|Right both move).
  // Offline (cached) view swaps Select for Refresh (reconnect) since check-off needs WiFi.
  const auto labels = offline
                          ? mappedInput.mapLabels(tr(STR_BACK), "Refresh", tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                          : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

// (Re)attempt WiFi, then fetch when connected or fall back to the saved cache.
// Used on entry and by the retry paths (empty + FAILED states).
void GoogleTasksActivity::startConnect() {
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

// Persist the aggregated tasks (grouped by list) to SD so they can be shown
// offline next time. Small (capped + only id/title/done), so a String is fine.
void GoogleTasksActivity::saveCache() const {
  JsonDocument doc;
  JsonArray jlists = doc["lists"].to<JsonArray>();
  for (const TaskList& l : listsForHeaders) {
    JsonObject jl = jlists.add<JsonObject>();
    jl["id"] = l.id;
    jl["title"] = l.title;
    JsonArray jt = jl["tasks"].to<JsonArray>();
    for (const Task& t : tasks) {
      if (t.listId != l.id) continue;
      JsonObject jo = jt.add<JsonObject>();
      jo["id"] = t.id;
      jo["title"] = t.title;
      jo["done"] = t.done;
    }
  }
  String out;
  if (serializeJson(doc, out) == 0 || out.isEmpty()) {
    LOG_ERR("GTASK", "cache serialize failed; keeping previous cache");
    return;  // don't overwrite a good cache with a truncated/empty document
  }
  if (!Storage.writeFile(kCachePath, out)) {
    LOG_ERR("GTASK", "cache write failed");
  }
}

// Read the cached tasks into the given vectors (caller swaps them in under the
// render lock). Returns true if at least one titled task was loaded.
bool GoogleTasksActivity::loadCache(std::vector<Task>& outTasks, std::vector<TaskList>& outLists) const {
  if (!Storage.exists(kCachePath)) return false;
  const String j = Storage.readFile(kCachePath);
  if (j.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, j.c_str())) return false;
  outTasks.clear();
  outLists.clear();
  for (JsonObject jl : doc["lists"].as<JsonArray>()) {
    TaskList tl;
    tl.id = static_cast<const char*>(jl["id"] | "");
    tl.title = static_cast<const char*>(jl["title"] | "");
    if (tl.id.empty()) continue;
    bool any = false;
    for (JsonObject jo : jl["tasks"].as<JsonArray>()) {
      Task t;
      t.id = static_cast<const char*>(jo["id"] | "");
      t.title = static_cast<const char*>(jo["title"] | "");
      t.listId = tl.id;
      t.done = jo["done"] | false;
      if (t.title.empty()) continue;
      outTasks.push_back(std::move(t));
      any = true;
    }
    if (any) outLists.push_back(std::move(tl));
  }
  return !outTasks.empty();
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
    // Confirm retries: re-attempt WiFi, then fetch (online) or load the cache.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) startConnect();
    return;
  }

  if (state != SHOW) return;

  if (rows.empty()) {
    // Empty list: Confirm retries (re-attempts WiFi). Back exits (handled above).
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) startConnect();
    return;
  }

  // Navigation matches the rest of the app's list screens: NEXT = Down OR Right,
  // PREVIOUS = Up OR Left. The device maps the physical nav buttons to Left/Right
  // (not Up/Down) in this orientation, so checking only Up/Down missed them.
  // After moving, skip header rows in the same direction; the `!= startIdx` guard
  // bounds the loop so a degenerate all-header model can never spin forever.
  const int rowCount = static_cast<int>(rows.size());
  const bool goNext = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                      mappedInput.wasPressed(MappedInputManager::Button::Right);
  const bool goPrev = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                      mappedInput.wasPressed(MappedInputManager::Button::Left);
  if (goNext) {
    RenderLock lock;
    const int startIdx = selectedIndex;
    do {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rowCount);
    } while (rows[selectedIndex].isHeader && selectedIndex != startIdx);
    requestUpdate();
  } else if (goPrev) {
    RenderLock lock;
    const int startIdx = selectedIndex;
    do {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rowCount);
    } while (rows[selectedIndex].isHeader && selectedIndex != startIdx);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Online: check off the selected task. Offline (cached view): reconnect/refresh.
    if (offline) {
      startConnect();
    } else {
      toggleSelected();
    }
  }
}
