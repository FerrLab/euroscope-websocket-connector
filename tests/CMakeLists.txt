# Standalone test project for the platform-independent parts of the plugin.
# Runs on ANY OS — no Windows, no EuroScope:
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

add_executable(gateway_config_test
    gateway_config_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/GatewayConfig.cpp
)
target_compile_features(gateway_config_test PRIVATE cxx_std_17)
target_include_directories(gateway_config_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_test(NAME gateway_config COMMAND gateway_config_test)

add_executable(injection_verdict_test
    injection_verdict_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/InjectionVerdict.cpp
)
target_compile_features(injection_verdict_test PRIVATE cxx_std_17)
target_include_directories(injection_verdict_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_test(NAME injection_verdict COMMAND injection_verdict_test)

# End-to-end transport test: the production Gateway (poll/send threads,
# backoff, auth) over live TCP against a scripted HTTP backend. UNIX-only
# (the test HTTP client/server use POSIX sockets); on Windows the same
# Gateway runs on WinHTTP.
if(UNIX)
    find_package(Threads REQUIRED)

    add_executable(http_gateway_test
        http_gateway_test.cpp
        posix_http_client.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/Gateway.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/http/HttpUrl.cpp
    )
    target_compile_features(http_gateway_test PRIVATE cxx_std_17)
    target_include_directories(http_gateway_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(http_gateway_test PRIVATE Threads::Threads)
    add_test(NAME http_gateway COMMAND http_gateway_test)
endif()
