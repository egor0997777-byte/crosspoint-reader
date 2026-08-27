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
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "MagazineIssueStore.h"
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
constexpr char DIAGNOSTIC_PATH[] = "/.crosspoint/auto_opds_sync.log";
constexpr size_t DIAGNOSTIC_MAX_BYTES = 32 * 1024;

void rotateDiagnosticIfNeeded() {
  if (!Storage.exists(DIAGNOSTIC_PATH)) return;
  HalFile file;
  if (!Storage.openFileForRead("AODS", DIAGNOSTIC_PATH, file) || !file) return;
  const size_t size = file.fileSize();
  file.close();
  if (size >= DIAGNOSTIC_MAX_BYTES) Storage.remove(DIAGNOSTIC_PATH);
}

void diagnostic(const char* format, ...) {
  if (!Storage.ready()) return;
  Storage.ensureDirectoryExists("/.crosspoint");
  rotateDiagnosticIfNeeded();

  char message[224];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  char prefix[64];
  uint8_t utcHour = 0;
  uint8_t utcMinute = 0;
  uint8_t utcSecond = 0;
  if (halClock.isAvailable() && halClock.getTimeWithSeconds(utcHour, utcMinute, utcSecond)) {
    const int offsetMinutes = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
    int localSeconds = static_cast<int>(utcHour) * 3600 + static_cast<int>(utcMinute) * 60 + utcSecond +
                       offsetMinutes * 60;
    localSeconds %= 86400;
    if (localSeconds < 0) localSeconds += 86400;
    const int localHour = localSeconds / 3600;
    const int localMinute = (localSeconds % 3600) / 60;
    const int localSecond = localSeconds % 60;
    snprintf(prefix, sizeof(prefix), "UTC %02u:%02u:%02u | local %02d:%02d:%02d | ", utcHour, utcMinute,
             utcSecond, localHour, localMinute, localSecond);
  } else {
    snprintf(prefix, sizeof(prefix), "RTC unavailable | ");
  }

  HalFile file = Storage.open(DIAGNOSTIC_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return;
  file.write(prefix, strlen(prefix));
  file.write(message, strlen(message));
  static constexpr char newline = '\n';
  file.write(&newline, 1);
  file.flush();
  file.close();
}

bool isAutoServer(const OpdsServer& server) { return isAutoServerName(server.name); }

std::string magazineSeriesName(const OpdsServer& server) {
  std::string name = server.name;
  if (name.rfind("AUTO:", 0) == 0) name.erase(0, 5);

  const auto first = name.find_first_not_of(" \t");
  if (first == std::string::npos) return server.name;
  const auto last = name.find_last_not_of(" \t");
  return name.substr(first, last - first + 1);
}

bool tryCredential(const WifiCredential& credential, const unsigned long timeoutMs) {
  LOG_INF("AODS", "Trying saved WiFi: %s", credential.ssid.c_str());
  diagnostic("wifi try ssid=%s timeout_ms=%lu", credential.ssid.c_str(), timeoutMs);

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
      diagnostic("wifi connected ssid=%s ip=%s rssi=%d", credential.ssid.c_str(), WiFi.localIP().toString().c_str(),
                 WiFi.RSSI());
      WIFI_STORE.setLastConnectedSsid(credential.ssid);
      return true;
    }
    if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
      diagnostic("wifi terminal failure ssid=%s status=%d", credential.ssid.c_str(), static_cast<int>(status));
      break;
    }
    delay(100);
  }

  LOG_ERR("AODS", "WiFi connection failed: %s", credential.ssid.c_str());
  diagnostic("wifi failed ssid=%s final_status=%d", credential.ssid.c_str(), static_cast<int>(WiFi.status()));
  WiFi.disconnect();
  return false;
}

