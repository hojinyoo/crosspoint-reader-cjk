#include "GoogleTasksClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NetworkClientSecure.h>

#include <cctype>

namespace {
constexpr char kConfigPath[] = "/fridge.conf";
constexpr char kTokenUrl[] = "https://oauth2.googleapis.com/token";
// @default list, include completed so we can show check state; cap results.
// fields mask trims the reply to just what we render, keeping the buffered
// (de-chunked) body small on the ESP32-C3.
constexpr char kTasksUrl[] =
    "https://tasks.googleapis.com/tasks/v1/lists/@default/tasks"
    "?showCompleted=true&maxResults=20&fields=items(id,title,status)";
constexpr char kTaskBaseUrl[] = "https://tasks.googleapis.com/tasks/v1/lists/@default/tasks/";

// Refresh slightly before the real expiry to avoid racing the clock.
constexpr uint32_t kTokenSkewMs = 30 * 1000;

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

void GoogleTasksClient::invalidateToken() {
  accessToken_.clear();
  tokenExpiryMs_ = 0;
}

bool GoogleTasksClient::loadCreds() {
  if (credsLoaded_) return true;

  String j = Storage.exists(kConfigPath) ? Storage.readFile(kConfigPath) : String();
  if (j.isEmpty()) {
    lastError_ = "No /fridge.conf";
    return false;
  }
  JsonDocument cfg;
  if (deserializeJson(cfg, j.c_str())) {
    lastError_ = "Bad /fridge.conf";
    return false;
  }
  clientId_ = static_cast<const char*>(cfg["google"]["client_id"] | "");
  clientSecret_ = static_cast<const char*>(cfg["google"]["client_secret"] | "");
  refreshToken_ = static_cast<const char*>(cfg["google"]["refresh_token"] | "");
  if (clientId_.empty() || clientSecret_.empty() || refreshToken_.empty()) {
    lastError_ = "No Google creds in /fridge.conf";
    return false;
  }
  credsLoaded_ = true;
  return true;
}

GoogleTasksClient::Result GoogleTasksClient::refreshToken() {
  if (!loadCreds()) return Result::NO_CREDS;

  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, kTokenUrl)) {
    lastError_ = "token begin failed";
    return Result::HTTP_ERROR;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=refresh_token";
  body += "&client_id=" + urlEncode(clientId_);
  body += "&client_secret=" + urlEncode(clientSecret_);
  body += "&refresh_token=" + urlEncode(refreshToken_);

  const int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    LOG_ERR("GTASK", "token HTTP %d", code);
    http.end();
    if (code == HTTP_CODE_FORBIDDEN) {
      lastError_ = "Re-consent needed";
      return Result::PERMISSION;
    }
    lastError_ = "Token refresh failed";
    return Result::AUTH_FAILED;
  }

  // Token response is small; getString() is fine here (unlike the tasks list).
  const String resp = http.getString();
  http.end();

  JsonDocument d;
  if (deserializeJson(d, resp)) {
    lastError_ = "Bad token JSON";
    return Result::PARSE_ERROR;
  }
  const char* at = d["access_token"] | "";
  if (at[0] == '\0') {
    lastError_ = "No access_token";
    return Result::AUTH_FAILED;
  }
  const long expiresIn = d["expires_in"] | 3600L;  // seconds; Google default 3600
  accessToken_ = at;
  const uint32_t lifetimeMs = (expiresIn > 0 ? static_cast<uint32_t>(expiresIn) : 3600u) * 1000u;
  const uint32_t now = millis();
  tokenExpiryMs_ = now + (lifetimeMs > kTokenSkewMs ? lifetimeMs - kTokenSkewMs : lifetimeMs);
  if (tokenExpiryMs_ == 0) tokenExpiryMs_ = 1;  // reserve 0 for "no token"
  return Result::OK;
}

GoogleTasksClient::Result GoogleTasksClient::ensureToken(bool force) {
  if (force) invalidateToken();
  if (!accessToken_.empty() && tokenExpiryMs_ != 0) {
    // millis() wraps every ~49 days; treat a wrap as "stale" and refresh.
    const uint32_t now = millis();
    if (static_cast<int32_t>(tokenExpiryMs_ - now) > 0) {
      return Result::OK;  // still fresh
    }
  }
  return refreshToken();
}

