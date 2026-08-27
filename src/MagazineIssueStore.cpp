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
  if (series.empty() || path.empty()) return false;

  auto it = std::find_if(issues.begin(), issues.end(), [&path](const MagazineIssue& issue) { return issue.path == path; });
  if (it != issues.end()) {
    it->series = series;
    it->title = title;
    it->author = author;
    MagazineIssue updated = std::move(*it);
    issues.erase(it);
    issues.insert(issues.begin(), std::move(updated));
  } else {
    issues.insert(issues.begin(), MagazineIssue{series, path, title, author});
    if (issues.size() > MAX_ISSUES) issues.resize(MAX_ISSUES);
  }

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
