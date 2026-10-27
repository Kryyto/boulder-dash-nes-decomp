cmake_minimum_required(VERSION 3.20)
project(boulderdash_port C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# warnings for this project's code only (not for SDL when it is built from source)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(BD_WARNINGS -Wall -Wextra -Wno-unused-parameter)
endif()

# Game logic, no platform dependencies.
add_library(bdcore STATIC
    src/rom.c
    src/core.c
    src/cave.c
    src/scan.c
    src/player.c
    src/amoeba.c
    src/gfx.c
    src/camera.c
    src/sound.c
    src/apu.c
    src/state.c
    src/nmi.c
    src/io.c
    src/screen.c
    src/title.c
    src/menu.c
    src/ending.c)
target_include_directories(bdcore PUBLIC include)
target_compile_options(bdcore PRIVATE ${BD_WARNINGS})

# RAM-diff test against fixtures recorded by tools/record.py.
add_executable(ramdiff tests/ramdiff.c)
target_link_libraries(ramdiff PRIVATE bdcore)
target_compile_options(ramdiff PRIVATE ${BD_WARNINGS})

enable_testing()
# The fixtures come from your ROM (tools/record.py); tests whose fixture file
# doesn't exist yet are left out.
set(BD_ROM "${CMAKE_CURRENT_SOURCE_DIR}/../Boulder Dash (USA).nes" CACHE FILEPATH "Path to the Boulder Dash (USA) ROM")
set(BD_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build" CACHE PATH "Where tools/record.py wrote the fixtures")
foreach(fx IN ITEMS fixtures fixtures_fuzz fixtures_sound fixtures_screens)
    if(EXISTS "${BD_FIXTURE_DIR}/${fx}.bdfx")
        string(REPLACE "fixtures" "ramdiff" test_name "${fx}")
        add_test(NAME ${test_name} COMMAND ramdiff "${BD_ROM}" "${BD_FIXTURE_DIR}/${fx}.bdfx")
    endif()
endforeach()

# SDL2 player (optional): uses an installed SDL2, or fetches and builds it.
option(BD_VIEWER "Build the SDL2 player" ON)
if(BD_VIEWER)
    find_package(SDL2 CONFIG QUIET)
    if(NOT SDL2_FOUND)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL2
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-2.30.9
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL2)
    endif()
    add_executable(bdview src/viewer.c)
    target_compile_options(bdview PRIVATE ${BD_WARNINGS})
    if(TARGET SDL2::SDL2main)
        target_link_libraries(bdview PRIVATE SDL2::SDL2main)
    endif()
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(bdview PRIVATE bdcore SDL2::SDL2-static)
    else()
        target_link_libraries(bdview PRIVATE bdcore SDL2::SDL2)
    endif()
endif()
