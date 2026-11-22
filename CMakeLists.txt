cmake_minimum_required(VERSION 3.16)
project(NurseOCReport LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W3)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX _WIN32_WINNT=0x0601 WINVER=0x0601)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    if(WIN32)
        add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
    endif()
endif()

option(NURSEOCR_BUILD_TESTS "Build core unit and eval tests" ON)
option(NURSEOCR_BUILD_WINDOWS "Build Windows native desktop GUI" ON)
option(NURSEOCR_USE_YY_THUNKS "Link YY-Thunks for Win7 SP1 compatibility on MSVC" OFF)

add_subdirectory(core)

if(NURSEOCR_BUILD_TESTS)
    enable_testing()
    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE nurseocr_core)
    target_compile_definitions(core_tests PRIVATE
        NURSEOCR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME core_tests COMMAND core_tests)

    add_executable(local_archive_eval tests/local_archive_eval.cpp)
    target_link_libraries(local_archive_eval PRIVATE nurseocr_core)
endif()

if(WIN32 AND NURSEOCR_BUILD_WINDOWS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/windows/CMakeLists.txt")
    add_subdirectory(windows)
endif()
