add_library(games STATIC
    slider_module.cpp
    flashing_module.cpp
    fingerprint_module.cpp
    games.h
)

target_include_directories(games PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(games PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_link_libraries(games PUBLIC
    gdi32
    user32
    gdiplus
    dwmapi
)

# --- MSVC-specific settings ---
if(MSVC)
    set_property(TARGET games PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(games PRIVATE /EHsc)
endif()
