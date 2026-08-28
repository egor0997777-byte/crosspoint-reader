#include "LibraryActivity.h"

#include <Bitmap.h>
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
constexpr int GRID_COLUMNS = 2;
constexpr int GRID_ROWS = 2;
constexpr int GRID_BOOKS_PER_PAGE = GRID_COLUMNS * GRID_ROWS;

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

int readingProgressPercent(const std::string& path) {
  Epub epub(path, "/.crosspoint");
  if (!epub.load(true, true) || epub.getBookSize() == 0) return -1;

  HalFile file;
  if (!Storage.openFileForRead("LIB", epub.getCachePath() + "/progress.bin", file) || !file) return -1;

  uint8_t data[10] = {};
  const int dataSize = file.read(data, sizeof(data));
  file.close();
  if (dataSize != 4 && dataSize != 6 && dataSize != 10) return -1;

  const int spineIndex = data[0] + (data[1] << 8);
  const int page = data[2] + (data[3] << 8);
  const int pageCount = dataSize >= 6 ? data[4] + (data[5] << 8) : 0;
  float chapterProgress = 0.0f;
  if (pageCount > 1) {
    chapterProgress = std::clamp(static_cast<float>(page) / static_cast<float>(pageCount - 1), 0.0f, 1.0f);
  }

  const float bookProgress = epub.calculateProgress(spineIndex, chapterProgress) * 100.0f;
  return std::clamp(static_cast<int>(bookProgress + 0.5f), 0, 100);
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
      LibraryEntry book;
      book.path = fullPath;
      book.title = fileTitleFromPath(fullPath);

      // Reuse CrossPoint's existing EPUB metadata cache. The first scan may
      // build it; later opens reuse it instead of parsing the package again.
      Epub epub(fullPath, "/.crosspoint");
      if (epub.load(true, true)) {
        if (!epub.getTitle().empty()) book.title = epub.getTitle();
        book.author = epub.getAuthor();
      }
      books.push_back(std::move(book));
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
      series.newCount = issue.isNew ? 1 : 0;
      series.latestTitle = issue.title.empty() ? fileTitleFromPath(issue.path) : issue.title;
      magazineSeries.push_back(std::move(series));
    } else {
      ++it->issueCount;
      if (issue.isNew) ++it->newCount;
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

size_t LibraryActivity::regularBookCount() const {
  size_t count = 0;
  for (const auto& book : books) {
    if (!isMagazinePath(book.path)) ++count;
  }
  return count;
}

LibraryActivity::LibraryEntry* LibraryActivity::regularBookAt(size_t index) {
  for (auto& book : books) {
    if (isMagazinePath(book.path)) continue;
    if (index == 0) return &book;
    --index;
  }
  return nullptr;
}

const LibraryActivity::LibraryEntry* LibraryActivity::regularBookAt(size_t index) const {
  for (const auto& book : books) {
    if (isMagazinePath(book.path)) continue;
    if (index == 0) return &book;
    --index;
  }
  return nullptr;
}

int LibraryActivity::rootPrefixCount() const {
  int count = 0;
  const auto& recent = RECENT_BOOKS.getBooks();
  if (!recent.empty() && !RecentBooksStore::isMissing(recent.front())) ++count;
  if (!magazineSeries.empty()) ++count;
  return count;
}

int LibraryActivity::currentItemCount() const {
  if (view == View::Magazines) return static_cast<int>(magazineSeries.size());
  if (view == View::Issues) return static_cast<int>(visibleIssues.size());
  return rootPrefixCount() + static_cast<int>(regularBookCount());
}

bool LibraryActivity::ensureThumb(LibraryEntry& book, const int coverHeight) {
  if (!book.thumbPath.empty() && Storage.exists(book.thumbPath.c_str())) return true;
  if (book.coverAttempted) return false;
  book.coverAttempted = true;

  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(false, true)) return false;
  book.thumbPath = epub.getThumbBmpPath(coverHeight);
  if (!Storage.exists(book.thumbPath.c_str()) && !epub.generateThumbBmp(coverHeight)) {
    book.thumbPath.clear();
    return false;
  }
  return Storage.exists(book.thumbPath.c_str());
}

