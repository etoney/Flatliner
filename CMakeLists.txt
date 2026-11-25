cmake_minimum_required(VERSION 3.22)
# Must come BEFORE project(), or CMake ignores them: universal (Apple Silicon + Intel), macOS 11+
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
project(FLATLINER VERSION 1.4.2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same JUCE version as Cutthroat, DREAD, Possessed and Dead Air.
# (Pass -DJUCE_LOCAL_DIR=/path/to/JUCE to build against a local checkout.)
if(JUCE_LOCAL_DIR)
    add_subdirectory(${JUCE_LOCAL_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ARA (Flatliner applied to the clip itself): pass -DFLATLINER_ARA_SDK=/path/to/ARA_SDK (releases/2.1.0).
# Without it Flatliner builds as a plain plugin (live mode only).
set(FLATLINER_ARA_SDK "" CACHE PATH "ARA SDK location (empty = no ARA)")
set(FLATLINER_ARA_ARGS "")
if(NOT FLATLINER_TESTS_ONLY AND FLATLINER_ARA_SDK)
    juce_set_ara_sdk_path("${FLATLINER_ARA_SDK}")
    set(FLATLINER_ARA_ARGS IS_ARA_EFFECT TRUE)
    message(STATUS "Flatliner: ARA on (${FLATLINER_ARA_SDK})")
endif()

if(NOT FLATLINER_TESTS_ONLY)
juce_add_plugin(Flatliner
    ${FLATLINER_ARA_ARGS}
    COMPANY_NAME "Castle Danger"
    PRODUCT_NAME "Flatliner"
    BUNDLE_ID com.castledanger.flatliner
    PLUGIN_MANUFACTURER_CODE Cdgr
    PLUGIN_CODE Flt1
    FORMATS VST3 AU
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Dynamics
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(Flatliner PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Flatliner PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Flatliner
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endif()

# Headless checks: DSP tests, preset round trip, and renders the editor to PNGs
juce_add_console_app(FlatlinerSnapshot PRODUCT_NAME "FlatlinerSnapshot")
target_sources(FlatlinerSnapshot PRIVATE test/snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_compile_definitions(FlatlinerSnapshot PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
    "JucePlugin_VersionString=\"${PROJECT_VERSION}\"")
target_link_libraries(FlatlinerSnapshot PRIVATE juce::juce_audio_processors juce::juce_gui_basics PUBLIC juce::juce_recommended_config_flags)

# A tiny ARA host that applies the built Flatliner to an in-memory take and checks the ride (ARA builds only)
if(NOT FLATLINER_TESTS_ONLY AND FLATLINER_ARA_SDK)
    juce_add_console_app(FlatlinerARAHost PRODUCT_NAME "FlatlinerARAHost")
    target_sources(FlatlinerARAHost PRIVATE test/ara_host.cpp)
    target_compile_definitions(FlatlinerARAHost PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_ARA=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(FlatlinerARAHost PRIVATE juce::juce_audio_processors juce::juce_audio_formats juce_ara_sdk
        PUBLIC juce::juce_recommended_config_flags)
endif()
