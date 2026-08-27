#include "ActivityManager.h"

#include <memory>

#include "home/LibraryActivity.h"

void ActivityManager::goToLibrary() {
  replaceActivity(std::make_unique<LibraryActivity>(renderer, mappedInput));
}
