#pragma once

#include "common/LockFreeQueue.h"
#include "common/PipelineTypes.h"

#include <atomic>
#include <memory>
#include <thread>

namespace aerochord {

// =============================================================================
// CaptureModule — Módulo 1: Captura de Quadros da Webcam
//
// Responsabilidades:
//   - Abrir o dispositivo de câmera (índice configurável)
//   - Ler quadros em loop em thread dedicada
//   - Aplicar buffering circular interno
//   - Publicar VideoFrames na fila de saída (lock-free, SPSC)
//
// Abstração de backend (sem lógica aqui — ver implementação):
//   - Windows : DirectShow / Media Foundation
//   - macOS   : AVFoundation
//   - Linux   : V4L2
//
// Uso:
//   auto queue  = std::make_shared<LockFreeQueue<VideoFrame>>();
//   CaptureModule capture(queue, /*deviceIndex=*/0, /*fps=*/30);
//   capture.start();
//   // ... pipeline running ...
//   capture.stop();
// =============================================================================

using FrameQueue = LockFreeQueue<VideoFrame, 16>;

class CaptureModule {
public:
    // -------------------------------------------------------------------------
    // Configuração
    // -------------------------------------------------------------------------
    struct Config {
        int deviceIndex    = 0;     // índice da webcam (0 = padrão)
        int targetWidth    = 0;     // 0 = auto-detectar resolução máxima da câmera
        int targetHeight   = 0;     // 0 = auto-detectar resolução máxima da câmera
        int targetFps      = 0;     // 0 = auto-detectar FPS máximo da câmera
        int circularBuffer = 4;     // quadros no buffer circular interno
        Config() = default;
    };

    // -------------------------------------------------------------------------
    // Construção / destruição
    // -------------------------------------------------------------------------
    explicit CaptureModule(std::shared_ptr<FrameQueue> outputQueue);
    explicit CaptureModule(std::shared_ptr<FrameQueue> outputQueue, Config config);
    ~CaptureModule();

    // Não copiável
    CaptureModule(const CaptureModule&)            = delete;
    CaptureModule& operator=(const CaptureModule&) = delete;

    // -------------------------------------------------------------------------
    // Ciclo de vida
    // -------------------------------------------------------------------------
    bool start();   // Abre câmera e inicia thread de captura; retorna false em erro
    void stop();    // Sinaliza parada e aguarda thread encerrar (join)

    bool isRunning() const;

    // -------------------------------------------------------------------------
    // Diagnóstico
    // -------------------------------------------------------------------------
    uint64_t framesDropped() const;  // quadros descartados por fila cheia
    uint64_t framesCaptured() const;
    int      actualFps() const;     // FPS efetivo negociado com a câmera

private:
    void captureLoop();   // executado na thread dedicada
    bool openCamera();    // abre dispositivo e inicia streaming V4L2
    void closeCamera();   // encerra streaming e libera recursos

    std::shared_ptr<FrameQueue> outputQueue_;
    Config config_;

    std::thread       thread_;
    std::atomic<bool> running_{ false };

    std::atomic<uint64_t> framesDropped_{ 0 };
    std::atomic<uint64_t> framesCaptured_{ 0 };
    std::atomic<int>      actualFps_{ 30 };    // FPS efetivo (atualizado em openCamera)

    // Ponteiro opaco para o handle de câmera (backend específico de plataforma)
    struct CameraHandle;
    std::unique_ptr<CameraHandle> camera_;
};

} // namespace aerochord
