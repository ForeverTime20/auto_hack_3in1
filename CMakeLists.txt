cmake_minimum_required(VERSION 3.16)
project(auto_hack_4in1
    VERSION 1.2.0
    DESCRIPTION "Windows C++/Win32 helper for four GTA Online hacking minigames"
    LANGUAGES CXX RC
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src)
