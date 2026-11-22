cmake_minimum_required(VERSION 3.22.1)
project(nurseocr_jni LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Point to shared core/ and deps/
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(NURSEOCR_DEPS_DIR "${REPO_ROOT}/deps" CACHE PATH "" FORCE)

add_subdirectory("${REPO_ROOT}/core" "${CMAKE_CURRENT_BINARY_DIR}/core")

add_library(nurseocr_jni SHARED
    jni_bridge.cpp
)

target_link_libraries(nurseocr_jni PRIVATE
    nurseocr_core
    android
    jnigraphics
    log
)
