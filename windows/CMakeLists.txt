cmake_minimum_required(VERSION 3.16)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(NURSEOCR_PACKED_MODELS "${CMAKE_CURRENT_BINARY_DIR}/embedded_models.pack")
set(NURSEOCR_MODEL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/../models/PP_OCRv5_mobile_det.ncnn.param"
    "${CMAKE_CURRENT_SOURCE_DIR}/../models/PP_OCRv5_mobile_det.ncnn.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/../models/PP_OCRv5_mobile_rec.ncnn.param"
    "${CMAKE_CURRENT_SOURCE_DIR}/../models/PP_OCRv5_mobile_rec.ncnn.bin"
)

add_custom_command(
    OUTPUT "${NURSEOCR_PACKED_MODELS}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/pack_models_cpp.py"
            --models-dir "${CMAKE_CURRENT_SOURCE_DIR}/../models"
            --output "${NURSEOCR_PACKED_MODELS}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/pack_models_cpp.py"
        ${NURSEOCR_MODEL_FILES}
    COMMENT "Packing compressed OCR models into embedded_models.pack"
    VERBATIM
)

set_source_files_properties(app.rc PROPERTIES
    OBJECT_DEPENDS "${NURSEOCR_PACKED_MODELS};${CMAKE_CURRENT_SOURCE_DIR}/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
)

add_executable(NurseOCReport WIN32
    src/main_win.cpp
    src/editable_grid.cpp
    src/photo_canvas.cpp
    app.rc
)

target_include_directories(NurseOCReport PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../deps/miniz"
)

target_link_libraries(NurseOCReport PRIVATE
    nurseocr_core
    comctl32
    comdlg32
    shell32
    shlwapi
    gdi32
    user32
    ole32
    advapi32
)

target_compile_definitions(NurseOCReport PRIVATE
    UNICODE
    _UNICODE
)

if(MSVC)
    target_link_options(NurseOCReport PRIVATE /SUBSYSTEM:WINDOWS,6.01 /MANIFEST:NO /INCREMENTAL:NO)
    if(NURSEOCR_USE_YY_THUNKS)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            get_filename_component(YY_OBJ "${CMAKE_CURRENT_SOURCE_DIR}/../deps/yy-thunks/objs/x64/YY_Thunks_for_Win7.obj" ABSOLUTE)
        else()
            get_filename_component(YY_OBJ "${CMAKE_CURRENT_SOURCE_DIR}/../deps/yy-thunks/objs/x86/YY_Thunks_for_Win7.obj" ABSOLUTE)
            target_link_options(NurseOCReport PRIVATE /LARGEADDRESSAWARE)
        endif()
        if(EXISTS "${YY_OBJ}")
            target_link_options(NurseOCReport PRIVATE "${YY_OBJ}")
        else()
            message(FATAL_ERROR "YY-Thunks obj not found: ${YY_OBJ}")
        endif()
    endif()
elseif(MINGW)
    target_link_options(NurseOCReport PRIVATE -municode -mwindows -static -static-libgcc -static-libstdc++)
endif()
