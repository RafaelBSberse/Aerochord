add_library(aerochord_midi_generation STATIC
    MidiGenerationModule.cpp
)

target_link_libraries(aerochord_midi_generation
    PUBLIC
        aerochord_common
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_core
)

# ALSA — necessário para transporte UMP MIDI 2.0 no Linux
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA QUIET alsa)
    endif()

    if(ALSA_FOUND)
        target_include_directories(aerochord_midi_generation PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_libraries(aerochord_midi_generation PRIVATE ${ALSA_LIBRARIES})
        message(STATUS "ALSA ${ALSA_VERSION} encontrado — suporte UMP MIDI 2.0 habilitado.")
    else()
        message(STATUS "ALSA não encontrado — UMP MIDI 2.0 desabilitado; usando MIDI 1.0 via JUCE.")
    endif()
endif()
