cmake_minimum_required(VERSION 3.20)
project(obs-localvqe LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# libobs: prefer the CMake package shipped by obs-studio, fall back to pkg-config.
find_package(libobs QUIET)
if(NOT libobs_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBOBS REQUIRED libobs)
endif()

# LocalVQE: either built in-tree (ggml/ subdir built with LOCALVQE_BUILD_SHARED=ON)
# or installed system-wide. Allow LOCALVQE_ROOT to override.
find_path(LOCALVQE_INCLUDE_DIR NAMES localvqe_api.h
    HINTS ${LOCALVQE_ROOT}/include ${CMAKE_SOURCE_DIR}/../ggml)
find_library(LOCALVQE_LIBRARY NAMES localvqe
    HINTS ${LOCALVQE_ROOT}/lib ${CMAKE_SOURCE_DIR}/../build/ggml/bin)
if(NOT LOCALVQE_INCLUDE_DIR OR NOT LOCALVQE_LIBRARY)
    message(FATAL_ERROR
        "LocalVQE not found. Build the parent project with "
        "-DLOCALVQE_BUILD_SHARED=ON, or set -DLOCALVQE_ROOT=<install-prefix>.")
endif()

add_library(obs-localvqe MODULE
    src/plugin-main.c
    src/localvqe-filter.c)

target_include_directories(obs-localvqe PRIVATE
    ${LOCALVQE_INCLUDE_DIR}
    ${LIBOBS_INCLUDE_DIRS})

if(TARGET OBS::libobs)
    target_link_libraries(obs-localvqe PRIVATE OBS::libobs)
else()
    target_link_libraries(obs-localvqe PRIVATE ${LIBOBS_LIBRARIES})
    target_link_directories(obs-localvqe PRIVATE ${LIBOBS_LIBRARY_DIRS})
endif()
target_link_libraries(obs-localvqe PRIVATE ${LOCALVQE_LIBRARY})

set_target_properties(obs-localvqe PROPERTIES PREFIX "")

# User-local install layout that OBS discovers automatically:
#   ~/.config/obs-studio/plugins/obs-localvqe/bin/64bit/obs-localvqe.so
#   ~/.config/obs-studio/plugins/obs-localvqe/data/locale/en-US.ini
#   ~/.config/obs-studio/plugins/obs-localvqe/data/localvqe-v1.1-1.3M-f32.gguf
set(OBS_PLUGIN_DESTINATION
    "$ENV{HOME}/.config/obs-studio/plugins/obs-localvqe"
    CACHE PATH "OBS plugin install root")

install(TARGETS obs-localvqe LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}/bin/64bit")
install(DIRECTORY data/ DESTINATION "${OBS_PLUGIN_DESTINATION}/data")
