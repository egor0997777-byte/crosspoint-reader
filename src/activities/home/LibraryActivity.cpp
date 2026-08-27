#include "LibraryActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
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

LibraryActivity::LibraryEntry buildLibraryEntry(const std::string& path) {
  LibraryActivity::LibraryEntry result{path, fileTitleFromPath(path), ""};

  // Reuse CrossPoint's existing EPUB metadata cache. The first scan may need
  // to build cache data; later Library opens should reuse it instead of parsing
  // the EPUB package from scratch.
  Epub epub(path, "/.crosspoint");
  if (epub.load(true, true)) {
    if (!epub.getTitle().empty()) result.title = epub.getTitle();
    result.author = epub.getAuthor();
  }

  return result;
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
      books.push_back(buildLibraryEntry(fullPath));
    }
  }

  dir.close();
}

void LibraryActivity::loadMagazineSeries() {
  magazineSeries.clear();

  for (const auto& issue : MAGAZINE_ISSUES.getIssues()) {
    auto it = std::find_if(magazineSeries.begin(), magazineSeries.end(), [&issue](const MagazineSeriesEntry& entry) {
      return entry.name == issue.series;
    });
    if (it == magazineSeries.end()) {
      MagazineSeriesEntry series;
      series.name = issue.series;
      series.issueCount = 1;
      series.latestTitle = issue.title.empty() ? fileTitleFromPath(issue.path) : issue.title;
      magazineSeries.push_back(std::move(series));
    } else {
      ++it->issueCount;
    }
  }

  std::sort(magazineSeries.begin(), magazineSeries.end(), [](const MagazineSeriesEntry& a, const MagazineSeriesEntry& b) {
    return a.name < b.name;
  });
}

void LibraryActivity::loadIssuesForSeries(const std::string& series) {
  visibleIssues.clear();
  for (const auto& issue : MAGAZINE_ISSUES.getIssues()) {
    if (issue.series == series && Storage.exists(issue.path.c_str())) visibleIssues.push_back(issue);
  }
}

bool LibraryActivity::isMagazinePath(const std::string& path) const {
  const auto& issues = MAGAZINE_ISSUES.getIssues();
  return std::any_of(issues.begin(), issues.end(), [&path](const MagazineIssue& issue) { return issue.path == path; });
}

int LibraryActivity::currentItemCount() const {
  if (view == View::Magazines) return static_cast<int>(magazineSeries.size());
  if (view == View::Issues) return static_cast<int>(visibleIssues.size());

  int count = 0;
  const auto& recent = RECENT_BOOKS.getBooks();
  if (!recent.empty() && !RecentBooksStore::isMissing(recent.front())) ++count;
  if (!magazineSeries.empty()) ++count;
  for (const auto& book : books) {
    if (!isMagazinePath(book.path)) ++count;
  }
  return count;
}

void LibraryActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  view = View::Root;
  selectedSeries.clear();
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  MAGAZINE_ISSUES.loadFromFile();
  if (MAGAZINE_ISSUES.pruneMissing()) MAGAZINE_ISSUES.saveToFile();

  scanLibrary();
  loadMagazineSeries();
  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();
  books.clear();
  magazineSeries.clear();
  visibleIssues.clear();
}

void LibraryActivity::openSelected() {
  if (view == View::Magazines) {
    if (selectorIndex >= magazineSeries.size()) return;
    selectedSeries = magazineSeries[selectorIndex].name;
    loadIssuesForSeries(selectedSeries);
    selectorIndex = 0;
    view = View::Issues;
    requestUpdate();
    return;
  }

  if (view == View::Issues) {
    if (selectorIndex >= visibleIssues.size()) return;
    activityManager.goToReader(visibleIssues[selectorIndex].path);
    return;
  }

  size_t index = selectorIndex;
  const auto& recent = RECENT_BOOKS.getBooks();
  const bool hasContinue = !recent.empty() && !RecentBooksStore::isMissing(recent.front());
  if (hasContinue) {
    if (index == 0) {
      activityManager.goToReader(recent.front().path);
      return;
    }
    --index;
  }

  if (!magazineSeries.empty()) {
    if (index == 0) {
      selectorIndex = 0;
      view = View::Magazines;
      requestUpdate();
      return;
    }
    --index;
  }

  for (const auto& book : books) {
    if (isMagazinePath(book.path)) continue;
    if (index == 0) {
      activityManager.goToReader(book.path);
      return;
    }
    --index;
  }
}