bool connectSavedWifi() {
  if (!WIFI_STORE.loadFromFile()) {
    LOG_ERR("AODS", "Could not load saved WiFi credentials");
    diagnostic("wifi store load failed");
    return false;
  }

  const size_t credentialCount = WIFI_STORE.getCredentialCount();
  diagnostic("wifi credentials=%u last=%s", static_cast<unsigned>(credentialCount),
             WIFI_STORE.getLastConnectedSsid().c_str());
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
  diagnostic("download folder create failed path=%s", folder);
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

bool indexMagazineBatch(const std::vector<MagazineIssue>& issues) {
  if (issues.empty()) return true;
  if (MAGAZINE_ISSUES.recordIssues(issues)) {
    diagnostic("magazine batch indexed count=%u series=%s", static_cast<unsigned>(issues.size()),
               issues.front().series.c_str());
    return true;
  }
  LOG_ERR("AODS", "Failed to update magazine index batch");
  diagnostic("magazine batch index failed count=%u", static_cast<unsigned>(issues.size()));
  return false;
}

bool syncServer(const OpdsServer& server) {
  if (server.url.empty()) {
    LOG_ERR("AODS", "AUTO server has no URL: %s", server.name.c_str());
    diagnostic("server skipped name=%s reason=no_url", server.name.c_str());
    return false;
  }
  if (!ensureDownloadFolder()) return false;

  const std::string rootFeedUrl = UrlUtils::buildUrl(server.url, "");
  LOG_INF("AODS", "Fetching root OPDS feed: %s", server.name.c_str());
  diagnostic("server start name=%s url=%s", server.name.c_str(), rootFeedUrl.c_str());

  OpdsParser parser;
  {
    OpdsParserStream stream(parser);
    if (!HttpDownloader::fetchUrl(rootFeedUrl, stream, server.username, server.password)) {
      LOG_ERR("AODS", "Failed to fetch root OPDS feed: %s", server.name.c_str());
      diagnostic("server fetch failed name=%s", server.name.c_str());
      return false;
    }
  }

  if (!parser) {
    LOG_ERR("AODS", "Failed to parse root OPDS feed: %s", server.name.c_str());
    diagnostic("server parse failed name=%s", server.name.c_str());
    return false;
  }
  if (parser.truncated()) {
    LOG_INF("AODS", "Root feed truncated to parser capacity: %s", server.name.c_str());
    diagnostic("server feed truncated name=%s", server.name.c_str());
  }

  auto entries = std::move(parser).getEntries();
  size_t downloaded = 0;
  size_t bookEntries = 0;
  size_t navigationEntries = 0;
  size_t existingEntries = 0;
  bool serverOk = true;
  const std::string series = magazineSeriesName(server);
  std::vector<MagazineIssue> feedIssues;
  feedIssues.reserve(entries.size());

  for (const auto& entry : entries) {
    if (entry.type != OpdsEntryType::BOOK) {
      ++navigationEntries;
      continue;
    }
    ++bookEntries;

    const std::string destination = destinationPath(entry);
    if (Storage.exists(destination.c_str())) {
      ++existingEntries;
      LOG_DBG("AODS", "Already present: %s", destination.c_str());
      diagnostic("skip existing title=%s path=%s", entry.title.c_str(), destination.c_str());
      feedIssues.push_back(MagazineIssue{series, destination, entry.title, entry.author});
      continue;
    }

    const std::string downloadUrl = UrlUtils::buildUrl(rootFeedUrl, entry.href);
    LOG_INF("AODS", "Downloading EPUB: %s", entry.title.c_str());
    diagnostic("download start title=%s url=%s", entry.title.c_str(), downloadUrl.c_str());
    const auto result = HttpDownloader::downloadToFile(downloadUrl, destination, nullptr, nullptr, server.username,
                                                       server.password);
    if (result != HttpDownloader::OK) {
      LOG_ERR("AODS", "EPUB download failed (%d): %s", static_cast<int>(result), entry.title.c_str());
      diagnostic("download failed title=%s result=%d", entry.title.c_str(), static_cast<int>(result));
      serverOk = false;
      continue;
    }

    clearBookCache(destination);
    feedIssues.push_back(MagazineIssue{series, destination, entry.title, entry.author});
    ++downloaded;
    LOG_INF("AODS", "Downloaded EPUB: %s", destination.c_str());
    diagnostic("download ok title=%s path=%s", entry.title.c_str(), destination.c_str());
    if (downloaded >= MAX_DOWNLOADS_PER_SERVER) break;
  }

  if (!indexMagazineBatch(feedIssues)) serverOk = false;

  if (bookEntries == 0) LOG_INF("AODS", "No EPUB entries in root feed: %s", server.name.c_str());
  LOG_INF("AODS", "Server done: %s, new=%u", server.name.c_str(), static_cast<unsigned>(downloaded));
  diagnostic("server done name=%s books=%u navigation=%u existing=%u downloaded=%u indexed=%u ok=%d",
             server.name.c_str(), static_cast<unsigned>(bookEntries), static_cast<unsigned>(navigationEntries),
             static_cast<unsigned>(existingEntries), static_cast<unsigned>(downloaded),
             static_cast<unsigned>(feedIssues.size()), serverOk ? 1 : 0);
  return serverOk;
}

void stopWifi() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  diagnostic("wifi stopping");
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
    diagnostic("timer not armed reason=rtc_unavailable");
    return false;
  }

  uint8_t utcHour = 0;
  uint8_t utcMinute = 0;
  uint8_t utcSecond = 0;
  if (!halClock.getTimeWithSeconds(utcHour, utcMinute, utcSecond)) {
    LOG_ERR("AODS", "Timer not armed: X3 RTC time is unavailable");
    diagnostic("timer not armed reason=rtc_read_failed");
    return false;
  }

  const int secondsUntilWake = secondsUntilNextLocal0400(utcHour, utcMinute, utcSecond, SETTINGS.clockUtcOffsetQ);
  if (secondsUntilWake <= 0) {
    LOG_ERR("AODS", "Timer not armed: invalid RTC time");
    diagnostic("timer not armed reason=invalid_delay delay=%d", secondsUntilWake);
    return false;
  }

  const uint64_t wakeUs = static_cast<uint64_t>(secondsUntilWake) * MICROSECONDS_PER_SECOND;
  const esp_err_t error = esp_sleep_enable_timer_wakeup(wakeUs);
  if (error != ESP_OK) {
    LOG_ERR("AODS", "Timer wake setup failed: %d", static_cast<int>(error));
    diagnostic("timer arm failed esp_err=%d delay=%d", static_cast<int>(error), secondsUntilWake);
    return false;
  }

  LOG_INF("AODS", "Next auto-sync wake in %lu seconds at 04:00 local",
          static_cast<unsigned long>(secondsUntilWake));
  diagnostic("timer armed delay_seconds=%d utc_offset_q=%u", secondsUntilWake,
             static_cast<unsigned>(SETTINGS.clockUtcOffsetQ));
  return true;
}

