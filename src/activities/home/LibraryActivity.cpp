#include "LibraryActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t NAME_BUFFER_SIZE = 500;
constexpr size_t MAX_LIBRARY_BOOKS = 1000;

std::string fileTitleFromPath(const std::string& path) {
  const auto slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const auto dot = name.find_last_of('.');
  if (dot != std::string::npos) name.resize(dot);
  return name;
}

bool shouldSkipDirectory(const char* name) {
  return name[0] == '.' || strcmp(name, "System Volume Information") == 0;
}
}  // namespace

void LibraryActivity::scanLibrary() {
  books.clear();
  scanDirectory("/");
  std::sort(books.begin(), books.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
    return a.title < b.title;
  });
}

void LibraryActivity::scanDirectory(const std::string& path) {
  if (books.size() >= MAX_LIBRARY_BOOKS) return;

  auto dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) return;

  dir.rewindDirectory();
  char name[NAME_BUFFER_SIZE];

  for (auto entry = dir.openNextFile(); entry && books.size() < MAX_LIBRARY_BOOKS; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

    const bool isDir = entry.isDirectory();
    entry.close();

    if (isDir && shouldSkipDirectory(name)) continue;

    std::string fullPath = path;
    if (fullPath.empty() || fullPath.back() != '/') fullPath += '/';
    fullPath += name;

    if (isDir) {
      scanDirectory(fullPath);
    } else if (FsHelpers::hasEpubExtension(fullPath)) {
      books.push_back({fullPath, fileTitleFromPath(fullPath)});
    }
  }

  dir.close();
}

void LibraryActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  scanLibrary();
  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();
  books.clear();
}

void LibraryActivity::openSelected() {
  if (books.empty() || selectorIndex >= books.size()) return;
  activityManager.goToReader(books[selectorIndex].path);
}

void LibraryActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
    } else {
      openSelected();
    }
    return;
  }

  int touchSel = static_cast<int>(selectorIndex);
  const auto listTouch = handleListTouch(touchSel, static_cast<int>(books.size()), contentTop, contentHeight, true);
  if (listTouch != ListTouchResult::None) {
    selectorIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) openSelected();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const int listSize = static_cast<int>(books.size());
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Library");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (books.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No EPUB books found");
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, books.size(), selectorIndex,
                 [this](int index) { return books[index].title; },
                 [this](int index) { return books[index].path; },
                 [this](int) { return UITheme::getFileIcon("book.epub"); });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
