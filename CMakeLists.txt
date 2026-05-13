cmake_minimum_required(VERSION 3.20)
project(hollow_l2_overlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# -----------------------------------------------------------------------------
# ImGui (vendored via FetchContent, built as a static library)
# -----------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.0
)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG master
)
FetchContent_MakeAvailable(imgui minhook)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# -----------------------------------------------------------------------------
# l2ui.dll — loaded into L2.exe via an IAT entry pointing at
# l2ui.dll!L2UI_Init (use CFF Explorer or `add_import.py` to wire it up).
# Hooks D3D9::EndScene/Present/Reset and renders an ImGui overlay inside
# L2's framebuffer.
# -----------------------------------------------------------------------------
add_library(l2ui SHARED
    src/dllmain.cpp
    src/overlay_ui.cpp
    src/d3d9_hook.cpp
)
set_target_properties(l2ui PROPERTIES
    PREFIX ""
    OUTPUT_NAME "l2ui"
    SUFFIX ".dll"
)
target_compile_definitions(l2ui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(l2ui PRIVATE imgui minhook d3d9)
if(MSVC)
    target_compile_options(l2ui PRIVATE /guard:cf- /Zi)
    set_target_properties(l2ui PROPERTIES
        LINK_FLAGS "/GUARD:NO /MAP /DEBUG /OPT:REF /OPT:ICF"
    )
endif()
