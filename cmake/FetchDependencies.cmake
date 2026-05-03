include(FetchContent)

# ---------------------------------------------------------------------------
# JUCE — framework de áudio e MIDI multiplataforma
# ---------------------------------------------------------------------------
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.7        # Atualize para a versão mais recente estável
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# MediaPipe — detecção de landmarks de mãos
#
# MediaPipe não possui integração CMake oficial no modelo FetchContent.
# Opções de integração (escolher uma):
#
# Opção A — Biblioteca pré-compilada (recomendada para início rápido):
#   Baixar os binários do MediaPipe C++ Tasks de:
#   https://developers.google.com/mediapipe/solutions/vision/hand_landmarker
#   e definir MEDIAPIPE_ROOT apontando para o diretório descompactado.
#
# Opção B — Build a partir do código-fonte (via Bazel):
#   git clone https://github.com/google/mediapipe.git
#   Seguir guia oficial: https://google.github.io/mediapipe/getting_started/install.html
#   Expor os cabeçalhos e a biblioteca como target CMake importado abaixo.
#
# ---------------------------------------------------------------------------

# Caminho para MediaPipe definido externamente ou via variável de ambiente
set(MEDIAPIPE_ROOT "" CACHE PATH "Diretório raiz da instalação do MediaPipe C++")

if(MEDIAPIPE_ROOT AND EXISTS "${MEDIAPIPE_ROOT}")
    message(STATUS "MediaPipe encontrado em: ${MEDIAPIPE_ROOT}")

    # Detectar automaticamente se é .so ou .a
    if(EXISTS "${MEDIAPIPE_ROOT}/lib/libmediapipe.so")
        set(_MP_LIB "${MEDIAPIPE_ROOT}/lib/libmediapipe.so")
        add_library(mediapipe SHARED IMPORTED GLOBAL)
    elseif(EXISTS "${MEDIAPIPE_ROOT}/lib/libmediapipe_tasks_vision.a")
        set(_MP_LIB "${MEDIAPIPE_ROOT}/lib/libmediapipe_tasks_vision.a")
        add_library(mediapipe STATIC IMPORTED GLOBAL)
    else()
        message(FATAL_ERROR "MediaPipe: nenhuma biblioteca encontrada em ${MEDIAPIPE_ROOT}/lib/")
    endif()

    set_target_properties(mediapipe PROPERTIES
        IMPORTED_LOCATION             "${_MP_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${MEDIAPIPE_ROOT}/include"
    )
    message(STATUS "MediaPipe library: ${_MP_LIB}")
else()
    message(WARNING
        "MediaPipe não encontrado. "
        "Defina -DMEDIAPIPE_ROOT=<path> ao chamar o CMake. "
        "O módulo de Detecção de Pose usará um stub no lugar."
    )

    # Cria um target vazio para que o restante do projeto compile sem MediaPipe
    add_library(mediapipe INTERFACE)
    target_compile_definitions(mediapipe INTERFACE AEROCHORD_MEDIAPIPE_STUB=1)
endif()

# ---------------------------------------------------------------------------
# ALSA — transporte UMP MIDI 2.0 (Linux)
# Instalar: sudo apt install libasound2-dev
# Requer kernel ≥ 6.5 e alsa-lib ≥ 1.2.10 para suporte UMP nativo.
# Se não encontrado, o módulo de geração MIDI usa fallback MIDI 1.0 via JUCE.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA QUIET alsa)
        if(ALSA_FOUND)
            message(STATUS "ALSA ${ALSA_VERSION} encontrado — suporte UMP habilitado.")
        else()
            message(STATUS "ALSA não encontrado. Para UMP MIDI 2.0: sudo apt install libasound2-dev")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# OpenCV — VisualizationModule (cv::imshow, cv::Mat, cv::circle, cv::line)
# Instalar: sudo apt install libopencv-dev
# ---------------------------------------------------------------------------
find_package(OpenCV QUIET COMPONENTS core highgui imgproc)

if(OpenCV_FOUND)
    message(STATUS "OpenCV ${OpenCV_VERSION} encontrado em ${OpenCV_DIR}")
else()
    message(STATUS
        "OpenCV não encontrado — VisualizationModule desativado. "
        "Para ativar: sudo apt install libopencv-dev")
endif()
