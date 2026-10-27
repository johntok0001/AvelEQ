cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
endif()

project(AvelEQ VERSION 2.0.0 LANGUAGES C CXX)

# Windows: static C++ runtime, no VC++ redistributable needed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_binary_data(AvelFonts SOURCES
    Resources/Fonts/Inter-Regular.otf
    Resources/Fonts/Inter-Medium.otf
    Resources/Fonts/Inter-SemiBold.otf)

set(AVEL_FORMATS VST3)
if(APPLE)
    list(APPEND AVEL_FORMATS AU)
endif()

juce_add_plugin(AvelEQ
    COMPANY_NAME "Avel"
    BUNDLE_ID "com.avel.eq"
    PLUGIN_MANUFACTURER_CODE Avel
    PLUGIN_CODE AvEq
    FORMATS ${AVEL_FORMATS}
    PRODUCT_NAME "Avel EQ"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect)

set(AVEL_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/UI/EQGraph.cpp)

set(AVEL_DEFINES
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_MP3AUDIOFORMAT=1)

if(UNIX AND NOT APPLE)
    list(APPEND AVEL_DEFINES JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0 JUCE_USE_XRENDER=0)
endif()

set(AVEL_MODULES
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_dsp
    juce::juce_gui_basics)

target_sources(AvelEQ PRIVATE ${AVEL_SOURCES})
target_compile_definitions(AvelEQ PUBLIC ${AVEL_DEFINES})
target_link_libraries(AvelEQ
    PRIVATE AvelFonts ${AVEL_MODULES}
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

option(AVEL_BUILD_TESTS "Build the offline test harness" OFF)
if(AVEL_BUILD_TESTS)
    juce_add_console_app(AvelTest PRODUCT_NAME "AvelTest")
    target_sources(AvelTest PRIVATE Tests/AvelTest.cpp ${AVEL_SOURCES})
    target_include_directories(AvelTest PRIVATE Source)
    target_compile_definitions(AvelTest PRIVATE ${AVEL_DEFINES} JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(AvelTest PRIVATE AvelFonts ${AVEL_MODULES} juce::juce_recommended_config_flags)

    juce_add_console_app(HostTest PRODUCT_NAME "HostTest")
    target_sources(HostTest PRIVATE Tests/HostTest.cpp)
    target_compile_definitions(HostTest PRIVATE ${AVEL_DEFINES} JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(HostTest PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)
endif()
