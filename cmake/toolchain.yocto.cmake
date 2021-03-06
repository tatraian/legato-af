#*******************************************************************************
# Copyright (C) Sierra Wireless Inc.
#*******************************************************************************

# This script requires TARGET_CC to be set in the environment.
# It must contain the path to the C cross compiler.

# Optionally, the target sysroot can be set using the TARGET_SYSROOT
# environment variable.

# We will force the C compiler to a specific one, instead of leaving it to
# CMake to find one for us.
include(CMakeForceCompiler)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_VERSION 1)

if(NOT DEFINED ENV{TARGET_CC})
    message(FATAL_ERROR "TARGET_CC not set")
endif()

set(TARGET_CC "$ENV{TARGET_CC}")

if(NOT DEFINED ENV{TARGET_CXX})
    message(FATAL_ERROR "TARGET_CXX not set")
endif()

set(TARGET_CXX "$ENV{TARGET_CXX}")

if(NOT DEFINED USE_CLANG)
    set(USE_CLANG $ENV{USE_CLANG})
endif()

message("Target compilers: ${TARGET_CC} ${TARGET_CXX}")

# Check for sysroot override via TARGET_SYSROOT environment variable.
if (DEFINED ENV{TARGET_SYSROOT})
    set(TARGET_SYSROOT "$ENV{TARGET_SYSROOT}")
    message("Target sysroot overridden by TARGET_SYSROOT environment variable")
else()
    # Ask the compiler for its sysroot.
    if(NOT USE_CLANG EQUAL 1)
        execute_process(
            COMMAND ${TARGET_CC} --print-sysroot
            OUTPUT_VARIABLE TARGET_SYSROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()
endif()

message("Target sysroot: ${TARGET_SYSROOT}")

CMAKE_FORCE_C_COMPILER("${TARGET_CC}" GNU)
CMAKE_FORCE_CXX_COMPILER("${TARGET_CXX}" GNU)

SET(CMAKE_REQUIRED_FLAGS "--sysroot=${TARGET_SYSROOT}")
add_definitions(--sysroot=${TARGET_SYSROOT})
SET(CMAKE_EXE_LINKER_FLAGS "--sysroot=${TARGET_SYSROOT}" CACHE STRING "" FORCE)
SET(CMAKE_SHARED_LINKER_FLAGS "--sysroot=${TARGET_SYSROOT}" CACHE STRING "" FORCE)
SET(CMAKE_MODULE_LINKER_FLAGS "--sysroot=${TARGET_SYSROOT}" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH "${TARGET_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

