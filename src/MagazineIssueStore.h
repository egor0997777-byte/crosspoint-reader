#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct MagazineIssue {
  std::string series;
  std::string path;
  std::string title;
  std::string author;
};

class MagazineIssueStore : public PersistableStore<MagazineIssueStore> {
 private:
  std::vector<MagazineIssue> issues;
  static constexpr size_t MAX_ISSUES = 256;

  MagazineIssueStore() = default;
  friend class PersistableStore<MagazineIssueStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/magazines.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool recordIssue(const std::string& series, const std::string& path, const std::string& title,
                   const std::string& author);
  bool pruneMissing();

  const std::vector<MagazineIssue>& getIssues() const { return issues; }
};

#define MAGAZINE_ISSUES MagazineIssueStore::getInstance()
