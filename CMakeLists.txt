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

# Step 20 (Plugin Isolation & Thread Sanitization) directive: a trivial,
# guaranteed-inert VST3 used only to isolate whether the Time-Slip Engine
# child-process hang is specific to Rift Filter Lite's own DSP, or a bug in
# this codebase's own lookahead IPC/threading plumbing — see
# Source/TestPlugins/CrateBulletproofTest/PluginProcessor.h's own doc comment.
add_subdirectory(Source/TestPlugins/CrateBulletproofTest)

# ---------------------------------------------------------------------------
# Anticipative FX (Shadow Freezing) directive — moodycamel::ReaderWriterQueue,
# the single-producer/single-consumer wait-free ring buffer CrateAnticipativeWrapper
# will use to hand pre-rendered background-thread audio blocks to the
# real-time callback without a lock. Header-only, pulled via FetchContent
# rather than vendored as a git submodule (it's a single header, not a build
# of its own) — CMake fetches/configures it once and caches it under
# build/_deps, same as any other FetchContent dependency.
include(FetchContent)
FetchContent_Declare(
    readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG v1.0.6
)
FetchContent_MakeAvailable(readerwriterqueue)

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
    Source/CrateStressTest.cpp
    Source/Engine/CrateAnticipativeWrapper.cpp
    Source/Engine/CrateSandboxBridge.cpp
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
    Source/UI/FlatGridComboLookAndFeel.cpp
    Source/UI/CrateValueBox.cpp
    Source/UI/CrateFoldArrow.cpp
    Source/UI/PluginSlotComponent.cpp
    Source/UI/PluginWindow.cpp
    Source/UI/PluginWindowHeader.cpp
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
    readerwriterqueue
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_opengl
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

# ---------------------------------------------------------------------------
# Plugin Sandboxing Step 5 (The Headless IPC Host Skeleton) directive:
# CrateSandbox is the out-of-process plugin host — a SEPARATE, headless
# console executable (juce_add_console_app, not juce_add_gui_app), built
# side-by-side with TheCrateStudio from this same CMake configure. It links
# tracktion::tracktion_engine (per directive) but deliberately none of this
# app's own UI sources or the GUI-only JUCE modules TheCrateStudio links
# (juce_gui_extra, juce_opengl, TheCrateStudioAssets) — it never creates a
# window. Note: tracktion::tracktion_engine's own module dependencies may
# still transitively pull in juce_gui_basics (TE ships plugin-editor-hosting
# code that depends on it even when unused) — that's unavoidable when
# linking TE at all and does not affect this target's headless behaviour,
# since juce_add_console_app builds it with the console subsystem and no
# window is ever instantiated.
# ---------------------------------------------------------------------------
juce_add_console_app(CrateSandbox
    BUNDLE_ID "com.thecrate.cratesandbox"
    PRODUCT_NAME "Crate Sandbox"
    COMPANY_NAME "The Crate"
)

juce_generate_juce_header(CrateSandbox)

target_sources(CrateSandbox PRIVATE
    Source/Sandbox/Main.cpp
)

target_compile_definitions(CrateSandbox PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    TRACKTION_UNIT_TESTS=0
)

target_link_libraries(CrateSandbox PRIVATE
    tracktion::tracktion_engine
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Step 51 (Silent Crash Hardening) directive: /EHa (asynchronous/structured
# exception handling), scoped ONLY to CrateSandbox — the untrusted,
# already-disposable third-party plugin host process this whole sandbox
# architecture exists to contain. Deliberately NOT applied to TheCrateStudio
# (the main app): MSVC's default /EHsc model only catches C++ exceptions —
# a __try/__except(EXCEPTION_EXECUTE_HANDLER) block around a raw hardware
# fault (an access violation inside third-party VST3 code, e.g. during
# getStateInformation()/setStateInformation()) does NOT actually catch
# anything under /EHsc; it silently does nothing, giving a false sense of
# safety. /EHa is what makes that catch real. Scoping it to this one target
# keeps the change low-risk (a different exception model only affects this
# already-isolated process, never the main app's own codegen/performance).
if (MSVC)
    target_compile_options(CrateSandbox PRIVATE /EHa)
endif()
