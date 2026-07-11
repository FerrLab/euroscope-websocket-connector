# EuroScope WebSocket Connector plugin.
#
# EuroScope is a 32-bit Windows application, so this MUST be configured for
# a 32-bit MSVC toolchain:
#
#   cmake -B build -A Win32
#   cmake --build build --config Release
#
# Output: build/Release/WebSocketConnector.dll
# Full instructions: docs/BUILDING.md

cmake_minimum_required(VERSION 3.20)
project(euroscope-websocket-connector VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "EuroScope plugins are Windows DLLs and must be built with MSVC on "
        "Windows (see docs/BUILDING.md).")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "EuroScope is a 32-bit application; a 64-bit plugin will not load. "
        "Configure with:  cmake -B build -A Win32")
endif()

add_library(WebSocketConnector SHARED
    src/dllmain.cpp
    src/ConnectorPlugin.cpp
    src/Actions.cpp
    src/ChatInjection.cpp
    src/InjectionVerdict.cpp
    src/JsonApi.cpp
    src/Gateway.cpp
    src/GatewayConfig.cpp
    src/http/HttpUrl.cpp
    src/http/WinHttpClient.cpp
)

target_compile_features(WebSocketConnector PRIVATE cxx_std_17)

# _MBCS: the EuroScope API uses multi-byte strings (the VATSIM protocol has
# no Unicode); never build this with UNICODE defined.
target_compile_definitions(WebSocketConnector PRIVATE
    _MBCS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(WebSocketConnector PRIVATE sdk src third_party)
target_link_libraries(WebSocketConnector PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk/EuroScopePlugInDll.lib"
    winhttp)

# Static CRT so users don't need a matching Visual C++ redistributable.
set_target_properties(WebSocketConnector PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    PREFIX ""
    OUTPUT_NAME "WebSocketConnector"
)