void LibraryActivity::drawBookCard(LibraryEntry& book, const int x, const int y, const int width, const int height,
                                   const bool selected) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int border = selected ? 3 : 1;
  const int padding = 7;
  renderer.drawRoundedRect(x, y, width, height, border, 7, true);

  const int titleHeight = 52;
  const int coverAreaHeight = std::max(40, height - titleHeight - padding * 2);
  const int coverHeight = std::max(36, coverAreaHeight - 4);
  const int coverMaxWidth = std::max(36, width - padding * 2 - 8);
  bool coverDrawn = false;

  if (ensureThumb(book, coverHeight)) {
    HalFile file = Storage.open(book.thumbPath.c_str());
    if (file) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const int naturalW = std::max(1, bitmap.getWidth());
        const int naturalH = std::max(1, bitmap.getHeight());
        int drawW = coverMaxWidth;
        int drawH = (naturalH * drawW) / naturalW;
        if (drawH > coverAreaHeight) {
          drawH = coverAreaHeight;
          drawW = (naturalW * drawH) / naturalH;
        }
        const int coverX = x + (width - drawW) / 2;
        renderer.drawBitmap(bitmap, coverX, y + padding, drawW, drawH);
        coverDrawn = true;
      }
      file.close();
    }
  }

  if (!coverDrawn) {
    const int fallbackW = std::min(coverMaxWidth, 94);
    const int fallbackH = std::min(coverAreaHeight, 128);
    const int fallbackX = x + (width - fallbackW) / 2;
    const int fallbackY = y + padding;
    renderer.drawRect(fallbackX, fallbackY, fallbackW, fallbackH, 2, true);
    UITheme::drawCenteredWrappedText(renderer, Rect{fallbackX + 5, fallbackY + 5, fallbackW - 10, fallbackH - 10},
                                     SMALL_FONT_ID, book.title.c_str(), 4, true);
  }

  const int titleY = y + height - titleHeight;
  UITheme::drawCenteredWrappedText(renderer, Rect{x + metrics.verticalSpacing, titleY, width - metrics.verticalSpacing * 2,
                                                  titleHeight},
                                   SMALL_FONT_ID, book.title.c_str(), 2, true,
                                   EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::CENTER);
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
    const std::string path = visibleIssues[selectorIndex].path;
    if (MAGAZINE_ISSUES.markRead(path)) {
      visibleIssues[selectorIndex].isNew = false;
      loadMagazineSeries();
    }
    activityManager.goToReader(path);
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

  if (auto* book = regularBookAt(index)) activityManager.goToReader(book->path);
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

  if (view != View::Root) {
    int touchSel = static_cast<int>(selectorIndex);
    const auto listTouch = handleListTouch(touchSel, listSize, contentTop, contentHeight, true);
    if (listTouch != ListTouchResult::None) {
      selectorIndex = static_cast<size_t>(touchSel);
      if (listTouch == ListTouchResult::Activated) openSelected();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goBack();
    return;
  }

  if (listSize <= 0) return;

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = view == View::Root
                        ? ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize)
                        : ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = view == View::Root
                        ? ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize)
                        : ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
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
    selectorIndex = view == View::Root
                        ? ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize)
                        : ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = view == View::Root
                        ? ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize)
                        : ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
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
                     std::string subtitle = std::to_string(magazineSeries[index].issueCount) + " issues";
                     if (magazineSeries[index].newCount > 0) {
                       subtitle += " - " + std::to_string(magazineSeries[index].newCount) + " new";
                     }
                     subtitle += " - latest: " + magazineSeries[index].latestTitle;
                     return subtitle;
                   },
                   [this](int) { return UITheme::getFileIcon("magazine.epub"); });
    }
  } else if (view == View::Issues) {
    if (visibleIssues.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No issues found");
    } else {
      GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, visibleIssues.size(), selectorIndex,
                   [this](int index) {
                     std::string title = visibleIssues[index].title.empty() ? fileTitleFromPath(visibleIssues[index].path)
                                                                          : visibleIssues[index].title;
                     if (visibleIssues[index].isNew) title = "NEW - " + title;
                     return title;
                   },
                   [this](int index) { return visibleIssues[index].author; },
                   [this](int) { return UITheme::getFileIcon("magazine.epub"); });
    }
  } else {
    const auto& recent = RECENT_BOOKS.getBooks();
    const bool hasContinue = !recent.empty() && !RecentBooksStore::isMissing(recent.front());
    const bool hasMagazines = !magazineSeries.empty();
    const size_t regularCount = regularBookCount();

    int y = contentTop;
    int globalOffset = 0;

    if (hasContinue) {
      constexpr int continueHeight = 64;
      const bool selected = selectorIndex == 0;
      if (selected) renderer.drawRoundedRect(metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2,
                                             continueHeight, 3, 7, true);
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + 10, y + 8, "Continue reading", true,
                        EpdFontFamily::BOLD);
      const std::string title = recent.front().title.empty() ? fileTitleFromPath(recent.front().path) : recent.front().title;
      std::string subtitle = title;
      const int progress = readingProgressPercent(recent.front().path);
      if (progress >= 0) subtitle += " - " + std::to_string(progress) + "%";
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 10, y + 34, subtitle.c_str());
      y += continueHeight + metrics.verticalSpacing;
      ++globalOffset;
    }

    if (hasMagazines) {
      constexpr int magazineHeight = 54;
      const bool selected = selectorIndex == static_cast<size_t>(globalOffset);
      if (selected) renderer.drawRoundedRect(metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2,
                                             magazineHeight, 3, 7, true);
      size_t issueCount = 0;
      size_t newCount = 0;
      for (const auto& series : magazineSeries) {
        issueCount += series.issueCount;
        newCount += series.newCount;
      }
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + 10, y + 7, "Magazines", true,
                        EpdFontFamily::BOLD);
      std::string subtitle = std::to_string(magazineSeries.size()) + " titles - " + std::to_string(issueCount) + " issues";
      if (newCount > 0) subtitle += " - " + std::to_string(newCount) + " new";
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + 10, y + 31, subtitle.c_str());
      y += magazineHeight + metrics.verticalSpacing;
      ++globalOffset;
    }

    if (regularCount == 0) {
      if (!hasContinue && !hasMagazines)
        renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 20, "No EPUB books found");
    } else {
      const size_t selectedRegular = selectorIndex >= static_cast<size_t>(globalOffset)
                                         ? selectorIndex - static_cast<size_t>(globalOffset)
                                         : 0;
      const size_t pageStart = (selectedRegular / GRID_BOOKS_PER_PAGE) * GRID_BOOKS_PER_PAGE;
      const int gridTop = y;
      const int gridHeight = pageHeight - gridTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      const int gap = 10;
      const int cellWidth = (pageWidth - metrics.contentSidePadding * 2 - gap) / GRID_COLUMNS;
      const int cellHeight = std::max(140, (gridHeight - gap) / GRID_ROWS);

      for (int cell = 0; cell < GRID_BOOKS_PER_PAGE; ++cell) {
        const size_t bookIndex = pageStart + static_cast<size_t>(cell);
        if (bookIndex >= regularCount) break;
        auto* book = regularBookAt(bookIndex);
        if (!book) continue;
        const int col = cell % GRID_COLUMNS;
        const int row = cell / GRID_COLUMNS;
        const int x = metrics.contentSidePadding + col * (cellWidth + gap);
        const int cardY = gridTop + row * (cellHeight + gap);
        const bool selected = selectorIndex == static_cast<size_t>(globalOffset) + bookIndex;
        drawBookCard(*book, x, cardY, cellWidth, cellHeight, selected);
      }
    }
  }

  const auto labels = mappedInput.mapLabels(view == View::Root ? tr(STR_HOME) : "Back", tr(STR_OPEN),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
