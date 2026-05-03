#pragma once

#include "common/LockFreeQueue.h"
#include "common/PipelineTypes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace aerochord {

// =============================================================================
// GestureAnalysisModule — Módulo 3: Análise de Gestos (FSM)
//
// Responsabilidades:
//   - Consumir HandLandmarks da fila de entrada
//   - Calcular métricas de gesto (distâncias, velocidades, ângulos)
//   - Executar FSM para reconhecer gestos estáticos e dinâmicos
//   - Publicar GestureEvents na fila de saída (lock-free, SPSC)
//
// FSM — Estados principais:
//   IDLE              → nenhum gesto ativo
//   RIGHT_PINCH_START → polegar + indicador se aproximando
//   RIGHT_PINCH_HOLD  → pinça mantida (sustentação de nota)
//   LEFT_CONTROL      → mão esquerda em modo de controle (volume / oitava)
//
// Métricas utilizadas:
//   - distância euclidiana 3D entre landmarks (pinça, abertura)
//   - velocidade de variação da distância de pinça (velocity de nota)
//   - inclinação da mão (pitch bend): ângulo entre WRIST e MIDDLE_FINGER_MCP
//   - distância MIDDLE_FINGER_TIP → palma (timbre/filtro)
//   - amplitude de oscilação lateral (vibrato)
// =============================================================================

using LandmarkQueue = LockFreeQueue<HandLandmarks, 32>;
using GestureQueue  = LockFreeQueue<GestureEvent,  64>;

// Identificadores de estado da FSM (mão direita — mão esquerda é stateless)
enum class FsmState {
    IDLE,
    RIGHT_PINCH_START,
    RIGHT_PINCH_HOLD,
};

class GestureAnalysisModule {
public:
    // -------------------------------------------------------------------------
    // Configuração
    // -------------------------------------------------------------------------
    struct Config {
        // Limiar de distância normalizada para considerar pinça fechada
        float pinchThreshold      = 0.05f;
        // Limiar de distância para considerar pinça "abrindo" (histerese)
        float pinchReleaseMargin  = 0.02f;
        // Número mínimo de frames consecutivos para confirmar transição de estado
        int   debounceFrames      = 3;
        // Janela de tempo (ms) para calcular velocidade de fechamento
        int   velocityWindowMs    = 50;
        // Delta mínimo de oscilação lateral para detectar vibrato (evita ruído)
        float vibratoThreshold    = 0.018f;
        // Confiança mínima do MediaPipe para aceitar landmarks
        float minConfidence       = 0.3f;
        Config() = default;
    };

    // -------------------------------------------------------------------------
    // Construção / destruição
    // -------------------------------------------------------------------------
    explicit GestureAnalysisModule(std::shared_ptr<LandmarkQueue> inputQueue,
                                   std::shared_ptr<GestureQueue>  outputQueue);
    explicit GestureAnalysisModule(std::shared_ptr<LandmarkQueue> inputQueue,
                                   std::shared_ptr<GestureQueue>  outputQueue,
                                   Config config);
    ~GestureAnalysisModule();

    GestureAnalysisModule(const GestureAnalysisModule&)            = delete;
    GestureAnalysisModule& operator=(const GestureAnalysisModule&) = delete;

    // -------------------------------------------------------------------------
    // Ciclo de vida
    // -------------------------------------------------------------------------
    bool start();
    void stop();
    bool isRunning() const;

    // -------------------------------------------------------------------------
    // Diagnóstico
    // -------------------------------------------------------------------------
    FsmState currentState() const;
    uint64_t gesturesEmitted() const;

private:
    void analysisLoop();

    // Cálculos de métricas sobre landmarks
    static float pinchDistance(const HandLandmarks& lm);
    static float middleFingerPalmDistance(const HandLandmarks& lm);
    static float handTiltAngle(const HandLandmarks& lm);       // pitch bend
    static float lateralOscillation(const HandLandmarks& lm);  // vibrato
    static float verticalPosition(const HandLandmarks& lm);    // pulso — usado pela mão esquerda
    static float pinchVerticalPosition(const HandLandmarks& lm); // ponto médio polegar/indicador — zona de nota

    // Velocity via janela temporal
    float computePinchVelocity(std::chrono::steady_clock::time_point now) const;
    void  recordPinchSample(float dist, std::chrono::steady_clock::time_point time);

    // Transições de FSM
    void processRightHand(const HandLandmarks& lm);
    void processLeftHand(const HandLandmarks& lm);

    std::shared_ptr<LandmarkQueue> inputQueue_;
    std::shared_ptr<GestureQueue>  outputQueue_;
    Config config_;

    std::thread       thread_;
    std::atomic<bool> running_{ false };

    // Estado da FSM (acessado apenas pela thread de análise — sem atomic)
    FsmState fsmState_{ FsmState::IDLE };
    int      debounceCounter_{ 0 };

    float prevPinchDist_{ 1.0f };  // frame anterior — usado para calcular velocity

    // Janela temporal para cálculo de velocity (velocidade de fechamento da pinça)
    struct PinchSample {
        float dist;
        std::chrono::steady_clock::time_point time;
    };
    static constexpr size_t kVelocityWindowMax = 8;
    std::array<PinchSample, kVelocityWindowMax> velocityWindow_{};
    size_t velocityWindowCount_{ 0 };

    // Contador de frames consecutivos de baixa confiança (fallback de detecção)
    int lowConfidenceStreak_{ 0 };
    static constexpr int kMaxLowConfidenceFrames = 5;  // frames tolerados antes de resetar

    std::atomic<uint64_t> gesturesEmitted_{ 0 };

    // Timeout para detecção perdida — se nenhum landmark da mão direita chega
    // dentro deste intervalo enquanto há nota ativa, emitir release automático.
    std::chrono::steady_clock::time_point lastRightHandTime_{};
    static constexpr int kHandLostTimeoutMs = 200;  // ~6 frames a 30 FPS

    // Rastreamento de posição lateral (RIGHT_VIBRATO)
    float prevLateralX_{ 0.5f };

    // Rastreamento de direção vertical (LEFT_OCTAVE_SELECT)
    float prevLeftY_{ 0.5f };
    int   leftOctaveDebounce_{ 0 };
    static constexpr float kOctaveVelocityThreshold = 0.015f;  // deslocamento mínimo/frame

    // Rastreamento de sustain (LEFT_SUSTAIN)
    int   leftStationaryCount_{ 0 };       // frames consecutivos com mão estacionária
    bool  sustainActive_{ false };         // estado atual do sustain
    static constexpr int   kSustainFrames = 15;     // frames para ativar sustain (~0.5s a 30fps)
    static constexpr float kStationaryThreshold = 0.008f; // delta Y máximo para "parado"
};

} // namespace aerochord
