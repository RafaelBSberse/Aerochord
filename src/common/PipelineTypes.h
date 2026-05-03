#pragma once

#include "LockFreeQueue.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace aerochord {

// =============================================================================
// VideoFrame — saída do Módulo de Captura
// =============================================================================
struct VideoFrame {
    std::vector<uint8_t> data;     // pixels brutos em formato BGR
    int width    = 0;
    int height   = 0;
    int channels = 3;
    std::chrono::steady_clock::time_point timestamp;
};

// =============================================================================
// HandLandmarks — saída do Módulo de Detecção de Pose
// =============================================================================

// Índices dos 21 landmarks MediaPipe Hand (conforme documentação oficial)
enum class LandmarkIndex : size_t {
    WRIST             = 0,
    THUMB_CMC         = 1,
    THUMB_MCP         = 2,
    THUMB_IP          = 3,
    THUMB_TIP         = 4,
    INDEX_FINGER_MCP  = 5,
    INDEX_FINGER_PIP  = 6,
    INDEX_FINGER_DIP  = 7,
    INDEX_FINGER_TIP  = 8,
    MIDDLE_FINGER_MCP = 9,
    MIDDLE_FINGER_PIP = 10,
    MIDDLE_FINGER_DIP = 11,
    MIDDLE_FINGER_TIP = 12,
    RING_FINGER_MCP   = 13,
    RING_FINGER_PIP   = 14,
    RING_FINGER_DIP   = 15,
    RING_FINGER_TIP   = 16,
    PINKY_MCP         = 17,
    PINKY_PIP         = 18,
    PINKY_DIP         = 19,
    PINKY_TIP         = 20,
};

struct HandLandmark {
    float x          = 0.0f;  // coordenada normalizada [0, 1]
    float y          = 0.0f;  // coordenada normalizada [0, 1]
    float z          = 0.0f;  // profundidade relativa ao pulso
    float visibility = 0.0f;  // confiança de detecção [0, 1]
};

enum class HandLabel { LEFT, RIGHT, UNKNOWN };

struct HandLandmarks {
    static constexpr size_t NUM_LANDMARKS = 21;

    std::array<HandLandmark, NUM_LANDMARKS> landmarks{};
    HandLabel hand       = HandLabel::UNKNOWN;
    float     confidence = 0.0f;
    std::chrono::steady_clock::time_point timestamp;

    const HandLandmark& at(LandmarkIndex idx) const {
        return landmarks[static_cast<size_t>(idx)];
    }
};

// =============================================================================
// GestureEvent — saída do Módulo de Análise de Gestos
// =============================================================================

enum class GestureType {
    NONE,

    // --- Mão Direita ---
    RIGHT_PINCH_START,       // polegar + indicador iniciando fechamento → Note On iminente
    RIGHT_PINCH_RELEASE,     // pinça solta → Note Off

    // Modificadores contínuos com pinça ativa (mão direita)
    RIGHT_PITCH_BEND,        // inclinação frente/trás
    RIGHT_TIMBRE_CONTROL,    // distância dedo médio–palma
    RIGHT_VIBRATO,           // oscilação lateral leve
    RIGHT_PINCH_POSITION,    // posição vertical durante pinça ativa (legato zone tracking)

    // --- Mão Esquerda ---
    LEFT_OCTAVE_SELECT,      // pinça + movimento vertical → seleção de oitava
    LEFT_VOLUME_CONTROL,     // mão aberta + movimento vertical → volume global
    LEFT_SUSTAIN,            // mão aberta mantida parada → CC#64 sustain on/off
};

struct GestureEvent {
    GestureType type = GestureType::NONE;
    HandLabel   hand = HandLabel::UNKNOWN;

    // Valores contínuos normalizados [0, 1] ou [-1, 1] conforme o gesto
    float primaryValue   = 0.0f;  // altura vertical (pitch zone), posição de oitava, volume
    float secondaryValue = 0.0f;  // pitch bend, distância médio-palma
    float velocity       = 0.0f;  // velocidade de fechamento da pinça [0, 1]

    std::chrono::steady_clock::time_point timestamp;
};

// =============================================================================
// MidiCommand — saída do Módulo de Mapeamento
// =============================================================================

enum class MidiCommandType {
    NOTE_ON,
    NOTE_OFF,
    CONTROL_CHANGE,
    PITCH_BEND,
    PER_NOTE_PITCH_BEND,  // MIDI 2.0: pitch bend por nota individual (UMP MT4 Opcode 0x6)
    PROGRAM_CHANGE,
};

struct MidiCommand {
    MidiCommandType type   = MidiCommandType::NOTE_ON;
    uint8_t  channel       = 0;    // canal MIDI [0, 15]
    uint8_t  noteNumber    = 60;   // número de nota MIDI [0, 127]
    uint8_t  group         = 0;    // UMP group [0, 15] — cap 3: mãos em grupos distintos

    // Resolução MIDI 2.0 (32 bits); truncado para 7 bits no fallback MIDI 1.0
    uint32_t velocity      = 0;
    uint32_t controlNumber = 0;
    uint32_t controlValue  = 0;
    int32_t  pitchBend     = 0;    // MIDI 2.0: signed 32 bits; MIDI 1.0: [-8192, 8191]

    std::chrono::steady_clock::time_point timestamp;
};

// =============================================================================
// VizFrame — entrada do VisualizationModule
//
// Produzido por PoseDetectionModule após cada detecção.
// landmarks é nullopt quando MediaPipe não está disponível (stub ativo).
// =============================================================================

struct VizFrame {
    VideoFrame frame;  // BGR 640×480

    // Índice 0 = mão esquerda, índice 1 = mão direita.
    // nullopt quando a mão não foi detectada neste frame.
    std::array<std::optional<HandLandmarks>, 2> landmarks{};
};

using VizQueue = LockFreeQueue<VizFrame, 4>;

} // namespace aerochord
