# Step 20 (Plugin Isolation & Thread Sanitization) directive: a trivial,
# guaranteed-inert VST3 built from JUCE's own bundled AudioPlugin example
# template, used ONLY to isolate whether the Time-Slip Engine child-process
# hang is specific to Rift Filter Lite's own DSP or a bug in this codebase's
# own lookahead IPC/threading plumbing. See PluginProcessor.h's own doc
# comment. JUCE itself is already added via add_subdirectory(modules/JUCE) in
# the top-level CMakeLists.txt, so this file does NOT re-add it.

juce_add_plugin(CrateBulletproofTest
    BUNDLE_ID "com.thecrate.cratebulletprooftest"
    COMPANY_NAME "The Crate"
    PLUGIN_MANUFACTURER_CODE Crat
    PLUGIN_CODE Bltp
    FORMATS VST3
    PRODUCT_NAME "Crate Bulletproof Test"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(CrateBulletproofTest
    PRIVATE
        PluginEditor.cpp
        PluginProcessor.cpp)

target_compile_definitions(CrateBulletproofTest
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(CrateBulletproofTest
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
