#pragma once

#include <cstdint>
#include <string_view>

namespace AutoOpdsSync {

inline constexpr std::string_view AUTO_SERVER_PREFIX = "AUTO:";
inline constexpr int SECONDS_PER_DAY = 24 * 60 * 60;
inline constexpr int TARGET_LOCAL_SECONDS = 4 * 60 * 60;

// Return the number of seconds from a UTC RTC reading until the next 04:00
// after applying CrossPoint's biased quarter-hour UTC offset. A reading at
// exactly 04:00 is deliberately scheduled for the following day so a timer
// wake cannot immediately re-enter the same sync cycle.
constexpr int secondsUntilNextLocal0400(const uint8_t utcHour, const uint8_t utcMinute, const uint8_t utcSecond,
                                        const uint8_t utcOffsetQ) noexcept {
  if (utcHour > 23 || utcMinute > 59 || utcSecond > 59) return -1;

  // CrossPoint persists offsets in the inclusive range 0..104. Match the
  // existing status-bar formatter's defensive upper clamp for corrupted data.
  const uint8_t boundedOffsetQ = utcOffsetQ > 104 ? 104 : utcOffsetQ;
  const int offsetSeconds = (static_cast<int>(boundedOffsetQ) - 48) * 15 * 60;
  int localSeconds = static_cast<int>(utcHour) * 60 * 60 + static_cast<int>(utcMinute) * 60 + utcSecond +
                     offsetSeconds;
  localSeconds %= SECONDS_PER_DAY;
  if (localSeconds < 0) localSeconds += SECONDS_PER_DAY;

  int untilTarget = TARGET_LOCAL_SECONDS - localSeconds;
  if (untilTarget <= 0) untilTarget += SECONDS_PER_DAY;
  return untilTarget;
}

constexpr bool isAutoServerName(const std::string_view name) noexcept {
  return name.size() >= AUTO_SERVER_PREFIX.size() &&
         name.compare(0, AUTO_SERVER_PREFIX.size(), AUTO_SERVER_PREFIX) == 0;
}

}  // namespace AutoOpdsSync
