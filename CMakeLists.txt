#
# CMAKE build file for OpenPnP Capture library
#
# This generates make files for several build systems,
# such as GNU Make, Ninja and visual studio
#
# When invoking on Windows systems, make sure the
# compiler (Visual Studio) is in the search path.
#
# Author: Niels A. Moseley, Jason von Nieda
#

cmake_minimum_required(VERSION 3.14)
project (openpnp-capture)
set(OPENPNP_CAPTURE_LIB_VERSION "0.0.28" CACHE STRING "openpnp-capture library version")
set(OPENPNP_CAPTURE_LIB_SOVERSION "0" CACHE STRING "openpnp-capture library soversion")

# Library type options
option(BUILD_SHARED_LIBS "Build shared library" ON)
option(BUILD_STATIC_LIBS "Build static library" OFF)
option(LINK_DYNAMIC_MSVC_RUNTIME "Link MSVC runtime statically" OFF)
option(WITH_SIMD_LIBJPEG "Enable SIMD optimizations in libjpeg-turbo (disable for ARM64 Windows cross-compilation)" ON)

# -------------------------------------------------------------------
# spdlog — fast, header-only C++ logging library
# -------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.2
)
option(SPDLOG_BUILD_SHARED "Build spdlog shared" OFF)
if(MSVC)
    # Force spdlog to use the same MSVC runtime as openpnp-capture.
    # Without this, spdlog defaults to /MD while we use /MT, causing
    # LNK2038 (RuntimeLibrary mismatch) when linking test executables.
    if(LINK_DYNAMIC_MSVC_RUNTIME)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)
    endif()
endif()
FetchContent_MakeAvailable(spdlog)

# -------------------------------------------------------------------
# log compile level — controls SPDLOG_ACTIVE_LEVEL
# Levels below this are compiled out (zero cost).
# Override: cmake -DLOG_ACTIVE_LEVEL=trace|debug|info|warn|error
# -------------------------------------------------------------------
set(LOG_ACTIVE_LEVEL "trace" CACHE STRING "Minimum compiled-in log level")
set_property(CACHE LOG_ACTIVE_LEVEL PROPERTY STRINGS trace debug info warn error)

# map human-readable to spdlog integer constants
if(LOG_ACTIVE_LEVEL STREQUAL "trace")
    set(SPDLOG_ACTIVE_LEVEL_DEFINE "SPDLOG_LEVEL_TRACE")
elseif(LOG_ACTIVE_LEVEL STREQUAL "debug")
    set(SPDLOG_ACTIVE_LEVEL_DEFINE "SPDLOG_LEVEL_DEBUG")
elseif(LOG_ACTIVE_LEVEL STREQUAL "info")
    set(SPDLOG_ACTIVE_LEVEL_DEFINE "SPDLOG_LEVEL_INFO")
elseif(LOG_ACTIVE_LEVEL STREQUAL "warn")
    set(SPDLOG_ACTIVE_LEVEL_DEFINE "SPDLOG_LEVEL_WARN")
elseif(LOG_ACTIVE_LEVEL STREQUAL "error")
    set(SPDLOG_ACTIVE_LEVEL_DEFINE "SPDLOG_LEVEL_ERROR")
else()
    message(WARNING "Unknown LOG_ACTIVE_LEVEL '${LOG_ACTIVE_LEVEL}', defaulting to trace")
    set(SPDLOG_ACTIVE_LEVEL_DEFINE "SPDLOG_LEVEL_TRACE")
endif()

message(STATUS "Log active level: ${LOG_ACTIVE_LEVEL} (SPDLOG_ACTIVE_LEVEL=${SPDLOG_ACTIVE_LEVEL_DEFINE})")

# Validate library type options
if(BUILD_STATIC_LIBS AND BUILD_SHARED_LIBS)
    message(WARNING "Both BUILD_STATIC_LIBS and BUILD_SHARED_LIBS are enabled. Building static library only.")
    set(BUILD_SHARED_LIBS OFF)
elseif(NOT BUILD_STATIC_LIBS AND NOT BUILD_SHARED_LIBS)
    message(STATUS "Neither BUILD_STATIC_LIBS nor BUILD_SHARED_LIBS specified. Defaulting to shared library.")
    set(BUILD_SHARED_LIBS ON)
