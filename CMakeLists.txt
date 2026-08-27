cmake_minimum_required(VERSION 3.22)

project(WaveformDisplay VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is expected either as a sibling checkout (../JUCE) or fetched from git.
# Override JUCE_PATH on the CMake command line if you have JUCE installed elsewhere:
#   cmake -B build -DJUCE_PATH=C:/SDKs/JUCE
if(DEFINED JUCE_PATH)
    add_subdirectory(${JUCE_PATH} JUCE)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
    add_subdirectory(../JUCE JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(WaveformDisplay
    PRODUCT_NAME "WaveformDisplay"
    COMPANY_NAME "StudioNexus"
)

juce_generate_juce_header(WaveformDisplay)

target_sources(WaveformDisplay
    PRIVATE
        Source/Main.cpp
        Source/MainComponent.cpp
        Source/AudioEngine.cpp
        Source/WaveformComponent.cpp
        Source/ParametersPanel.cpp
)

target_compile_definitions(WaveformDisplay
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:WaveformDisplay,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:WaveformDisplay,JUCE_VERSION>"
)

target_link_libraries(WaveformDisplay
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
