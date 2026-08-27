#include "AutoOpdsSync.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <OpdsParser.h>
#include <OpdsStream.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "WifiCredentialStore.h"
#include "network/AutoOpdsSyncSchedule.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/UrlUtils.h"

namespace AutoOpdsSync {
namespace {

constexpr size_t MAX_DOWNLOADS_PER_SERVER = 3;
constexpr unsigned long LAST_WIFI_TIMEOUT_MS = 12000;
constexpr unsigned long OTHER_WIFI_TIMEOUT_MS = 6000;
constexpr uint64_t MICROSECONDS_PER_SECOND = 1000000ULL;

bool isAutoServer(const OpdsServer& server) { return isAutoServerName(server.name); }

bool tryCredential(const WifiCredential& credential, const unsigned long timeoutMs) {
  LOG_INF("AODS", "Trying saved WiFi: %s", credential.ssid.c_str());

  // Credentials are owned by WifiCredentialStore. Keep Arduino's own NVS
  // auto-connect disabled, matching WifiSelectionActivity's manual flow.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  if (credential.password.empty()) {
    WiFi.begin(credential.ssid.c_str());
  } else {
    WiFi.begin(credential.ssid.c_str(), credential.password.c_str());
  }

  const unsigned long startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      LOG_INF("AODS", "WiFi connected: %s", credential.ssid.c_str());
      WIFI_STORE.setLastConnectedSsid(credential.ssid);
      return true;
    }
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) break;
    delay(100);
  }

  LOG_ERR("AODS", "WiFi connection failed: %s", credential.ssid.c_str());
  WiFi.disconnect();
  return false;
}

bool connectSavedWifi() {
  if (!WIFI_STORE.loadFromFile()) {
    LOG_ERR("AODS", "Could not load saved WiFi credentials");
    return false;
  }

  const size_t credentialCount = WIFI_STORE.getCredentialCount();
  if (credentialCount == 0) {
    LOG_ERR("AODS", "No saved WiFi credentials");
    return false;
  }

  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    const auto credential = WIFI_STORE.findCredential(lastSsid);
    if (credential && tryCredential(*credential, LAST_WIFI_TIMEOUT_MS)) return true;
  }

  for (size_t index = 0; index < credentialCount; ++index) {
    const auto credential = WIFI_STORE.getCredentialAt(index);
    if (!credential || credential->ssid == lastSsid) continue;
    if (tryCredential(*credential, OTHER_WIFI_TIMEOUT_MS)) return true;
  }

  return false;
}

bool ensureDownloadFolder() {
  const char* folder = SETTINGS.opdsDownloadFolder;
  if (folder[0] == '\0' || Storage.exists(folder)) return true;
  if (Storage.mkdir(folder)) return true;

  LOG_ERR("AODS", "Cannot create configured OPDS folder: %s", folder);
  return false;
}

std::string destinationPath(const OpdsEntry& book) {
  std::string path;
  path.reserve(112);

  const char* folder = SETTINGS.opdsDownloadFolder;
  if (folder[0] == '\0') {
    path = "/";
  } else {
    path = folder;
    if (path.back() != '/') path += '/';
  }

  path += opdsBookFilename(book.author, book.title,
                           static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  return path;
}

bool syncServer(const OpdsServer& server) {
  if (server.url.empty()) {
    LOG_ERR("AODS", "AUTO server has no URL: %s", server.name.c_str());
    return false;
  }
  if (!ensureDownloadFolder()) return false;

  const std::string rootFeedUrl = UrlUtils::buildUrl(server.url, "");
  LOG_INF("AODS", "Fetching root OPDS feed: %s", server.name.c_str());

  OpdsParser parser;
  {
    OpdsParserStream stream(parser);
    if (!HttpDownloader::fetchUrl(rootFeedUrl, stream, server.username, server.password)) {
      LOG_ERR("AODS", "Failed to fetch root OPDS feed: %s", server.name.c_str());
      return false;
    }
  }

  if (!parser) {
    LOG_ERR("AODS", "Failed to parse root OPDS feed: %s", server.name.c_str());
    return false;
  }
  if (parser.truncated()) LOG_INF("AODS", "Root feed truncated to parser capacity: %s", server.name.c_str());

  // Moving the bounded parser vector keeps the existing parser allocation and
  // avoids a second copy. Only this root feed is inspected; navigation entries
  // are deliberately ignored and never fetched recursively.
  auto entries = std::move(parser).getEntries();
  size_t downloaded = 0;
  size_t bookEntries = 0;
  bool serverOk = true;

  for (const auto& entry : entries) {
    if (entry.type != OpdsEntryType::BOOK) continue;
    ++bookEntries;

    const std::string destination = destinationPath(entry);
    if (Storage.exists(destination.c_str())) {
      LOG_DBG("AODS", "Already present: %s", destination.c_str());
      continue;
    }

    const std::string downloadUrl = UrlUtils::buildUrl(rootFeedUrl, entry.href);
    LOG_INF("AODS", "Downloading EPUB: %s", entry.title.c_str());
    const auto result = HttpDownloader::downloadToFile(downloadUrl, destination, nullptr, nullptr, server.username,
                                                       server.password);
    if (result != HttpDownloader::OK) {
      LOG_ERR("AODS", "EPUB download failed (%d): %s", static_cast<int>(result), entry.title.c_str());
      serverOk = false;
      continue;
    }

    // The downloader rejects empty responses and removes failed partial files.
    // The existence check above ensures an unattended run never intentionally
    // replaces a pre-existing EPUB.
    clearBookCache(destination);
    ++downloaded;
    LOG_INF("AODS", "Downloaded EPUB: %s", destination.c_str());
    if (downloaded >= MAX_DOWNLOADS_PER_SERVER) break;
  }

  if (bookEntries == 0) LOG_INF("AODS", "No EPUB entries in root feed: %s", server.name.c_str());
  LOG_INF("AODS", "Server done: %s, new=%u", server.name.c_str(), static_cast<unsigned>(downloaded));
  return serverOk;
}

void stopWifi() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

}  // namespace

