cmake_minimum_required(VERSION 3.22)

project(LoopX VERSION 0.4.1)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(JUCE_DIR "" CACHE PATH "Path to the JUCE source tree")

if(NOT JUCE_DIR AND DEFINED ENV{JUCE_DIR})
    set(JUCE_DIR "$ENV{JUCE_DIR}")
endif()

if(NOT JUCE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
endif()

if(NOT JUCE_DIR OR NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE was not found. Configure with -DJUCE_DIR=<path-to-JUCE> "
        "or place JUCE in the project/JUCE directory.")
endif()

add_subdirectory("${JUCE_DIR}" JUCE)

# 1.1.0 is a fixed upstream release, header-only, with its DSP dependencies
# vendored in dsp/. It predates upstream's CMake target, so provide an adapter.
include(FetchContent)
FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 44c8f865af9da8c29cc4a70a2d5a3ec83639c711
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(signalsmith_stretch)
add_library(signalsmith_stretch_headers INTERFACE)
target_include_directories(signalsmith_stretch_headers INTERFACE ${signalsmith_stretch_SOURCE_DIR})

juce_add_plugin(LoopX
    COMPANY_NAME "Codex Test"
    BUNDLE_ID "com.forsemusic.LoopX"
    PLUGIN_MANUFACTURER_CODE CdxT
    # Keep the existing host identity across the product rename.
    PLUGIN_CODE MnSm
    FORMATS VST3 Standalone
    PRODUCT_NAME "LoopX"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE)

juce_generate_juce_header(LoopX)

target_sources(LoopX
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

target_compile_definitions(LoopX
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(LoopX
    PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
        signalsmith_stretch_headers
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Exercise real JUCE drop dispatch, engine synchronization, state restore and
# resize/audio concurrency on the same Windows runner that builds the VST3.
juce_add_console_app(LoopXChecks PRODUCT_NAME "LoopXChecks")
juce_generate_juce_header(LoopXChecks)
target_sources(LoopXChecks PRIVATE Source/SamplerChecks.cpp)
target_include_directories(LoopXChecks PRIVATE Source)
target_compile_definitions(LoopXChecks PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(LoopXChecks PRIVATE LoopX juce::juce_audio_utils juce::juce_gui_extra signalsmith_stretch_headers)
# Keep both upstream MIT notices in every build artifact.
configure_file(${signalsmith_stretch_SOURCE_DIR}/LICENSE.txt ${CMAKE_BINARY_DIR}/Signalsmith-Stretch-LICENSE.txt COPYONLY)
configure_file(${signalsmith_stretch_SOURCE_DIR}/dsp/LICENSE.txt ${CMAKE_BINARY_DIR}/Signalsmith-DSP-LICENSE.txt COPYONLY)
enable_testing()
add_test(NAME LoopXChecks COMMAND LoopXChecks)
set_tests_properties(LoopXChecks PROPERTIES TIMEOUT 90)
add_executable(EngineChecks Source/EngineChecks.cpp)
target_compile_features(EngineChecks PRIVATE cxx_std_17)
target_link_libraries(EngineChecks PRIVATE signalsmith_stretch_headers)
add_test(NAME EngineChecks COMMAND EngineChecks)
