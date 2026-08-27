#include "MagazineIssueStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

void MagazineIssueStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["issues"].to<JsonArray>();
  for (const auto& issue : issues) {
    JsonObject obj = arr.add<JsonObject>();
    obj["series"] = issue.series;
    obj["path"] = issue.path;
    obj["title"] = issue.title;
    obj["author"] = issue.author;
  }
}

bool MagazineIssueStore::fromJson(JsonVariantConst doc) {
  issues.clear();
  JsonArrayConst arr = doc["issues"].as<JsonArrayConst>();
  issues.reserve(std::min(arr.size(), MAX_ISSUES));

  for (JsonObjectConst obj : arr) {
    if (issues.size() >= MAX_ISSUES) break;
    MagazineIssue issue;
    issue.series = obj["series"] | "";
    issue.path = obj["path"] | "";
    issue.title = obj["title"] | "";
    issue.author = obj["author"] | "";
    if (issue.series.empty() || issue.path.empty()) continue;
    issues.push_back(std::move(issue));
  }

  LOG_DBG("MAG", "Loaded %zu magazine issues", issues.size());
  return true;
}

bool MagazineIssueStore::recordIssue(const std::string& series, const std::string& path, const std::string& title,
                                     const std::string& author) {
  return recordIssues({MagazineIssue{series, path, title, author}});
}

bool MagazineIssueStore::recordIssues(const std::vector<MagazineIssue>& feedOrder) {
  if (feedOrder.empty()) return true;

  std::vector<MagazineIssue> incoming;
  incoming.reserve(feedOrder.size());
  for (const auto& issue : feedOrder) {
    if (issue.series.empty() || issue.path.empty()) continue;
    const bool duplicate = std::any_of(incoming.begin(), incoming.end(), [&issue](const MagazineIssue& existing) {
      return existing.path == issue.path;
    });
    if (!duplicate) incoming.push_back(issue);
  }
  if (incoming.empty()) return false;

  issues.erase(std::remove_if(issues.begin(), issues.end(), [&incoming](const MagazineIssue& existing) {
                 return std::any_of(incoming.begin(), incoming.end(), [&existing](const MagazineIssue& fresh) {
                   return fresh.path == existing.path;
                 });
               }),
               issues.end());

  issues.insert(issues.begin(), incoming.begin(), incoming.end());
  if (issues.size() > MAX_ISSUES) issues.resize(MAX_ISSUES);
  return saveToFile();
}

bool MagazineIssueStore::pruneMissing() {
  const auto oldSize = issues.size();
  issues.erase(std::remove_if(issues.begin(), issues.end(), [](const MagazineIssue& issue) {
                 return !Storage.exists(issue.path.c_str());
               }),
               issues.end());
  return issues.size() != oldSize;
}