GoogleTasksClient::Result GoogleTasksClient::fetchTasksOnce(std::vector<Task>& out, bool& outAuthExpired) {
  outAuthExpired = false;
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, kTasksUrl)) {
    lastError_ = "tasks begin failed";
    return Result::HTTP_ERROR;
  }
  http.addHeader("Authorization", String("Bearer ") + accessToken_.c_str());
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    LOG_ERR("GTASK", "tasks HTTP %d", code);
    http.end();
    if (code == HTTP_CODE_UNAUTHORIZED) {
      outAuthExpired = true;
      lastError_ = "Token expired";
      return Result::AUTH_FAILED;
    }
    if (code == HTTP_CODE_FORBIDDEN) {
      lastError_ = "Re-consent needed";
      return Result::PERMISSION;
    }
    lastError_ = "Tasks GET failed";
    return Result::HTTP_ERROR;
  }

  // The Tasks API replies with Transfer-Encoding: chunked. HTTPClient::getStream()
  // hands back the RAW socket — chunk-size hex prefixes (e.g. "5f0\r\n{...") still
  // inline — which ArduinoJson misreads as a bare number root: parsing "succeeds"
  // with an empty document, so the list silently renders "No tasks". getString()
  // de-chunks first (same path the token request already uses). The fields mask +
  // maxResults keep the buffered body small enough for the ESP32-C3.
  const String body = http.getString();
  http.end();

  // Filter keeps the parsed document tiny: only items[].{id,title,status} are kept.
  JsonDocument filter;
  JsonObject item = filter["items"].add<JsonObject>();
  item["id"] = true;
  item["title"] = true;
  item["status"] = true;

  JsonDocument d;
  const DeserializationError err = deserializeJson(d, body, DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("GTASK", "tasks JSON: %s", err.c_str());
    lastError_ = "Bad tasks JSON";
    return Result::PARSE_ERROR;
  }

  out.clear();
  for (JsonObject t : d["items"].as<JsonArray>()) {
    if (static_cast<int>(out.size()) >= kMaxTasks) break;
    Task task;
    task.id = static_cast<const char*>(t["id"] | "");
    task.title = static_cast<const char*>(t["title"] | "");
    task.done = String(static_cast<const char*>(t["status"] | "")) == "completed";
    if (!task.title.empty()) out.push_back(task);
  }
  return Result::OK;
}

GoogleTasksClient::Result GoogleTasksClient::patchTaskOnce(const std::string& taskId, bool done,
                                                           bool& outAuthExpired) {
  outAuthExpired = false;
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  const String url = String(kTaskBaseUrl) + urlEncode(taskId);
  if (!http.begin(client, url)) {
    lastError_ = "patch begin failed";
    return Result::HTTP_ERROR;
  }
  http.addHeader("Authorization", String("Bearer ") + accessToken_.c_str());
  http.addHeader("Content-Type", "application/json");

  // completed: set status; needsAction: also clear the completed timestamp.
  const char* body =
      done ? "{\"status\":\"completed\"}" : "{\"status\":\"needsAction\",\"completed\":null}";
  const int code = http.PATCH(reinterpret_cast<uint8_t*>(const_cast<char*>(body)), strlen(body));
  http.end();

  if (code == HTTP_CODE_OK || code == HTTP_CODE_NO_CONTENT) {
    return Result::OK;
  }
  LOG_ERR("GTASK", "patch HTTP %d", code);
  if (code == HTTP_CODE_UNAUTHORIZED) {
    outAuthExpired = true;
    lastError_ = "Token expired";
    return Result::AUTH_FAILED;
  }
  if (code == HTTP_CODE_FORBIDDEN) {
    lastError_ = "Re-consent needed";
    return Result::PERMISSION;
  }
  lastError_ = "Task update failed";
  return Result::HTTP_ERROR;
}

GoogleTasksClient::Result GoogleTasksClient::listTasks(std::vector<Task>& out) {
  Result tr = ensureToken(false);
  if (tr != Result::OK) return tr;

  bool authExpired = false;
  Result r = fetchTasksOnce(out, authExpired);
  if (r == Result::OK) return r;
  if (!authExpired) return r;  // 403/parse/http — do not retry

  // 401: discard token, force one refresh, retry once.
  tr = ensureToken(true);
  if (tr != Result::OK) return tr;
  r = fetchTasksOnce(out, authExpired);
  return r;
}

GoogleTasksClient::Result GoogleTasksClient::setTaskDone(const std::string& taskId, bool done) {
  if (taskId.empty()) {
    lastError_ = "Missing task id";
    return Result::HTTP_ERROR;
  }
  Result tr = ensureToken(false);
  if (tr != Result::OK) return tr;

  bool authExpired = false;
  Result r = patchTaskOnce(taskId, done, authExpired);
  if (r == Result::OK) return r;
  if (!authExpired) return r;  // 403/http — do not retry

  // 401: discard token, force one refresh, retry once.
  tr = ensureToken(true);
  if (tr != Result::OK) return tr;
  r = patchTaskOnce(taskId, done, authExpired);
  return r;
}
