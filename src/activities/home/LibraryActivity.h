#pragma once

#include <string>
#include <vector>

#include "MagazineIssueStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class LibraryActivity final : public Activity {
 private:
  struct LibraryEntry {
    std::string path;
    std::string title;
  };

  struct MagazineSeriesEntry {
    std::string name;
    size_t issueCount = 0;
    std::string latestTitle;
  };

  enum class View { Root, Magazines, Issues };

  ButtonNavigator buttonNavigator;
  std::vector<LibraryEntry> books;
  std::vector<MagazineSeriesEntry> magazineSeries;
  std::vector<MagazineIssue> visibleIssues;
  size_t selectorIndex = 0;
  bool lockNextConfirmRelease = false;
  View view = View::Root;
  std::string selectedSeries;

  void scanLibrary();
  void scanDirectory(const std::string& path);
  void loadMagazineSeries();
  void loadIssuesForSeries(const std::string& series);
  bool isMagazinePath(const std::string& path) const;
  int currentItemCount() const;
  void openSelected();
  void goBack();

 public:
  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