endif()

# make sure the libjpegturbo is compiled with the
# position independent flag -fPIC
IF (UNIX)
    set(POSITION_INDEPENDENT_CODE TRUE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

    # -march=native is invalid when cross-compiling (resolves to host CPU name
    # which the target backend won't recognize, e.g. 'apple-m3' for x86_64)
    set(_use_march_native TRUE)
    if(CMAKE_CROSSCOMPILING)
        set(_use_march_native FALSE)
    elseif(APPLE AND DEFINED CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "")
        set(_use_march_native FALSE)
    endif()

    if(_use_march_native)
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -march=native")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native")
    else()
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
    endif()
ENDIF()

# make CMAKE search the current cmake dir inside the
# current project
set (CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH};${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# force C++11 standard
set(CMAKE_CXX_STANDARD 11)

# load module that gets info from GIT 
# see: http://brianmilco.blogspot.nl/2012/11/cmake-automatically-use-git-tags-as.html
include(GetGitRevisionDescription)

# create library version from GIT tag using cmake/version.h.in as a template
# and write it to common/version.h
git_describe(GITVERSION --tags)
MESSAGE(STATUS "Using GIT tag: " ${GITVERSION} )
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/common/version.h)

# determine number of bits of compiler
if( CMAKE_SIZEOF_VOID_P EQUAL 8 )
  set( COMPILERBITS "64 bit")
else( CMAKE_SIZEOF_VOID_P EQUAL 8 )
  set( COMPILERBITS "32 bit")
endif( CMAKE_SIZEOF_VOID_P EQUAL 8 )

# check the build type and set the build type string
if(CMAKE_BUILD_TYPE MATCHES Release)
    add_definitions(-D__BUILDTYPE__="release")
else(CMAKE_BUILD_TYPE MATCHES Release)
    add_definitions(-D__BUILDTYPE__="debug")
endif(CMAKE_BUILD_TYPE MATCHES Release)

# create our capture library
if(BUILD_STATIC_LIBS AND NOT BUILD_SHARED_LIBS)
    set(LIBRARY_TYPE STATIC)
    add_definitions(-DOPENPNPCAPTURE_STATIC)
    message(STATUS "Building static library")
else()
    set(LIBRARY_TYPE SHARED)
    message(STATUS "Building shared library")
endif()

add_library(openpnp-capture ${LIBRARY_TYPE} common/libmain.cpp
                                           common/context.cpp
                                           common/logging.cpp
                                           common/logging_v2.cpp
                                           common/stream.cpp
                                           common/mjpeghelper.cpp)

