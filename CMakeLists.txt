cmake_minimum_required(VERSION 3.22)

project(TheCrateStudio VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Dependencies (vendored as git submodules under modules/)
#   git submodule add https://github.com/juce-framework/JUCE.git modules/JUCE
#   git submodule add https://github.com/Tracktion/tracktion_engine.git modules/tracktion_engine
#   git submodule update --init --recursive
# ---------------------------------------------------------------------------
add_subdirectory(modules/JUCE)
add_subdirectory(modules/tracktion_engine)

juce_add_gui_app(TheCrateStudio
    BUNDLE_ID "com.thecrate.thecratestudio"
    PRODUCT_NAME "The Crate Studio"
    COMPANY_NAME "The Crate"
    ICON_BIG ""
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE
)

juce_generate_juce_header(TheCrateStudio)

# ---------------------------------------------------------------------------
# Binary assets (Assets/) — photorealistic hardware PNGs etc., compiled into a
# BinaryData.h/.cpp pair via JUCE's own binary-builder, NOT loaded from disk at
# runtime (so the .exe stays single-file/portable). Add new asset files to the
# SOURCES list below; each one becomes BinaryData::<name>_<ext> +
# BinaryData::<name>_<ext>Size automatically (dots in the filename become
# underscores, e.g. fader_cap.png -> BinaryData::fader_cap_png).
# ---------------------------------------------------------------------------
juce_add_binary_data(TheCrateStudioAssets
    SOURCES
        Assets/fader_cap.png
        Assets/pan_knob.png
)

target_sources(TheCrateStudio PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/CrateWorkflowManager.cpp
    Source/UI/TransportBar.cpp
    Source/UI/TimelineComponent.cpp
    Source/UI/MixerComponent.cpp
    Source/UI/MixerStrip.cpp
    Source/UI/MasterStrip.cpp
    Source/UI/InsertsRackComponent.cpp
    Source/UI/UniversalDeviceChainComponent.cpp
    Source/UI/SettingsComponent.cpp
    Source/UI/PluginBrowserComponent.cpp
    Source/UI/BrowserComponent.cpp
    Source/UI/CrateMixerLookAndFeel.cpp
    Source/UI/HardwareSlotLookAndFeel.cpp
    Source/UI/GhostButtonLookAndFeel.cpp
    Source/UI/PluginSlotComponent.cpp
    Source/UI/PluginWindow.cpp
    Source/UI/TheCrateLookAndFeel.cpp
    Source/UI/AutomationLaneComponent.cpp
    Source/UI/TrackHeaderComponent.cpp
    Source/UI/ArrangementComponent.cpp
    Source/UI/TimeRulerComponent.cpp
    Source/UI/CratePianoRollComponent.cpp
    Source/UI/CrateMidiInspectorComponent.cpp
    Source/UI/PianoRollExpressionLane.cpp
    Source/UI/PianoRollArticulationLane.cpp
    Source/UI/CrateTrackInspectorComponent.cpp
)

target_compile_definitions(TheCrateStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_ASIO=1
    JUCE_DIRECTSOUND=1
    JUCE_WASAPI=1
    JUCE_ALSA=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    TRACKTION_UNIT_TESTS=0
)

target_link_libraries(TheCrateStudio PRIVATE
    TheCrateStudioAssets
    tracktion::tracktion_engine
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Enable ASIO and link the SDK
# Enable ASIO and link the SDK headers directly
target_compile_definitions(TheCrateStudio PUBLIC JUCE_ASIO=1)
target_include_directories(TheCrateStudio PRIVATE "C:/SDKs/ASIOSDK/common")
target_include_directories(TheCrateStudio PRIVATE "C:/SDKs/ASIOSDK/host")
target_include_directories(TheCrateStudio PRIVATE "C:/SDKs/ASIOSDK/host/pc")
