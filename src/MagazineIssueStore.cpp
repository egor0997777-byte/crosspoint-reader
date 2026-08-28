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
    obj["new"] = issue.isNew;
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
    issue.isNew = obj["new"] | false;
    if (issue.series.empty() || issue.path.empty()) continue;
    issues.push_back(std::move(issue));
  }

  LOG_DBG("MAG", "Loaded %zu magazine issues", issues.size());
  return true;
}

bool MagazineIssueStore::recordIssue(const std::string& series, const std::string& path, const std::string& title,
                                     const std::string& author) {
  return recordIssues({MagazineIssue{series, path, title, author, false}});
}

bool MagazineIssueStore::recordIssues(const std::vector<MagazineIssue>& feedOrder) {
  if (feedOrder.empty()) return true;

  std::vector<MagazineIssue> incoming;
  incoming.reserve(feedOrder.size());
  for (const auto& candidate : feedOrder) {
    if (candidate.series.empty() || candidate.path.empty()) continue;
    const bool duplicate = std::any_of(incoming.begin(), incoming.end(), [&candidate](const MagazineIssue& existing) {
      return existing.path == candidate.path;
    });
    if (duplicate) continue;

    MagazineIssue issue = candidate;
    const auto existing = std::find_if(issues.begin(), issues.end(), [&issue](const MagazineIssue& oldIssue) {
      return oldIssue.path == issue.path;
    });
    if (existing == issues.end()) {
      issue.isNew = true;
    } else {
      issue.isNew = issue.isNew || existing->isNew;
    }
    incoming.push_back(std::move(issue));
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

bool MagazineIssueStore::markRead(const std::string& path) {
  auto it = std::find_if(issues.begin(), issues.end(), [&path](const MagazineIssue& issue) { return issue.path == path; });
  if (it == issues.end() || !it->isNew) return true;
  it->isNew = false;
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
