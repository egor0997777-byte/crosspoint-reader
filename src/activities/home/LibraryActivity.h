#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class LibraryActivity final : public Activity {
 private:
  struct LibraryEntry {
    std::string path;
    std::string title;
  };

  ButtonNavigator buttonNavigator;
  std::vector<LibraryEntry> books;
  size_t selectorIndex = 0;
  bool lockNextConfirmRelease = false;

  void scanLibrary();
  void scanDirectory(const std::string& path);
  void openSelected();

 public:
  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
