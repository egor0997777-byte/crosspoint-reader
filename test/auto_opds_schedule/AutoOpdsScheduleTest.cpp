#include <gtest/gtest.h>

#include <string_view>

#include "AutoOpdsSyncSchedule.h"

namespace {

using AutoOpdsSync::isAutoServerName;
using AutoOpdsSync::secondsUntilNextLocal0400;

TEST(AutoOpdsSchedule, ExactFourOclockRollsToTomorrow) {
  EXPECT_EQ(secondsUntilNextLocal0400(4, 0, 0, 48), 24 * 60 * 60);
}

TEST(AutoOpdsSchedule, CalculatesBoundaryWithinTheSameDay) {
  EXPECT_EQ(secondsUntilNextLocal0400(3, 59, 59, 48), 1);
  EXPECT_EQ(secondsUntilNextLocal0400(1, 30, 0, 48), 2 * 60 * 60 + 30 * 60);
}

TEST(AutoOpdsSchedule, CrossesMidnight) {
  EXPECT_EQ(secondsUntilNextLocal0400(23, 30, 0, 48), 4 * 60 * 60 + 30 * 60);
}

TEST(AutoOpdsSchedule, AppliesPositiveNegativeAndQuarterHourOffsets) {
  // 00:00 UTC +03:00 = 03:00 local.
  EXPECT_EQ(secondsUntilNextLocal0400(0, 0, 0, 60), 60 * 60);
  // 08:00 UTC -05:00 = 03:00 local.
  EXPECT_EQ(secondsUntilNextLocal0400(8, 0, 0, 28), 60 * 60);
  // 22:15 UTC +05:45 = 04:00 local.
  EXPECT_EQ(secondsUntilNextLocal0400(22, 15, 0, 71), 24 * 60 * 60);
}

TEST(AutoOpdsSchedule, RejectsInvalidRtcFields) {
  EXPECT_EQ(secondsUntilNextLocal0400(24, 0, 0, 48), -1);
  EXPECT_EQ(secondsUntilNextLocal0400(0, 60, 0, 48), -1);
  EXPECT_EQ(secondsUntilNextLocal0400(0, 0, 60, 48), -1);
}

TEST(AutoOpdsSchedule, RecognisesOnlyTheExactCaseSensitivePrefix) {
  EXPECT_TRUE(isAutoServerName("AUTO:"));
  EXPECT_TRUE(isAutoServerName("AUTO: Лайфхакер"));
  EXPECT_TRUE(isAutoServerName(std::string_view{"AUTO:ChatGPT"}));
  EXPECT_FALSE(isAutoServerName("AUTO"));
  EXPECT_FALSE(isAutoServerName("auto: ChatGPT"));
  EXPECT_FALSE(isAutoServerName(" AUTO: ChatGPT"));
  EXPECT_FALSE(isAutoServerName("AUTOMATIC: ChatGPT"));
}

}  // namespace
