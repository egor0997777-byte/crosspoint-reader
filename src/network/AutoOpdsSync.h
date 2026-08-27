#pragma once

namespace AutoOpdsSync {

// True only for an X3 deep-sleep reset caused by the ESP32 timer wake source.
bool isTimerWake();

// True when the loaded OPDS store contains at least one server named with the
// exact AUTO: prefix. The prefix is intentionally case-sensitive.
bool hasEnabledServers();

// Arm the next local 04:00 timer wake. Returns false when this X3 has no
// enabled AUTO: servers or the RTC cannot provide a valid current time.
bool armNextWake();

// Connect through the existing saved Wi-Fi credentials and download missing
// EPUB entries directly present in each AUTO: server's root feed.
bool run();

}  // namespace AutoOpdsSync
