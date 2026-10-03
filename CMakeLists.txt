cmake_minimum_required(VERSION 3.15)

project(Pilot
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# much faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h. Set
# globally rather than per-file: the next file to include windows.h hits it too,
# and the error gives no hint what happened.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(PILOT_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# The FFGL plugin, the FFGL SDK submodule and GLEW are one dependency group, and
# the OpenFX plugin belongs to none of it. Turning this OFF builds the OFX plugin
# alone, with no GL loader anywhere in the configure -- which is what the Linux
# job does, because the FFGL SDK has no Linux path and GLEW would otherwise have
# to be installed to build something that never calls it. The harness drives the
# FFGL class, so it goes with this too.
option(PILOT_BUILD_FFGL "Build the FFGL plugin (needs the FFGL SDK and, off macOS, GLEW)" ON)

# ---------------------------------------------------------------------------
# The parts with no GL in them at all: the address order, the tape, the
# machines, the controls, the font, one frame's uniforms (Frame.cpp) and the
# CPU copy of the three passes (Render.cpp).
#
# A separate library because all three consumers need it and only one has a GL
# context: the FFGL plugin, the harness, and the OpenFX plugin, which renders on
# the CPU and is built on Linux with no GL loader configured at all. This split
# is what lets the OpenFX build link the SAME arithmetic rather than a second
# transcription of it.
# ---------------------------------------------------------------------------
add_library(pilot_model OBJECT
    source/Font.h
    source/Font.cpp
    source/Spectrum.h
    source/Spectrum.cpp
    source/Machines.h
    source/Machines.cpp
    source/Loader.h
    source/Loader.cpp
    source/Controls.h
    source/Frame.h
    source/Frame.cpp
    source/Render.h
    source/Render.cpp
)
target_include_directories(pilot_model PUBLIC source)
set_target_properties(pilot_model PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PILOT_BUILD_FFGL)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the FFGL plugin is beyond the model, as an OBJECT library so both
# the bundle and the offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor in Pilot.cpp and nothing ever references that object by
# name, so in an archive the linker is entitled to drop the whole translation
# unit -- and the result is a bundle that loads, exports plugMain, and reports
# that it contains no plugins. Do not "tidy" it to STATIC.
# ---------------------------------------------------------------------------
add_library(pilot_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/StoatworksAbout.h
    source/StoatworksAboutLinks.h
    source/StoatworksAboutParams.h
    source/Shaders.h
    source/shaders/Vertex.cpp
    source/shaders/Raster.cpp
    source/shaders/Attr.cpp
    source/shaders/Compose.cpp
    source/Pilot.h
    source/Pilot.cpp
)

target_include_directories(pilot_core PUBLIC source)
target_link_libraries(pilot_core PUBLIC ffgl::sdk pilot_model)
target_compile_definitions(pilot_core PUBLIC PILOT_VERSION="${PROJECT_VERSION}")
set_target_properties(pilot_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers pull in <GL/glew.h>. The SDK links GLEW
# privately, so consumers need it too -- for the include path and for the
# GLEW_STATIC define the static vcpkg triplet propagates. macOS uses the system
# OpenGL framework and needs none of this. GLEW arrives through vcpkg.json,
# which nothing in this file points at: lose it and only Windows breaks, and
# only at configure time.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(pilot_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin. FFGL resolves one plugMain per binary.
# ---------------------------------------------------------------------------
function(pilot_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    # pilot_model is named again here, and on every other final target, because
    # an OBJECT library's objects do NOT travel transitively through a second
    # OBJECT library. pilot_core linking it gives this target the include paths
    # and nothing to link, and the failure is a pile of undefined symbols at the
    # very last step of an otherwise clean build.
    target_link_libraries(${target} PRIVATE pilot_core pilot_model)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL"
            "-framework Carbon"
            "-framework AppKit"
        )
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

pilot_add_plugin(Pilot source/PluginEntry.cpp
    "Pilot" "com.stoatworks.ffgl.pilot")

# ---------------------------------------------------------------------------
# The harness: the real plugin class in a real GL context, with no host.
#
# macOS only, because it stands up a CGL context directly. Most of what it
# checks needs no context at all -- the address order, the border period and the
# error rate are integer arithmetic against source/Spectrum.cpp and
# source/Loader.cpp -- and those modes deliberately run before any GL call, so
# they still work on a machine that cannot make a context.
# ---------------------------------------------------------------------------
if(PILOT_BUILD_TOOLS AND APPLE)
    enable_testing()

    add_executable(pttest tools/pttest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(pttest PRIVATE pilot_core pilot_model z
        "-framework OpenGL"
        "-framework AppKit"
    )

    add_test(NAME agree      COMMAND pttest --agree)
    add_test(NAME order      COMMAND pttest --order)
    add_test(NAME thirds     COMMAND pttest --thirds)
    add_test(NAME attributes COMMAND pttest --attributes)
    add_test(NAME border     COMMAND pttest --border)
    add_test(NAME error      COMMAND pttest --error)
    add_test(NAME message    COMMAND pttest --message)
    add_test(NAME clock      COMMAND pttest --clock)
    add_test(NAME names      COMMAND pttest --names)
    add_test(NAME negative   COMMAND pttest --negative)
    add_test(NAME reveal     COMMAND pttest --reveal)
    add_test(NAME pixels     COMMAND pttest --pixels)
    add_test(NAME cpu        COMMAND pttest --cpu)
    add_test(NAME transition COMMAND pttest --transition)
endif()

# ---------------------------------------------------------------------------
# `cmake --install build` drops the bundle where Arena reads it. Avenue reads
# ~/Documents/Resolume Avenue/Extra Effects instead; pass --prefix for that.
# ---------------------------------------------------------------------------
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()
install(TARGETS Pilot
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)

endif()  # PILOT_BUILD_FFGL

# ---------------------------------------------------------------------------
# The OpenFX build, for Resolve/Nuke/Natron/Vegas. A CPU render against the
# vendored OFX SDK subset under external/openfx (C headers + the official C++
# Support library, BSD-3). pilot_model links straight in, so the address order,
# the tape, the machines, the controls and every uniform are the same code as
# the FFGL build; the three passes are Render.cpp, the marked CPU copy of the
# GLSL.
#
# Not installed by `cmake --install`: /Library/OFX/Plugins is root-owned and a
# build has no business writing there. Copy build/Pilot.ofx.bundle by hand.
# ---------------------------------------------------------------------------
option(BUILD_OFX "Build the OpenFX plugin" ON)

if(BUILD_OFX)
    set(OFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/openfx")

    add_library(PilotOFX MODULE
        source/ofx/PilotOFX.cpp
        source/ofx/StoatworksAboutOFX.h
        ${OFX_SDK}/Support/Library/ofxsCore.cpp
        ${OFX_SDK}/Support/Library/ofxsImageEffect.cpp
        ${OFX_SDK}/Support/Library/ofxsInteract.cpp
        ${OFX_SDK}/Support/Library/ofxsLog.cpp
        ${OFX_SDK}/Support/Library/ofxsMultiThread.cpp
        ${OFX_SDK}/Support/Library/ofxsParams.cpp
        ${OFX_SDK}/Support/Library/ofxsProperty.cpp
        ${OFX_SDK}/Support/Library/ofxsPropertyValidation.cpp
    )
    # PRIVATE on an OBJECT library gives both the objects and the usage
    # requirements. `$<TARGET_OBJECTS:pilot_model>` would give the objects
    # alone -- no include directories, no compile definitions -- and the
    # failure is platform-specific enough to cost a day.
    target_link_libraries(PilotOFX PRIVATE pilot_model)
    target_include_directories(PilotOFX PRIVATE
        source
        "${OFX_SDK}/include"
        "${OFX_SDK}/Support/include"
    )
    target_compile_definitions(PilotOFX PRIVATE
        PILOT_VERSION="${PROJECT_VERSION}"
        PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    )
    set_target_properties(PilotOFX PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # Threads, explicitly. The OFX Support library's multi-thread suite calls
    # pthread_create, and on Rocky 8 -- glibc 2.28, the floor Resolve supports --
    # that still lives in libpthread; libpthread was only folded into libc in
    # glibc 2.34. Leave it out and the plugin compiles, links, passes a symbol
    # check and a glibc-version check, and then fails at dlopen with "undefined
    # symbol: pthread_create". macOS and Windows never show it.
    find_package(Threads REQUIRED)
    target_link_libraries(PilotOFX PRIVATE Threads::Threads)

    set(OFX_BUNDLE_DIR "${CMAKE_BINARY_DIR}/Pilot.ofx.bundle")
    set_target_properties(PilotOFX PROPERTIES
        PREFIX ""
        SUFFIX ".ofx"
        OUTPUT_NAME "Pilot"
        LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/MacOS"
    )
    if(APPLE)
        set(OFX_BUNDLE_IDENTIFIER "com.stoatworks.pilot.ofx")
        configure_file(cmake/InfoOFX.plist.in
            "${OFX_BUNDLE_DIR}/Contents/Info.plist" @ONLY)
    elseif(WIN32)
        set_target_properties(PilotOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Win64"
            LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/Contents/Win64"
            # MSVC's .exp/.lib for this target; its default directory collides
            # with the FFGL target's, which shares the output base name, and a
            # parallel build then races on the file (LNK1104).
            ARCHIVE_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/importlib"
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${OFX_BUNDLE_DIR}/importlib")
    else()
        set_target_properties(PilotOFX PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE_DIR}/Contents/Linux-x86-64")
    endif()
endif()
