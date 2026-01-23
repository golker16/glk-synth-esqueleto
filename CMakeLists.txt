cmake_minimum_required(VERSION 3.15)
project(BasicInstrument VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- JUCE ---
FetchContent_Declare(
  juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.10
)
FetchContent_MakeAvailable(juce)

# --- Plugin target (VST3) ---
juce_add_plugin(BasicInstrument
  COMPANY_NAME "YourCompany"
  PRODUCT_NAME "BasicInstrument"

  # IDs únicos (cámbialos)
  PLUGIN_MANUFACTURER_CODE Yc01
  PLUGIN_CODE BsI1

  FORMATS VST3
  VST3_CATEGORIES "Instrument" "Synth"

  IS_SYNTH TRUE
  IS_MIDI_EFFECT FALSE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE

  COPY_PLUGIN_AFTER_BUILD FALSE
  VST3_AUTO_MANIFEST TRUE
)

juce_generate_juce_header(BasicInstrument)

# --- Embed assets (fuente) ---
# Asegúrate de tener: assets/mi_fuente.ttf
juce_add_binary_data(BasicInstrumentAssets
  SOURCES
    assets/mi_fuente.ttf
)

target_sources(BasicInstrument
  PRIVATE
    src/PluginProcessor.cpp
    src/PluginProcessor.h
)

target_link_libraries(BasicInstrument
  PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    BasicInstrumentAssets
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
