add_executable(AutoOpdsScheduleTest
  AutoOpdsScheduleTest.cpp
)

target_include_directories(AutoOpdsScheduleTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(AutoOpdsScheduleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AutoOpdsScheduleTest)
