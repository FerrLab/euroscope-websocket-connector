# Standalone test project for the platform-independent parts of the plugin
# (the JSON contract layer). Runs on ANY OS — no Windows, no EuroScope SDK:
#
#   cmake -S tests -B build-tests
#   cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
#
# (Deliberately not wired into the root CMakeLists: the root refuses to
# configure on non-Windows / 64-bit, while the tests build everywhere.)

cmake_minimum_required(VERSION 3.20)
project(euroscope-websocket-connector-tests LANGUAGES CXX)

enable_testing()

add_executable(json_api_test
    json_api_test.cpp
    mock_chat_injection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/JsonApi.cpp
)
target_compile_features(json_api_test PRIVATE cxx_std_17)
target_include_directories(json_api_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME json_api COMMAND json_api_test)
