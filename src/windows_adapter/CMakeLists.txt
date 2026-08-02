cmake_minimum_required(VERSION 3.12)
project(afsim_ns3_windows_adapter LANGUAGES CXX)

add_library(
    afsim_ns3_windows_adapter
    STATIC
    AfsimBridgeController.cpp
    AfsimNs3Protocol.cpp
    GeoCoordinates.cpp
    NetworkEffectGate.cpp
    StateDeltaTracker.cpp
    TcpJsonlClient.cpp)

target_include_directories(
    afsim_ns3_windows_adapter
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_features(afsim_ns3_windows_adapter PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(
        afsim_ns3_windows_adapter
        PRIVATE
        /W4
        /permissive-
        /utf-8)
    target_compile_definitions(
        afsim_ns3_windows_adapter
        PRIVATE
        _WIN32_WINNT=0x0601)
    target_link_libraries(afsim_ns3_windows_adapter PRIVATE Ws2_32)
else()
    target_compile_options(
        afsim_ns3_windows_adapter
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic)
    find_package(Threads REQUIRED)
    target_link_libraries(
        afsim_ns3_windows_adapter
        PRIVATE
        Threads::Threads)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(
        windows_adapter_unit
        "../../tests/windows_adapter_unit.cpp")
    target_link_libraries(
        windows_adapter_unit
        PRIVATE
        afsim_ns3_windows_adapter)
    add_test(NAME windows_adapter_unit COMMAND windows_adapter_unit)

    add_executable(
        windows_adapter_probe
        "../../tests/windows_adapter_probe.cpp")
    target_link_libraries(
        windows_adapter_probe
        PRIVATE
        afsim_ns3_windows_adapter)
endif()
