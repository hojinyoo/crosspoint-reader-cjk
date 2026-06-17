#include "GoogleTasksActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char kConfigPath[] = "/fridge.conf";
constexpr char kTokenUrl[] = "https://oauth2.googleapis.com/token";
// @default list, include completed so we can show check state; cap results.
constexpr char kTasksUrl[] =
    "https://tasks.googleapis.com/tasks/v1/lists/@default/tasks?showCompleted=true&maxResults=20";

// Percent-encode a form value (refresh tokens / secrets can contain / + = &).
String urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0xF];
    }
  }
  return out;
}
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
  }
  requestUpdate();
}

std::string GoogleTasksActivity::refreshAccessToken(const std::string& clientId, const std::string& clientSecret,
                                                    const std::string& refreshToken) {
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, kTokenUrl)) {
    statusMsg = "token begin failed";
    return "";
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "client_id=" + urlEncode(clientId) + "&client_secret=" + urlEncode(clientSecret) +
                "&refresh_token=" + urlEncode(refreshToken) + "&grant_type=refresh_token";
  const int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    LOG_ERR("GTASK", "token HTTP %d", code);
    statusMsg = "Token refresh failed";
    http.end();
    return "";
  }
  const String resp = http.getString();
  http.end();
  JsonDocument d;
  if (deserializeJson(d, resp)) {
    statusMsg = "Bad token JSON";
    return "";
  }
  const char* at = d["access_token"] | "";
  if (at[0] == '\0') {
    statusMsg = "No access_token";
    return "";
  }
  return std::string(at);
}

bool GoogleTasksActivity::fetchTasks(const std::string& accessToken) {
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, kTasksUrl)) {
    statusMsg = "tasks begin failed";
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + accessToken.c_str());
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    LOG_ERR("GTASK", "tasks HTTP %d", code);
    statusMsg = "Tasks GET failed";
    http.end();
    return false;
  }
  const String resp = http.getString();
  http.end();
  JsonDocument d;
  if (deserializeJson(d, resp)) {
    statusMsg = "Bad tasks JSON";
    return false;
  }
  tasks.clear();
  for (JsonObject t : d["items"].as<JsonArray>()) {
    if (static_cast<int>(tasks.size()) >= kMaxTasks) break;
    Task task;
    task.title = static_cast<const char*>(t["title"] | "");
    task.done = String(static_cast<const char*>(t["status"] | "")) == "completed";
    if (!task.title.empty()) tasks.push_back(task);
  }
  return true;
}

void GoogleTasksActivity::doFetch() {
  String j = Storage.exists(kConfigPath) ? Storage.readFile(kConfigPath) : String();
  JsonDocument cfg;
  if (j.isEmpty() || deserializeJson(cfg, j.c_str())) return fail("No /fridge.conf");
  const std::string cid = static_cast<const char*>(cfg["google"]["client_id"] | "");
  const std::string csec = static_cast<const char*>(cfg["google"]["client_secret"] | "");
  const std::string rtok = static_cast<const char*>(cfg["google"]["refresh_token"] | "");
  if (cid.empty() || csec.empty() || rtok.empty()) return fail("No google creds in /fridge.conf");

  step("Refreshing token...");
  const std::string access = refreshAccessToken(cid, csec, rtok);
  if (access.empty()) return fail(statusMsg.c_str());

  step("Fetching tasks...");
  if (!fetchTasks(access)) return fail(statusMsg.c_str());

  {
    RenderLock lock;
    state = SHOW;
  }
  requestUpdate();
}

void GoogleTasksActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
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
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - lineH) / 2, statusMsg.c_str(), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SHOW) {
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    if (tasks.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, (pageHeight - lineH) / 2, "No tasks");
    }
    for (const auto& t : tasks) {
      if (y + lineH > pageHeight - lineH * 2) break;
      const std::string line = (t.done ? "[x] " : "[ ] ") + t.title;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.c_str());
      y += lineH + 4;
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void GoogleTasksActivity::loop() {
  if (state == SHOW || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}
