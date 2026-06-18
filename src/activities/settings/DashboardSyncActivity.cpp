#include "DashboardSyncActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
// Fixed HTTPS URL of the encrypted dashboard (a GitHub Release asset, updated in
// place with `gh release upload --clobber`). Always available; no LAN server.
constexpr char kEncUrl[] =
    "https://github.com/hojinyoo/fridge-dashboard/releases/download/dashboard/dashboard.enc";
// AES-256 key lives ONLY here on the SD (and on the Mac) — never on GitHub:
//   /fridge.conf -> {"key":"<base64 of 32 bytes>"}
constexpr char kConfigPath[] = "/fridge.conf";
constexpr char kEncPath[] = "/sleep.enc";   // downloaded ciphertext
constexpr char kTmpPath[] = "/sleep.tmp";   // decrypt target (renamed on success)
constexpr char kSleepBmpPath[] = "/sleep.bmp";
constexpr char kLogPath[] = "/fridge.log";
constexpr size_t kNonceLen = 12;
constexpr size_t kTagLen = 16;
constexpr size_t kChunk = 4096;

// AES-256-GCM file format produced by encrypt_and_publish.py:
//   [ 12-byte nonce | ciphertext | 16-byte tag ]

bool loadKey(uint8_t out[32]) {
  if (!Storage.exists(kConfigPath)) return false;
  String j = Storage.readFile(kConfigPath);
  JsonDocument d;
  if (deserializeJson(d, j.c_str())) return false;
  const char* kb64 = d["key"] | "";
  if (kb64[0] == '\0') return false;
  size_t olen = 0;
  int rc = mbedtls_base64_decode(out, 32, &olen, reinterpret_cast<const unsigned char*>(kb64), strlen(kb64));
  return rc == 0 && olen == 32;
}

// Streaming GCM decrypt of /sleep.enc -> /sleep.tmp, then rename to /sleep.bmp
// only if the auth tag verifies. Never holds the whole image in RAM.
bool decryptEncToBmp(const uint8_t key[32], const char** err) {
  static uint8_t inbuf[kChunk];
  static uint8_t outbuf[kChunk];

  HalFile in;
  if (!Storage.openFileForRead("FRIDGE", kEncPath, in)) { *err = "open enc"; return false; }
  const size_t total = in.fileSize();
  if (total < kNonceLen + kTagLen) { *err = "enc too small"; in.close(); return false; }
  const size_t ctLen = total - kNonceLen - kTagLen;

  uint8_t nonce[kNonceLen];
  uint8_t tag[kTagLen];
  bool ok = in.read(nonce, kNonceLen) == static_cast<int>(kNonceLen) &&
            in.seek(total - kTagLen) && in.read(tag, kTagLen) == static_cast<int>(kTagLen) &&
            in.seek(kNonceLen);
  if (!ok) { *err = "read header"; in.close(); return false; }

  HalFile out;
  if (!Storage.openFileForWrite("FRIDGE", kTmpPath, out)) { *err = "open tmp"; in.close(); return false; }

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool failed = false;
  if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) { *err = "setkey"; failed = true; }
  if (!failed && mbedtls_gcm_starts(&gcm, MBEDTLS_GCM_DECRYPT, nonce, kNonceLen) != 0) { *err = "starts"; failed = true; }

  size_t remaining = ctLen;
  while (!failed && remaining > 0) {
    const size_t want = remaining < kChunk ? remaining : kChunk;
    const int n = in.read(inbuf, want);
    if (n <= 0) { *err = "read ct"; failed = true; break; }
    size_t olen = 0;
    if (mbedtls_gcm_update(&gcm, inbuf, n, outbuf, sizeof(outbuf), &olen) != 0) { *err = "gcm update"; failed = true; break; }
    if (olen && out.write(outbuf, olen) != olen) { *err = "write"; failed = true; break; }
    remaining -= n;
  }

  uint8_t computed[kTagLen];
  if (!failed) {
    size_t flen = 0;
    if (mbedtls_gcm_finish(&gcm, outbuf, sizeof(outbuf), &flen, computed, kTagLen) != 0) { *err = "finish"; failed = true; }
    else if (flen && out.write(outbuf, flen) != flen) { *err = "write fin"; failed = true; }
  }
  mbedtls_gcm_free(&gcm);
  in.close();
  out.close();

  if (failed) { Storage.remove(kTmpPath); return false; }

  uint8_t diff = 0;
  for (size_t i = 0; i < kTagLen; i++) diff |= computed[i] ^ tag[i];
  if (diff != 0) { *err = "tag mismatch"; Storage.remove(kTmpPath); return false; }

  Storage.remove(kSleepBmpPath);
  if (!Storage.rename(kTmpPath, kSleepBmpPath)) { *err = "rename"; Storage.remove(kTmpPath); return false; }
  return true;
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

void DashboardSyncActivity::writeLog() const { Storage.writeFile(kLogPath, String(logBuf.c_str())); }

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
  step("Loading key (/fridge.conf)...");
  uint8_t key[32];
  if (!loadKey(key)) return fail("No key in /fridge.conf");

  step("Downloading dashboard...");
  Storage.remove(kEncPath);
  if (HttpDownloader::downloadToFile(kEncUrl, kEncPath) != HttpDownloader::OK) {
    return fail("Download failed (offline?)");
  }

  step("Decrypting...");
  const char* err = "?";
  if (!decryptEncToBmp(key, &err)) {
    logBuf += err;
    logBuf += "\n";
    return fail(err);
  }

  step("Setting sleep cover...");
  SETTINGS.sleepScreen = CrossPointSettings::CUSTOM;
  SETTINGS.saveToFile();
  Storage.remove(kEncPath);  // drop ciphertext, keep only /sleep.bmp

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
