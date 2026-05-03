cmake_minimum_required(VERSION 3.22)
project(Aerochord VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Padrões globais
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Coloca binários e libs no mesmo lugar para facilitar debugging
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------------------------------------------------------------------------
# Dependências externas (JUCE e MediaPipe via FetchContent)
# ---------------------------------------------------------------------------
include(cmake/FetchDependencies.cmake)

# ---------------------------------------------------------------------------
# Módulos internos (cada um gera uma static library)
# ---------------------------------------------------------------------------
add_subdirectory(src/common)
add_subdirectory(src/capture)
add_subdirectory(src/pose_detection)
add_subdirectory(src/gesture_analysis)
add_subdirectory(src/mapping)
add_subdirectory(src/midi_generation)

# ---------------------------------------------------------------------------
# VisualizationModule (opcional — requer OpenCV)
# ---------------------------------------------------------------------------
option(AEROCHORD_ENABLE_VISUALIZATION "Compilar VisualizationModule (requer OpenCV)" ON)

if(AEROCHORD_ENABLE_VISUALIZATION AND OpenCV_FOUND)
    add_subdirectory(src/visualization)
endif()

# ---------------------------------------------------------------------------
# Executável principal — app de console JUCE
# ---------------------------------------------------------------------------
juce_add_console_app(aerochord
    PRODUCT_NAME    "Aerochord"
    COMPANY_NAME    "TCC-EngSoftware"
    VERSION         "0.1.0"
)

target_sources(aerochord PRIVATE src/main.cpp)

target_compile_definitions(aerochord
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_link_libraries(aerochord
    PRIVATE
        aerochord_common
        aerochord_capture
        aerochord_pose_detection
        aerochord_gesture_analysis
        aerochord_mapping
        aerochord_midi_generation
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(AEROCHORD_ENABLE_VISUALIZATION AND OpenCV_FOUND)
    target_link_libraries(aerochord PRIVATE aerochord_visualization)
    target_compile_definitions(aerochord PRIVATE AEROCHORD_VISUALIZATION_ENABLED=1)
endif()

# ---------------------------------------------------------------------------
# Testes (opcional — habilitar com -DAEROCHORD_BUILD_TESTS=ON)
# ---------------------------------------------------------------------------
option(AEROCHORD_BUILD_TESTS "Build unit tests" OFF)
if(AEROCHORD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
