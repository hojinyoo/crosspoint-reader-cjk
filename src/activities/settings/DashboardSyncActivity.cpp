#include "DashboardSyncActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
// Config lives on the SD card (NOT baked into firmware) so the public OTA build
// carries no secret and the token/endpoint can change without reflashing:
//   /fridge.conf  ->  {"endpoint":"https://raw.../endpoint.json","token":"..."}
constexpr char kConfigPath[] = "/fridge.conf";
// Default pointer if /fridge.conf is missing (carries only a LAN address).
constexpr char kDefaultEndpoint[] =
    "https://raw.githubusercontent.com/hojinyoo/fridge-dashboard/HEAD/endpoint.json";
constexpr char kSleepBmpPath[] = "/sleep.bmp";
constexpr char kLogPath[] = "/fridge.log";

struct SyncConfig {
  std::string endpoint = kDefaultEndpoint;
  std::string token;
};

SyncConfig loadConfig() {
  SyncConfig c;
  if (Storage.exists(kConfigPath)) {
    String j = Storage.readFile(kConfigPath);
    JsonDocument d;
    if (!deserializeJson(d, j.c_str())) {
      const char* ep = d["endpoint"] | "";
      const char* tk = d["token"] | "";
      if (ep[0] != '\0') c.endpoint = ep;
      c.token = tk;
    }
  }
  return c;
}

std::string withToken(const std::string& url, const std::string& token) {
  if (token.empty()) return url;
  std::string out = url;
  out += (url.find('?') == std::string::npos) ? "?" : "&";
  out += "token=";
  out += token;
  return out;
}
}  // namespace

void DashboardSyncActivity::step(const char* msg) {
  LOG_INF("FRIDGE", "%s", msg);
  logBuf += msg;
  logBuf += "\n";
  {
    RenderLock lock;
    statusMsg = msg;
  }
  requestUpdateAndWait();
}

void DashboardSyncActivity::writeLog() const {
  Storage.writeFile(kLogPath, String(logBuf.c_str()));
}

void DashboardSyncActivity::fail(const char* msg) {
  LOG_ERR("FRIDGE", "FAILED: %s", msg);
  logBuf += "FAILED: ";
  logBuf += msg;
  logBuf += "\n";
  writeLog();
  {
    RenderLock lock;
    statusMsg = msg;
    state = FAILED;
  }
  requestUpdate();
}

void DashboardSyncActivity::doSync() {
  const SyncConfig cfg = loadConfig();

  step("Fetching pointer (GitHub)...");
  std::string endpointJson;
  if (!HttpDownloader::fetchUrl(cfg.endpoint, endpointJson)) {
    return fail("Pointer fetch failed (offline?)");
  }

  JsonDocument doc;
  if (deserializeJson(doc, endpointJson.c_str())) {
    return fail("Bad pointer JSON");
  }
  const char* image = doc["image"] | "";
  if (!image || image[0] == '\0') {
    return fail("No image URL in pointer");
  }
  logBuf += "server: ";
  logBuf += image;
  logBuf += "\n";

  step("Downloading dashboard...");
  const std::string imageUrl = withToken(image, cfg.token);
  const HttpDownloader::DownloadError err = HttpDownloader::downloadToFile(imageUrl, kSleepBmpPath);
  if (err != HttpDownloader::OK) {
    return fail("Download failed (server off?)");
  }

  step("Setting sleep cover...");
  SETTINGS.sleepScreen = CrossPointSettings::CUSTOM;
  SETTINGS.saveToFile();

  logBuf += "OK\n";
  writeLog();
  {
    RenderLock lock;
    state = DONE;
  }
  requestUpdate();
}

void DashboardSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("FRIDGE", "WiFi connection failed, exiting");
    finish();
    return;
  }
  {
    RenderLock lock;
    state = SYNCING;
  }
  requestUpdateAndWait();
  doSync();
}

void DashboardSyncActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void DashboardSyncActivity::onExit() {
  Activity::onExit();
  // Turn off wifi (mirrors OtaUpdateActivity).
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void DashboardSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYNC_DASHBOARD));

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == SYNCING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMsg.c_str());
  } else if (state == DONE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, "Dashboard updated", true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMsg.c_str(), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void DashboardSyncActivity::loop() {
  if (state == DONE || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  }
}
