#pragma once

#include <cstdint>
#include <string>
#include <vector>

// OAuth2 + Google Tasks REST client for the CrossPoint firmware.
//
// OAuth: user-consent refresh-token flow (Tasks is personal-account data, so a
// service account does not apply). Credentials live on the SD in /fridge.conf:
//   {"google":{"client_id":"...","client_secret":"...","refresh_token":"..."}}
// The refresh token is obtained once on a computer (see docs); the device only
// exchanges it for a short-lived access token (plain HTTPS POST — no JWT/RSA,
// no heavy OAuth library) and then calls the Tasks REST API over HTTPS.
//
// Memory: each list reply is de-chunked via getString() then parsed with an
// ArduinoJson field filter; a fields mask + maxResults keep the body to ~1-2 KB
// on a ~400KB-RAM part. Per-list results are capped at kMaxTasks.
class GoogleTasksClient {
 public:
  // A single Google Task. `id` is required by the write path (PATCH targets it).
  struct Task {
    std::string id;
    std::string title;
    std::string listId;  // the task list this task belongs to (for the check-off URL)
    bool done = false;
  };

  // A Google task list (a named collection of tasks).
  struct TaskList {
    std::string id;
    std::string title;
  };

  // Distinguishes failures so callers can render the right state.
  enum class Result {
    OK,
    NO_CREDS,       // /fridge.conf missing or missing google.{client_id,client_secret,refresh_token}
    AUTH_FAILED,    // token refresh failed (bad/revoked refresh_token, network at token endpoint)
    PERMISSION,     // HTTP 403 — insufficient scope / re-consent needed (do NOT retry)
    HTTP_ERROR,     // other non-success HTTP code or transport failure
    PARSE_ERROR,    // malformed JSON
  };

  static constexpr int kMaxTasks = 20;

  GoogleTasksClient() = default;

  // Read all of the user's task lists into `out` (cleared first). Same token
  // refresh + one-shot 401 retry behavior as listTasks().
  Result listTaskLists(std::vector<TaskList>& out);

  // Read one list's tasks into `out` (cleared first; each Task is stamped with
  // `listId`). Refreshes the access token if none is cached; on 401, discards
  // the cached token, forces one refresh, and retries once.
  Result listTasks(const std::string& listId, std::vector<Task>& out);

  // Mark a task complete (done=true) or needsAction (done=false) via PATCH on
  // its own list. Success = HTTP 200 || 204. 403 maps to PERMISSION (no retry);
  // 401 routes through the refresh+retry path.
  Result setTaskDone(const std::string& listId, const std::string& taskId, bool done);

  // Drop any cached access token (e.g. after /fridge.conf changes).
  void invalidateToken();

  // Human-readable message for the last failure (for on-device display).
  const std::string& lastError() const { return lastError_; }

 private:
  // Loads creds from /fridge.conf into members. Returns false (and sets
  // NO_CREDS state) when the file or any field is missing.
  bool loadCreds();

  // Ensure a fresh access token is cached, refreshing if missing/stale.
  // `force` discards the cached token first (used for the 401 retry path).
  Result ensureToken(bool force);

  // POST to the token endpoint; on success caches access_token + expiry.
  Result refreshToken();

  // One GET of the user's task lists with the current cached token.
  // `outAuthExpired` is set true on HTTP 401 for the refresh+retry path.
  Result fetchListsOnce(std::vector<TaskList>& out, bool& outAuthExpired);

  // One GET of `listId`'s tasks with the current cached token. `outAuthExpired`
  // is set true on HTTP 401 so the caller can run the refresh+retry path.
  Result fetchTasksOnce(const std::string& listId, std::vector<Task>& out, bool& outAuthExpired);

  // One PATCH of a task's status (on `listId`) with the current cached token.
  // `outAuthExpired` is set true on HTTP 401.
  Result patchTaskOnce(const std::string& listId, const std::string& taskId, bool done, bool& outAuthExpired);

  bool credsLoaded_ = false;
  std::string clientId_;
  std::string clientSecret_;
  std::string refreshToken_;

  std::string accessToken_;
  // Monotonic millis() value after which the cached token is considered stale.
  // 0 means "no token cached".
  uint32_t tokenExpiryMs_ = 0;

  std::string lastError_;
};