bool isTimerWake() {
  return gpio.deviceIsX3() && esp_reset_reason() == ESP_RST_DEEPSLEEP &&
         esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

bool hasEnabledServers() {
  if (!gpio.deviceIsX3()) return false;
  const auto& servers = OPDS_STORE.getServers();
  return std::any_of(servers.begin(), servers.end(), [](const OpdsServer& server) { return isAutoServer(server); });
}

bool armNextWake() {
  if (!gpio.deviceIsX3() || !hasEnabledServers()) return false;
  if (!halClock.isAvailable()) {
    LOG_ERR("AODS", "Timer not armed: X3 RTC is unavailable");
    return false;
  }

  uint8_t utcHour = 0;
  uint8_t utcMinute = 0;
  uint8_t utcSecond = 0;
  if (!halClock.getTimeWithSeconds(utcHour, utcMinute, utcSecond)) {
    LOG_ERR("AODS", "Timer not armed: X3 RTC time is unavailable");
    return false;
  }

  const int secondsUntilWake = secondsUntilNextLocal0400(utcHour, utcMinute, utcSecond, SETTINGS.clockUtcOffsetQ);
  if (secondsUntilWake <= 0) {
    LOG_ERR("AODS", "Timer not armed: invalid RTC time");
    return false;
  }

  const uint64_t wakeUs = static_cast<uint64_t>(secondsUntilWake) * MICROSECONDS_PER_SECOND;
  const esp_err_t error = esp_sleep_enable_timer_wakeup(wakeUs);
  if (error != ESP_OK) {
    LOG_ERR("AODS", "Timer wake setup failed: %d", static_cast<int>(error));
    return false;
  }

  LOG_INF("AODS", "Next auto-sync wake in %lu seconds at 04:00 local",
          static_cast<unsigned long>(secondsUntilWake));
  return true;
}

bool run() {
  if (!gpio.deviceIsX3()) return false;
  if (!hasEnabledServers()) {
    LOG_INF("AODS", "Timer wake but no AUTO: OPDS servers are enabled");
    stopWifi();
    return false;
  }

  LOG_INF("AODS", "Scheduled OPDS sync starting");
  if (!connectSavedWifi()) {
    LOG_ERR("AODS", "Scheduled sync skipped: no saved WiFi could connect");
    stopWifi();
    return false;
  }

  // NTP is best-effort here. A successful sync corrects the RTC before the
  // next timer is armed; a failed sync leaves the last valid RTC value in use.
  if (halClock.isAvailable()) {
    const bool ntpOk = halClock.syncFromNTP();
    if (ntpOk) {
      if (!SETTINGS.clockHasBeenSynced) {
        SETTINGS.clockHasBeenSynced = 1;
        if (!SETTINGS.saveToFile()) LOG_ERR("AODS", "Failed to persist NTP-sync status");
      }
    } else {
      LOG_ERR("AODS", "NTP sync failed; continuing with current RTC time");
    }
  }

  bool ok = true;
  for (const auto& server : OPDS_STORE.getServers()) {
    if (!isAutoServer(server)) continue;
    if (!syncServer(server)) ok = false;
  }

  stopWifi();
  LOG_INF("AODS", "Scheduled OPDS sync finished: %s", ok ? "ok" : "with errors");
  return ok;
}

}  // namespace AutoOpdsSync
