add_subdirectory(games)

add_executable(${PROJECT_NAME} WIN32
    main.cpp
    app/app_ui.cpp
    app/app_ui.h
    app/localization.cpp
    app/localization.h
    app/app_runtime.cpp
    app/app_runtime.h
    capture/game_window.cpp
    capture/game_window.h
    resources/app.rc
    resources/resource.h
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    games
    gdi32
    user32
    shell32
    gdiplus
    comctl32
    dwmapi
)

# --- MSVC-specific settings ---
if(MSVC)
    set_property(TARGET ${PROJECT_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_options(${PROJECT_NAME} PRIVATE /EHsc)
endif()

# --- MinGW-w64 specific settings ---
if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -municode
        -mwindows
    )
endif()
