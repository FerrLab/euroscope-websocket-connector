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

add_executable(ws_test
    ws_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ws/WsFrame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ws/WsHandshake.cpp
)
target_compile_features(ws_test PRIVATE cxx_std_17)
target_include_directories(ws_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_test(NAME ws COMMAND ws_test)

# End-to-end WsClient test: the production client code (POSIX branch)
# against a live in-process server. UNIX-only; on Windows the client is
# exercised inside EuroScope itself.
if(UNIX)
    add_executable(ws_client_test
        ws_client_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ws/WsClient.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ws/WsFrame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ws/WsHandshake.cpp
    )
    target_compile_features(ws_client_test PRIVATE cxx_std_17)
    target_include_directories(ws_client_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    find_package(Threads REQUIRED)
    target_link_libraries(ws_client_test PRIVATE Threads::Threads)
    add_test(NAME ws_client COMMAND ws_client_test)
endif()