void LibraryActivity::goBack() {
  if (view == View::Issues) {
    view = View::Magazines;
    selectorIndex = 0;
    visibleIssues.clear();
    requestUpdate();
  } else if (view == View::Magazines) {
    view = View::Root;
    selectorIndex = 0;
    selectedSeries.clear();
    requestUpdate();
  } else {
    onGoHome();
  }
}

void LibraryActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);
  const int listSize = currentItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
    } else {
      openSelected();
    }
    return;
  }

  int touchSel = static_cast<int>(selectorIndex);
  const auto listTouch = handleListTouch(touchSel, listSize, contentTop, contentHeight, true);
  if (listTouch != ListTouchResult::None) {
    selectorIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) openSelected();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }

  if (listSize <= 0) return;

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

  const char* heading = "Library";
  if (view == View::Magazines) heading = "Magazines";
  if (view == View::Issues) heading = selectedSeries.c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, heading);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (view == View::Magazines) {
    if (magazineSeries.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No magazine issues yet");
    } else {
      GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, magazineSeries.size(), selectorIndex,
                   [this](int index) { return magazineSeries[index].name; },
                   [this](int index) {
                     return std::to_string(magazineSeries[index].issueCount) + " issues - latest: " +
                            magazineSeries[index].latestTitle;
                   },
                   [this](int) { return UITheme::getFileIcon("magazine.epub"); });
    }
  } else if (view == View::Issues) {
    if (visibleIssues.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No issues found");
    } else {
      GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, visibleIssues.size(), selectorIndex,
                   [this](int index) {
                     return visibleIssues[index].title.empty() ? fileTitleFromPath(visibleIssues[index].path)
                                                               : visibleIssues[index].title;
                   },
                   [this](int index) { return visibleIssues[index].author; },
                   [this](int) { return UITheme::getFileIcon("magazine.epub"); });
    }
  } else {
    const auto& recent = RECENT_BOOKS.getBooks();
    const bool hasContinue = !recent.empty() && !RecentBooksStore::isMissing(recent.front());
    const int itemCount = currentItemCount();

    if (itemCount == 0) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No EPUB books found");
    } else {
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectorIndex,
          [this, hasContinue, &recent](int rawIndex) {
            size_t index = static_cast<size_t>(rawIndex);
            if (hasContinue) {
              if (index == 0) return recent.front().title.empty() ? fileTitleFromPath(recent.front().path)
                                                                  : recent.front().title;
              --index;
            }
            if (!magazineSeries.empty()) {
              if (index == 0) return std::string("Magazines");
              --index;
            }
            for (const auto& book : books) {
              if (isMagazinePath(book.path)) continue;
              if (index == 0) return book.title;
              --index;
            }
            return std::string();
          },
          [this, hasContinue, &recent](int rawIndex) {
            size_t index = static_cast<size_t>(rawIndex);
            if (hasContinue) {
              if (index == 0) {
                const std::string author = recent.front().author;
                return author.empty() ? std::string("Continue reading") : std::string("Continue reading - ") + author;
              }
              --index;
            }
            if (!magazineSeries.empty()) {
              if (index == 0) {
                size_t issueCount = 0;
                for (const auto& series : magazineSeries) issueCount += series.issueCount;
                return std::to_string(magazineSeries.size()) + " titles - " + std::to_string(issueCount) + " issues";
              }
              --index;
            }
            for (const auto& book : books) {
              if (isMagazinePath(book.path)) continue;
              if (index == 0) return book.author.empty() ? book.path : book.author;
              --index;
            }
            return std::string();
          },
          [this, hasContinue](int rawIndex) {
            size_t index = static_cast<size_t>(rawIndex);
            if (hasContinue) {
              if (index == 0) return UITheme::getFileIcon("book.epub");
              --index;
            }
            if (!magazineSeries.empty() && index == 0) return UITheme::getFileIcon("magazine.epub");
            return UITheme::getFileIcon("book.epub");
          });
    }
  }

  const auto labels = mappedInput.mapLabels(view == View::Root ? tr(STR_HOME) : "Back", tr(STR_OPEN),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
