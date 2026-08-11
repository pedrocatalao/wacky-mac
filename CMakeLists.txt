cmake_minimum_required(VERSION 3.24)

# Universal binary: Intel + Apple Silicon (must be set before project())
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)

project(openwacky C)

set(CMAKE_C_STANDARD 11)

# SDL2 built from source as a universal static lib — no Homebrew dependency
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.8/SDL2-2.32.8.tar.gz)
FetchContent_MakeAvailable(SDL2)

add_executable(openwacky
    src/main.c
    src/ai.c
    src/physics.c
    src/hud.c
    src/sprites.c
    src/weapons.c
    src/wdat.c
    src/wpcx.c
    src/track.c)

target_link_libraries(openwacky PRIVATE SDL2::SDL2-static SDL2::SDL2main)
target_compile_options(openwacky PRIVATE -Wall -Wextra)
