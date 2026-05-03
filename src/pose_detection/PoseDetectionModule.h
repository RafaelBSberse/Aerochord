#pragma once

#include "common/LockFreeQueue.h"
#include "common/PipelineTypes.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace aerochord {

// =============================================================================
// PoseDetectionModule — Módulo 2: Detecção de Pose de Mãos
//
// Responsabilidades:
//   - Consumir VideoFrames da fila de entrada
//   - Executar MediaPipe Hand Landmarker em cada quadro
//   - Aplicar filtro de suavização (média móvel ponderada) sobre os landmarks
//   - Publicar HandLandmarks na fila de saída (lock-free, SPSC)
//
// Suavização:
//   Cada coordenada x/y/z é filtrada por média móvel exponencial (EMA):
//     smoothed = alpha * raw + (1 - alpha) * prev_smoothed
//   onde alpha = config_.smoothingAlpha (padrão 0.5).
//
// Fallback MediaPipe (AEROCHORD_MEDIAPIPE_STUB):
//   Quando MediaPipe não está disponível, o módulo gera landmarks zerados
//   para permitir compilação e teste do restante do pipeline.
// =============================================================================

using FrameQueue     = LockFreeQueue<VideoFrame,     16>;
using LandmarkQueue  = LockFreeQueue<HandLandmarks,  32>;

class PoseDetectionModule {
public:
    // -------------------------------------------------------------------------
    // Configuração
    // -------------------------------------------------------------------------
    struct Config {
        int         maxHands        = 2;    // número máximo de mãos detectadas simultaneamente
        float       minConfidence   = 0.5f; // threshold de confiança mínima para aceitar detecção
        float       smoothingAlpha  = 0.5f; // fator EMA de suavização [0,1]; 1 = sem suavização
        std::string modelPath = "assets/hand_landmarker.task"; // caminho para o modelo TFLite
        Config() = default;
    };

    // -------------------------------------------------------------------------
    // Construção / destruição
    // -------------------------------------------------------------------------
    explicit PoseDetectionModule(std::shared_ptr<FrameQueue>    inputQueue,
                                 std::shared_ptr<LandmarkQueue> outputQueue);
    explicit PoseDetectionModule(std::shared_ptr<FrameQueue>    inputQueue,
                                 std::shared_ptr<LandmarkQueue> outputQueue,
                                 Config config);
    ~PoseDetectionModule();

    PoseDetectionModule(const PoseDetectionModule&)            = delete;
    PoseDetectionModule& operator=(const PoseDetectionModule&) = delete;

    // -------------------------------------------------------------------------
    // Ciclo de vida
    // -------------------------------------------------------------------------
    bool start();
    void stop();
    bool isRunning() const;

    // -------------------------------------------------------------------------
    // Fila de visualização — opcional; conectar antes de start()
    // -------------------------------------------------------------------------
    void setVizQueue(std::shared_ptr<VizQueue> vizQueue);

    // -------------------------------------------------------------------------
    // Diagnóstico
    // -------------------------------------------------------------------------
    uint64_t framesProcessed() const;
    uint64_t detectionFailures() const;
    uint64_t landmarksDetected() const;

private:
    void detectionLoop();

    // Aplica filtro EMA sobre um único HandLandmarks
    HandLandmarks smooth(const HandLandmarks& raw, const HandLandmarks& prev) const;

    std::shared_ptr<FrameQueue>    inputQueue_;
    std::shared_ptr<LandmarkQueue> outputQueue_;
    std::shared_ptr<VizQueue>      vizQueue_;   // nullptr = sem visualização
    Config config_;

    std::thread       thread_;
    std::atomic<bool> running_{ false };

    std::atomic<uint64_t> framesProcessed_{ 0 };
    std::atomic<uint64_t> detectionFailures_{ 0 };
    std::atomic<uint64_t> landmarksDetected_{ 0 };

    // Limiar de distância² (coords normalizadas) para matching por proximidade.
    // Acima deste valor, proximidade é descartada e o raw label do MediaPipe é usado.
    static constexpr float kMaxSlotDistSq = 0.04f;

    // Rastreia se cada slot estava ativo no frame anterior (evita EMA com prev stale)
    bool prevSlotActive_[2] = {false, false};
    bool currSlotActive_[2] = {false, false};

    // Último conjunto de landmarks detectados (por índice de mão 0/1)
    HandLandmarks prevLandmarks_[2]{};

    // Buffer reutilizável para conversão BGR→RGB (evita alocação por frame)
    std::vector<uint8_t> rgbBuffer_;

    // Ponteiro opaco para o handle do MediaPipe
    struct MediaPipeHandle;
    std::unique_ptr<MediaPipeHandle> mediapipe_;
};

} // namespace aerochord
