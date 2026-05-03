#pragma once

#include "common/LockFreeQueue.h"
#include "common/PipelineTypes.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace aerochord {

// =============================================================================
// VisualizationModule — Módulo 6: Visualização em Tempo Real
//
// Responsabilidades:
//   - Consumir VizFrames da fila de entrada (lock-free, SPSC)
//   - Renderizar frame BGR da câmera via cv::imshow em thread dedicada
//   - Sobrepor landmarks (21 círculos + 20 conexões de esqueleto) quando presentes
//   - Exibir HUD: FPS, modo MIDI, oitava corrente, nota ativa, frames descartados
//   - Encerrar ao receber tecla 'q' ou ESC (ou via stop())
//
// Restrição de thread:
//   cv::imshow / cv::waitKey DEVEM ser chamados sempre pela MESMA thread (OpenCV).
//   Todo acesso a APIs de GUI está confinado a vizLoop().
//
// Uso em main.cpp:
//   auto vizQueue = std::make_shared<VizQueue>();
//   VisualizationModule viz(vizQueue);
//   viz.setHudSource(&midiGen, &mapping);
//   viz.start();
//   // ... pipeline em execução ...
//   viz.stop();
// =============================================================================

// Forward declarations para stats do HUD — sem incluir os headers completos aqui
class MidiGenerationModule;
class MappingModule;

class VisualizationModule {
public:
    // -------------------------------------------------------------------------
    // Configuração
    // -------------------------------------------------------------------------
    struct Config {
        std::string windowName   = "Aerochord";
        int         displayFps   = 30;     // taxa de renderização (throttle)
        bool        drawSkeleton = true;   // desenhar conexões entre landmarks
        Config() = default;
    };

    // -------------------------------------------------------------------------
    // Construção / destruição
    // -------------------------------------------------------------------------
    explicit VisualizationModule(std::shared_ptr<VizQueue> inputQueue);
    explicit VisualizationModule(std::shared_ptr<VizQueue> inputQueue, Config config);
    ~VisualizationModule();

    VisualizationModule(const VisualizationModule&)            = delete;
    VisualizationModule& operator=(const VisualizationModule&) = delete;

    // -------------------------------------------------------------------------
    // Ciclo de vida
    // -------------------------------------------------------------------------
    bool start();
    void stop();
    bool isRunning() const;

    // -------------------------------------------------------------------------
    // HUD — ponteiros opcionais para leitura atômica de stats de outros módulos.
    // Chamar antes de start(). Ambos os ponteiros podem ser nullptr.
    // -------------------------------------------------------------------------
    void setHudSource(const MidiGenerationModule* midiGen,
                      const MappingModule*         mapping);

    // Ponteiro não-const para controles interativos (legato, program change).
    // Chamar antes de start().
    void setControlTarget(MappingModule* mapping);

    // -------------------------------------------------------------------------
    // Diagnóstico
    // -------------------------------------------------------------------------
    uint64_t framesRendered() const;
    uint64_t framesDropped()  const;

private:
    void vizLoop();

    std::shared_ptr<VizQueue>    inputQueue_;
    Config                       config_;
    const MidiGenerationModule*  midiGen_{ nullptr };
    const MappingModule*         mapping_{ nullptr };
    MappingModule*               controlTarget_{ nullptr };

    std::thread       thread_;
    std::atomic<bool> running_{ false };

    std::atomic<uint64_t> framesRendered_{ 0 };
    std::atomic<uint64_t> framesDropped_{ 0 };

    // Estado dos controles de UI (acessados pela vizLoop e pelo mouse callback)
    int  instrumentIdx_{ 0 };
    int  lastAppliedInstrument_{ -1 };
    int  frameWidth_{ 640 };
    int  frameHeight_{ 480 };

    // Callback estático de mouse — delega para instância via userdata
    static void onMouse(int event, int x, int y, int flags, void* userdata);
    void        handleClick(int x, int y);
};

} // namespace aerochord
