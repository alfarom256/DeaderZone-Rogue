cmake_minimum_required(VERSION 3.20)
project(DeaderZoneRogue LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── MinHook ──────────────────────────────────────────────────────────
add_library(minhook STATIC
    vendor/minhook/src/buffer.c
    vendor/minhook/src/hook.c
    vendor/minhook/src/trampoline.c
    vendor/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC vendor/minhook/include)
set_target_properties(minhook PROPERTIES C_STANDARD 11)

# ── ImGui ────────────────────────────────────────────────────────────
add_library(imgui STATIC
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
    vendor/imgui/backends/imgui_impl_dx11.cpp
    vendor/imgui/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui PUBLIC
    vendor/imgui
    vendor/imgui/backends
)
target_compile_definitions(imgui PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

# ── Mod DLL (dwmapi.dll proxy) ──────────────────────────────────────
add_library(dwmapi SHARED
    src/dllmain.cpp
    src/hooks.cpp
    src/ue_engine.cpp
    src/damage_hook.cpp
    src/stats_overlay.cpp
    src/damage_tracker.cpp
    src/dwmapi_proxy.def
)
target_include_directories(dwmapi PRIVATE
    src
    vendor/imgui
    vendor/imgui/backends
)
target_link_libraries(dwmapi PRIVATE
    minhook
    imgui
    d3d11
    d3d12
    dxgi
    windowscodecs
    ole32
    winmm
)
target_compile_definitions(dwmapi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
