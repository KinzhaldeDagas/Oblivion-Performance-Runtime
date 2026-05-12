cmake_minimum_required(VERSION 3.20)

project(OblivionPerformanceRuntime LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "OblivionPerformanceRuntime must be built as 32-bit because Oblivion.exe and OBSE are 32-bit.")
endif()

add_library(OblivionPerformanceRuntime SHARED
    src/OblivionPerformanceRuntime.cpp
    src/OblivionPerformanceRuntime.def
)

target_compile_features(OblivionPerformanceRuntime PRIVATE cxx_std_17)
target_compile_definitions(OblivionPerformanceRuntime PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0601
    _WIN32_WINNT=0x0601
    NTDDI_VERSION=0x06010000
)
target_link_libraries(OblivionPerformanceRuntime PRIVATE winmm psapi)

if(MSVC)
    target_compile_options(OblivionPerformanceRuntime PRIVATE /W4 /permissive-)
    target_link_options(OblivionPerformanceRuntime PRIVATE /SUBSYSTEM:WINDOWS,6.01)
endif()

set_target_properties(OblivionPerformanceRuntime PROPERTIES
    OUTPUT_NAME "OblivionPerformanceRuntime"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

add_custom_command(TARGET OblivionPerformanceRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/OblivionPerformanceRuntime.ini"
        "$<TARGET_FILE_DIR:OblivionPerformanceRuntime>/OblivionPerformanceRuntime.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "$<TARGET_FILE_DIR:OblivionPerformanceRuntime>/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
        "$<TARGET_FILE_DIR:OblivionPerformanceRuntime>/CHANGELOG.md"
)