bool run() {
  if (!gpio.deviceIsX3()) return false;
  diagnostic("timer wake detected reset_reason=%d wake_cause=%d auto_servers=%u", static_cast<int>(esp_reset_reason()),
             static_cast<int>(esp_sleep_get_wakeup_cause()),
             static_cast<unsigned>(std::count_if(OPDS_STORE.getServers().begin(), OPDS_STORE.getServers().end(),
                                                 [](const OpdsServer& server) { return isAutoServer(server); })));
  if (!hasEnabledServers()) {
    LOG_INF("AODS", "Timer wake but no AUTO: OPDS servers are enabled");
    diagnostic("sync aborted reason=no_auto_servers");
    stopWifi();
    return false;
  }

  LOG_INF("AODS", "Scheduled OPDS sync starting");
  diagnostic("sync start");
  if (!connectSavedWifi()) {
    LOG_ERR("AODS", "Scheduled sync skipped: no saved WiFi could connect");
    diagnostic("sync aborted reason=wifi_unavailable");
    stopWifi();
    return false;
  }

  if (halClock.isAvailable()) {
    const bool ntpOk = halClock.syncFromNTP();
    diagnostic("ntp result=%s", ntpOk ? "ok" : "failed");
    if (ntpOk) {
      if (!SETTINGS.clockHasBeenSynced) {
        SETTINGS.clockHasBeenSynced = 1;
        if (!SETTINGS.saveToFile()) {
          LOG_ERR("AODS", "Failed to persist NTP-sync status");
          diagnostic("settings save failed after ntp");
        }
      }
    } else {
      LOG_ERR("AODS", "NTP sync failed; continuing with current RTC time");
    }
  }

  MAGAZINE_ISSUES.loadFromFile();
  if (MAGAZINE_ISSUES.pruneMissing()) MAGAZINE_ISSUES.saveToFile();

  bool ok = true;
  for (const auto& server : OPDS_STORE.getServers()) {
    if (!isAutoServer(server)) continue;
    if (!syncServer(server)) ok = false;
  }

  diagnostic("sync finished result=%s", ok ? "ok" : "with_errors");
  stopWifi();
  LOG_INF("AODS", "Scheduled OPDS sync finished: %s", ok ? "ok" : "with errors");
  return ok;
}

}  // namespace AutoOpdsSync
