cmake_minimum_required(VERSION 3.2)

include(FetchContent)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(psd_text_parser_tests
  PsdTextParserTests.cpp
)
target_link_libraries(psd_text_parser_tests
  PRIVATE
    Psd
    gtest_main
)
target_include_directories(psd_text_parser_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_test(NAME PsdTextParserTests COMMAND psd_text_parser_tests)
