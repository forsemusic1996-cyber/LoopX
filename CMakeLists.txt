cmake_minimum_required(VERSION 3.22)

project(MiniSampler VERSION 0.1.0)

set(JUCE_DIR "" CACHE PATH "Path to the JUCE source tree")

if(NOT JUCE_DIR AND DEFINED ENV{JUCE_DIR})
    set(JUCE_DIR "$ENV{JUCE_DIR}")
endif()

if(NOT JUCE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
endif()

if(NOT JUCE_DIR OR NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE was not found. Configure with -DJUCE_DIR=D:/CppLibrary/JUCE "
        "or place JUCE in the project/JUCE directory.")
endif()

add_subdirectory("${JUCE_DIR}" JUCE)

juce_add_plugin(MiniSampler
    COMPANY_NAME "Codex Test"
    PLUGIN_MANUFACTURER_CODE CdxT
    PLUGIN_CODE MnSm
    FORMATS VST3 Standalone
    PRODUCT_NAME "Mini Sampler"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE)

juce_generate_juce_header(MiniSampler)

target_sources(MiniSampler
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

target_compile_definitions(MiniSampler
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MiniSampler
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if (WIN32)
    target_link_libraries(MiniSampler PRIVATE dbghelp)
endif()
