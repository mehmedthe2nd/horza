cmake_minimum_required(VERSION 3.19)
project(horza)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
pkg_check_modules(DEPS REQUIRED hyprland pixman-1 libdrm)

add_library(horza SHARED
    main.cpp
    overview.cpp
    OverviewPassElement.cpp
)

target_include_directories(horza PRIVATE ${DEPS_INCLUDE_DIRS})
target_compile_definitions(horza PRIVATE WLR_USE_UNSTABLE)
target_compile_options(horza PRIVATE -Wall)

install(TARGETS horza
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
