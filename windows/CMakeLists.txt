cmake_minimum_required(VERSION 3.16)

add_executable(NurseOCReport WIN32
    src/main_win.cpp
    src/editable_grid.cpp
    src/photo_canvas.cpp
    app.rc
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