target_include_directories(openpnp-capture PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# define common properties
set_target_properties(openpnp-capture PROPERTIES
                      VERSION ${OPENPNP_CAPTURE_LIB_VERSION}
                      SOVERSION ${OPENPNP_CAPTURE_LIB_SOVERSION})

# compile-time log level stripping (spdlog)
target_compile_definitions(openpnp-capture PRIVATE
    SPDLOG_ACTIVE_LEVEL=${SPDLOG_ACTIVE_LEVEL_DEFINE}
)

# spdlog is header-only; use the include path directly to avoid
# exporting a dependency on its CMake target in install(EXPORT)
target_include_directories(openpnp-capture PRIVATE
    ${spdlog_SOURCE_DIR}/include
)
# spdlog's bundled fmt requires UTF-8 source/execution charset on MSVC
if(MSVC)
    target_compile_options(openpnp-capture PRIVATE /utf-8)
endif()

# ======== libjpeg-turbo setup for ALL platforms ========
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_search_module(TurboJPEG libturbojpeg)
endif()
if(TurboJPEG_FOUND)
    target_link_directories(openpnp-capture PRIVATE ${TurboJPEG_LIBDIR})
    target_include_directories(openpnp-capture PRIVATE ${TurboJPEG_INCLUDE_DIRS})
    target_link_libraries(openpnp-capture PRIVATE ${TurboJPEG_LIBRARIES})
else()
    include(ExternalProject)
    set(PIC_FLAG "")
    set(RELEASE_FLAGS "")
    if(UNIX)
        set(PIC_FLAG "-DCMAKE_C_FLAGS=-fPIC" "-DCMAKE_CXX_FLAGS=-fPIC")
        set(RELEASE_FLAGS "-DCMAKE_C_FLAGS_RELEASE=-O3" "-DCMAKE_CXX_FLAGS_RELEASE=-O3")
    endif()
    set(LIBJPEG_TURBO_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo)
    set(LIBJPEG_TURBO_INSTALL_DIR ${LIBJPEG_TURBO_PREFIX}/install)
    if(MSVC)
        set(TURBOJPEG_LIB_FILE "${LIBJPEG_TURBO_INSTALL_DIR}/lib/turbojpeg-static${CMAKE_STATIC_LIBRARY_SUFFIX}")
    else()
        set(TURBOJPEG_LIB_FILE "${LIBJPEG_TURBO_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
    endif()
    set(ARCH_FLAGS "")
    if(DEFINED CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "")
        set(ARCH_FLAGS "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
    endif()
    ExternalProject_Add(libjpeg-turbo-external
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/linux/contrib/libjpeg-turbo-3.1.2
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${LIBJPEG_TURBO_INSTALL_DIR}
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DENABLE_SHARED=OFF
            -DWITH_SIMD=${WITH_SIMD_LIBJPEG}
            -DWITH_TOOLS=OFF
            -DWITH_JAVA=OFF
            ${ARCH_FLAGS}
            ${PIC_FLAG}
            ${RELEASE_FLAGS}
        BUILD_BYPRODUCTS ${TURBOJPEG_LIB_FILE}
    )
    add_library(turbojpeg-static STATIC IMPORTED)
    set_target_properties(turbojpeg-static PROPERTIES
        IMPORTED_LOCATION ${TURBOJPEG_LIB_FILE}
    )
    target_include_directories(openpnp-capture PRIVATE ${LIBJPEG_TURBO_INSTALL_DIR}/include)
    add_dependencies(openpnp-capture libjpeg-turbo-external)
    add_dependencies(turbojpeg-static libjpeg-turbo-external)
    target_link_libraries(openpnp-capture PRIVATE turbojpeg-static)
endif()
# ======================================================

IF (WIN32)
    if(MSVC)
        if(LINK_DYNAMIC_MSVC_RUNTIME)
            set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MD")
            set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MDd")
        else()
            set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
            set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
        endif()
    endif()

    # set the platform identification string
    add_definitions(-D__PLATFORM__="Win ${COMPILERBITS}")
    
    # remove annoying 'unsafe' function warnings
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)

    # add files for WIN32
    target_sources(openpnp-capture PRIVATE win/platformcontext.cpp
                                           win/platformstream.cpp)
    target_link_libraries(openpnp-capture PRIVATE strmiids)

    # add windows-specific test application
    add_subdirectory(win/tests)

ELSEIF(APPLE)
    # set the platform identification string
    add_definitions(-D__PLATFORM__="OSX ${COMPILERBITS}")

    target_sources(openpnp-capture PRIVATE mac/platformcontext.mm
                                           mac/platformstream.mm
                                           mac/uvcctrl.mm)

    # include OS X specific frameworks
    target_link_libraries(openpnp-capture PRIVATE
        "-framework AVFoundation"
        "-framework Foundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Accelerate"
        "-framework IOKit"
        )

    # add mac specific test application
    add_subdirectory(mac/tests)

ELSEIF(UNIX)
    # install path resolving
    include(GNUInstallDirs)

    # set the platform identification string
    add_definitions(-D__PLATFORM__="Linux ${COMPILERBITS}")

    target_sources(openpnp-capture PRIVATE linux/platformcontext.cpp
                                           linux/platformstream.cpp
                                           linux/yuvconverters.cpp)

    # add pthreads library
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)    
    target_link_libraries(openpnp-capture PRIVATE Threads::Threads)

    # add linux-specific test application
    add_subdirectory(linux/tests)

    # install lib and headers
    install(FILES include/openpnp-capture.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            COMPONENT headers)
    install(TARGETS openpnp-capture EXPORT openpnp-capture
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
            COMPONENT libraries)

    # add cmake install target
    install(EXPORT openpnp-capture
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/openpnp-capture
            COMPONENT libraries)

ENDIF()

