cmake_minimum_required(VERSION 3.16)

set(NURSEOCR_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps" CACHE PATH "Path to fetched third-party dependencies")

# Build ncnn cleanly as CPU-only static library
set(NCNN_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(NCNN_VULKAN OFF CACHE BOOL "" FORCE)
set(NCNN_OPENMP OFF CACHE BOOL "" FORCE)
set(NCNN_SIMPLEOMP OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NCNN_INSTALL_SDK OFF CACHE BOOL "" FORCE)
set(NCNN_PYTHON OFF CACHE BOOL "" FORCE)
set(NCNN_DISABLE_RTTI OFF CACHE BOOL "" FORCE)
set(NCNN_DISABLE_EXCEPTION OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_WITH_STATIC_CRT ON CACHE BOOL "" FORCE)
# Keep SSE2, AVX, FMA, F16C, and AVX2 while disabling legacy/server-only x86 ISA variants for faster x86/x64 builds
set(NCNN_FMA4 OFF CACHE BOOL "" FORCE)
set(NCNN_XOP OFF CACHE BOOL "" FORCE)
set(NCNN_AVXVNNI OFF CACHE BOOL "" FORCE)
set(NCNN_AVXNECONVERT OFF CACHE BOOL "" FORCE)
set(NCNN_AVX512 OFF CACHE BOOL "" FORCE)
if(NOT EXISTS "${NURSEOCR_DEPS_DIR}/ncnn-src/CMakeLists.txt")
    message(FATAL_ERROR "Missing deps/ncnn-src. Run python scripts/fetch_deps.py first.")
endif()
add_subdirectory("${NURSEOCR_DEPS_DIR}/ncnn-src" "${CMAKE_CURRENT_BINARY_DIR}/ncnn" EXCLUDE_FROM_ALL)

# Build libwebp decoder cleanly as static library
set(WEBP_LINK_STATIC ON CACHE BOOL "" FORCE)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_FUZZTEST OFF CACHE BOOL "" FORCE)
if(NOT EXISTS "${NURSEOCR_DEPS_DIR}/libwebp/CMakeLists.txt")
    message(FATAL_ERROR "Missing deps/libwebp. Run python scripts/fetch_deps.py first.")
endif()
add_subdirectory("${NURSEOCR_DEPS_DIR}/libwebp" "${CMAKE_CURRENT_BINARY_DIR}/libwebp" EXCLUDE_FROM_ALL)

# Core C++17 library
add_library(nurseocr_core STATIC
    src/calculator.cpp
    src/storage.cpp
    src/image_io.cpp
    src/name_doc_reader.cpp
    src/table_cv.cpp
    src/ocr_engine.cpp
    src/xlsx_exporter.cpp
    "${NURSEOCR_DEPS_DIR}/miniz/miniz.c"
    "${NURSEOCR_DEPS_DIR}/pugixml/src/pugixml.cpp"
)

target_include_directories(nurseocr_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${NURSEOCR_DEPS_DIR}"
        "${NURSEOCR_DEPS_DIR}/miniz"
        "${NURSEOCR_DEPS_DIR}/pugixml/src"
        "${NURSEOCR_DEPS_DIR}/ppocr"
        "${NURSEOCR_DEPS_DIR}/libwebp/src"
)

target_link_libraries(nurseocr_core
    PUBLIC
        ncnn
        webpdecoder
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties("${NURSEOCR_DEPS_DIR}/miniz/miniz.c" PROPERTIES
        COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable;-Wno-sign-compare")
endif()
