cmake_minimum_required(VERSION 3.12)
project(wsf_afsim_ns3 LANGUAGES CXX)

if(NOT TARGET afsim_ns3_windows_adapter)
    add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/../windows_adapter"
        "${CMAKE_CURRENT_BINARY_DIR}/windows_adapter")
endif()

add_library(
    wsf_afsim_ns3
    SHARED
    source/AfsimNs3Extension.cpp)

target_compile_features(wsf_afsim_ns3 PRIVATE cxx_std_17)
target_link_libraries(
    wsf_afsim_ns3
    PRIVATE
    afsim_ns3_windows_adapter)

if(DEFINED WSF_LIBS)
    # AFSIM's root build supplies its normal include paths and base libraries.
    target_link_libraries(wsf_afsim_ns3 PRIVATE ${WSF_LIBS} wsf_mil)
else()
    set(AFSIM_SOURCE_ROOT "" CACHE PATH "AFSIM 2.9 source root")
    set(AFSIM_BUILD_ROOT "" CACHE PATH "AFSIM 2.9 build root")
    set(
        AFSIM_LIBRARY_CONFIG
        "RelWithDebInfo"
        CACHE STRING
        "AFSIM library configuration")

    if(MSVC AND CMAKE_CONFIGURATION_TYPES)
        # The AFSIM 2.9cn runtime in scope is built with this exact configuration.
        # Restrict the standalone solution so Debug CRT/iterator settings cannot be mixed in.
        set(
            CMAKE_CONFIGURATION_TYPES
            "${AFSIM_LIBRARY_CONFIG}"
            CACHE STRING
            "Supported Visual Studio configuration"
            FORCE)
    endif()

    if(NOT IS_DIRECTORY "${AFSIM_SOURCE_ROOT}" OR
       NOT IS_DIRECTORY "${AFSIM_BUILD_ROOT}")
        message(FATAL_ERROR "AFSIM_SOURCE_ROOT and AFSIM_BUILD_ROOT are required")
    endif()

    set(_afsim_source "${AFSIM_SOURCE_ROOT}")
    set(_afsim_build "${AFSIM_BUILD_ROOT}")
    set(_afsim_lib "${_afsim_build}/lib/${AFSIM_LIBRARY_CONFIG}")

    target_include_directories(
        wsf_afsim_ns3
        PRIVATE
        "${_afsim_build}/wsf/source/include"
        "${_afsim_source}/core/wsf/source"
        "${_afsim_source}/core/wsf/source/comm"
        "${_afsim_source}/core/wsf/source/event_pipe"
        "${_afsim_source}/core/wsf/source/mover"
        "${_afsim_source}/core/wsf/source/observer"
        "${_afsim_source}/core/wsf/source/processor"
        "${_afsim_source}/core/wsf/source/sensor"
        "${_afsim_build}/wsf/source"
        "${_afsim_source}/core/wsf_mil/source"
        "${_afsim_source}/core/wsf_mil/source/weapon"
        "${_afsim_build}/build_wsf_mil/source"
        "${_afsim_build}"
        "${_afsim_build}/include"
        "${_afsim_source}/tools/util/source"
        "${_afsim_build}/util/source"
        "${_afsim_source}/tools/util_script/source"
        "${_afsim_build}/util_script"
        "${_afsim_build}/util_script/source"
        "${_afsim_source}/tools/geodata/source"
        "${_afsim_build}/geodata/source"
        "${_afsim_source}/tools/genio/source"
        "${_afsim_build}/genio"
        "${_afsim_source}/tools/packetio/source"
        "${_afsim_build}/packetio"
        "${_afsim_source}/tools/dis/source"
        "${_afsim_build}/dis"
        "${_afsim_source}/tools/tracking_filters/source"
        "${_afsim_build}/tracking_filters"
        "${_afsim_source}/tools/profiling/source"
        "${_afsim_source}/tools/profiling/common"
        "${_afsim_build}/profiling/source"
        "${_afsim_source}/core/wsf_util/source"
        "${_afsim_source}/tools/utilzmq/source"
        "${_afsim_build}/build_warlock/warlock_exec/source/utilzmq"
        "${_afsim_build}/3rd_party/zmq-4.3.2-x64-vs2017/include")

    target_compile_definitions(
        wsf_afsim_ns3
        PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _SCL_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        _SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING
        NOMINMAX
        PROMOTE_HARDWARE_EXCEPTIONS
        SWDEV_ALL_USE_DLL)

    foreach(_library
            wsf
            wsf_mil
            geodata
            packetio
            dis
            tracking_filters
            genio
            profiling
            wsf_util
            util_script
            util
            utilzmq)
        target_link_libraries(
            wsf_afsim_ns3
            PRIVATE
            "${_afsim_lib}/${_library}.lib")
    endforeach()

    target_link_libraries(
        wsf_afsim_ns3
        PRIVATE
        "${_afsim_build}/3rd_party/zmq-4.3.2-x64-vs2017/release/libzmq-v141-mt-4_3_2.lib"
        Ws2_32
        Advapi32
        Iphlpapi)
endif()

if(MSVC)
    target_compile_options(
        wsf_afsim_ns3
        PRIVATE
        /W4
        /utf-8)
endif()

set_target_properties(
    wsf_afsim_ns3
    PROPERTIES
    PREFIX ""
    OUTPUT_NAME "wsf_afsim_ns3")
