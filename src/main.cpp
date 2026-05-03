#include <juce_core/juce_core.h>

#include "capture/CaptureModule.h"
#include "common/Logger.h"
#include "common/LockFreeQueue.h"
#include "common/PipelineTypes.h"
#include "gesture_analysis/GestureAnalysisModule.h"
#include "mapping/MappingModule.h"
#include "midi_generation/MidiGenerationModule.h"
#include "pose_detection/PoseDetectionModule.h"

#ifdef AEROCHORD_VISUALIZATION_ENABLED
#  include "visualization/VisualizationModule.h"
#endif

#include <csignal>
#include <atomic>
#include <iostream>
#include <memory>
#include <string>

#ifndef _WIN32
#  include <getopt.h>
#endif

// ---------------------------------------------------------------------------
// Sinal de parada global (SIGINT / SIGTERM)
// ---------------------------------------------------------------------------
static std::atomic<bool> g_running{ true };

static void signalHandler(int) {
    g_running.store(false);
}

// ---------------------------------------------------------------------------
// Argumentos de linha de comando
// ---------------------------------------------------------------------------
struct Args {
    int         deviceIndex = 0;       // índice V4L2 do dispositivo de câmera
    std::string midiOutName;           // vazio = primeiro dispositivo disponível
    bool        noViz  = false;        // desativar janela de visualização
    bool        eval   = false;        // modo de avaliação (CSV logging — reservado)
    int         width  = 0;            // 0 = auto-detectar resolução máxima
    int         height = 0;            // 0 = auto-detectar resolução máxima
    int         fps    = 0;            // 0 = auto-detectar FPS
};

static Args parseArgs(int argc, char* argv[]) {
    Args args;

#ifdef _WIN32
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--no-viz") {
            args.noViz = true;
        } else if (a == "--eval") {
            args.eval = true;
        } else if (a == "--device" && i + 1 < argc) {
            try { args.deviceIndex = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "[main] --device: valor inválido\n"; }
        } else if (a == "--midi-out" && i + 1 < argc) {
            args.midiOutName = argv[++i];
        } else if (a == "--width" && i + 1 < argc) {
            try { args.width = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "[main] --width: valor inválido\n"; }
        } else if (a == "--height" && i + 1 < argc) {
            try { args.height = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "[main] --height: valor inválido\n"; }
        } else if (a == "--fps" && i + 1 < argc) {
            try { args.fps = std::stoi(argv[++i]); }
            catch (...) { std::cerr << "[main] --fps: valor inválido\n"; }
        } else if (a == "--res" && i + 1 < argc) {
            // --res 1280x720
            std::string res = argv[++i];
            auto pos = res.find('x');
            if (pos != std::string::npos) {
                try { args.width = std::stoi(res.substr(0, pos));
                      args.height = std::stoi(res.substr(pos + 1)); }
                catch (...) { std::cerr << "[main] --res: formato inválido (use WxH)\n"; }
            }
        } else {
            std::cerr << "[main] Argumento desconhecido: " << a << "\n";
        }
    }
#else
    static const option kLongOptions[] = {
        { "device",   required_argument, nullptr, 'd' },
        { "midi-out", required_argument, nullptr, 'm' },
        { "no-viz",   no_argument,       nullptr, 'n' },
        { "eval",     no_argument,       nullptr, 'e' },
        { "width",    required_argument, nullptr, 'W' },
        { "height",   required_argument, nullptr, 'H' },
        { "fps",      required_argument, nullptr, 'f' },
        { "res",      required_argument, nullptr, 'r' },
        { nullptr,    0,                 nullptr,  0  }
    };

    int opt;
    int optIndex = 0;
    while ((opt = getopt_long(argc, argv, "d:m:nef:r:", kLongOptions, &optIndex)) != -1) {
        switch (opt) {
        case 'd':
            try { args.deviceIndex = std::stoi(optarg); }
            catch (...) {
                std::cerr << "[main] --device requer um número inteiro.\n";
                std::exit(1);
            }
            break;
        case 'm': args.midiOutName = optarg; break;
        case 'n': args.noViz = true;         break;
        case 'e': args.eval  = true;         break;
        case 'W':
            try { args.width = std::stoi(optarg); }
            catch (...) { std::cerr << "[main] --width: valor inválido\n"; }
            break;
        case 'H':
            try { args.height = std::stoi(optarg); }
            catch (...) { std::cerr << "[main] --height: valor inválido\n"; }
            break;
        case 'f':
            try { args.fps = std::stoi(optarg); }
            catch (...) { std::cerr << "[main] --fps: valor inválido\n"; }
            break;
        case 'r': {
            std::string res = optarg;
            auto pos = res.find('x');
            if (pos != std::string::npos) {
                try { args.width = std::stoi(res.substr(0, pos));
                      args.height = std::stoi(res.substr(pos + 1)); }
                catch (...) { std::cerr << "[main] --res: formato inválido (use WxH)\n"; }
            }
            break;
        }
        default:
            std::cerr << "Uso: aerochord [--device N] [--midi-out nome] "
                         "[--res WxH] [--fps N] [--no-viz] [--eval]\n";
            break;
        }
    }
#endif

    return args;
}

// =============================================================================
// main — monta e executa o pipeline Aerochord
// =============================================================================
int main(int argc, char* argv[]) {
    const Args args = parseArgs(argc, argv);

    juce::initialiseJuce_GUI();
    // Garante shutdown do JUCE em qualquer caminho de saída
    struct JuceGuard { ~JuceGuard() { juce::shutdownJuce_GUI(); } } juceGuard;

    auto& log = aerochord::Logger::instance();
    log.setLevel(aerochord::LogLevel::DBG);
    log.info("main", "Aerochord iniciando...");

    if (args.eval)
        log.info("main", "Modo --eval ativo — CSV de latência será gravado no diretório atual.");
    if (args.noViz)
        log.info("main", "Modo --no-viz: janela de visualização desativada.");

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    // -------------------------------------------------------------------------
    // Filas lock-free de comunicação entre módulos
    // -------------------------------------------------------------------------
    auto frameQueue    = std::make_shared<aerochord::FrameQueue>();
    auto landmarkQueue = std::make_shared<aerochord::LandmarkQueue>();
    auto gestureQueue  = std::make_shared<aerochord::GestureQueue>();
    auto midiQueue     = std::make_shared<aerochord::MidiQueue>();

#ifdef AEROCHORD_VISUALIZATION_ENABLED
    std::shared_ptr<aerochord::VizQueue> vizQueue;
    if (!args.noViz)
        vizQueue = std::make_shared<aerochord::VizQueue>();
#endif

    // -------------------------------------------------------------------------
    // Instanciar módulos
    // -------------------------------------------------------------------------
    aerochord::CaptureModule capture(
        frameQueue,
        aerochord::CaptureModule::Config{
            .deviceIndex  = args.deviceIndex,
            .targetWidth  = args.width,
            .targetHeight = args.height,
            .targetFps    = args.fps,
        }
    );

    aerochord::PoseDetectionModule poseDetection(
        frameQueue,
        landmarkQueue,
        aerochord::PoseDetectionModule::Config{
            .smoothingAlpha = 0.6f,
            .modelPath      = "assets/hand_landmarker.task"
        }
    );

#ifdef AEROCHORD_VISUALIZATION_ENABLED
    if (vizQueue)
        poseDetection.setVizQueue(vizQueue);
#endif

    aerochord::GestureAnalysisModule gestureAnalysis(
        landmarkQueue,
        gestureQueue
    );

    aerochord::MappingModule mapping(
        gestureQueue,
        midiQueue,
        aerochord::MappingModule::Config{ .midiChannel = 0, .defaultOctave = 4 }
    );

    aerochord::MidiGenerationModule midiGen(
        midiQueue,
        aerochord::MidiGenerationModule::Config{
            .outputDeviceName = args.midiOutName,
            .preferMidi2      = true,
            .midiChannel      = 0,
            .evalMode         = args.eval
        }
    );

#ifdef AEROCHORD_VISUALIZATION_ENABLED
    std::unique_ptr<aerochord::VisualizationModule> viz;
#endif

    // -------------------------------------------------------------------------
    // Listar dispositivos MIDI disponíveis
    // -------------------------------------------------------------------------
    const auto midiDevices = aerochord::MidiGenerationModule::listOutputDevices();
    log.info("main", "Dispositivos MIDI disponíveis:");
    for (const auto& dev : midiDevices)
        log.info("main", "  - " + std::string(dev.toRawUTF8()));

    // -------------------------------------------------------------------------
    // Iniciar pipeline (consumidores primeiro para evitar race conditions)
    // -------------------------------------------------------------------------
    if (!midiGen.start())        { log.fatal("main", "Falha ao iniciar MidiGenerationModule.");  return 1; }
    if (!mapping.start())        { log.fatal("main", "Falha ao iniciar MappingModule.");          return 1; }
    if (!gestureAnalysis.start()){ log.fatal("main", "Falha ao iniciar GestureAnalysisModule."); return 1; }
    if (!poseDetection.start())  { log.fatal("main", "Falha ao iniciar PoseDetectionModule.");   return 1; }
    if (!capture.start())        { log.fatal("main", "Falha ao iniciar CaptureModule.");          return 1; }

    log.info("main", "FPS da câmera: " + std::to_string(capture.actualFps()));

#ifdef AEROCHORD_VISUALIZATION_ENABLED
    if (vizQueue) {
        // Criar visualização com FPS adaptado ao da câmera (após negociação V4L2)
        aerochord::VisualizationModule::Config vizCfg;
        vizCfg.displayFps = capture.actualFps();
        viz = std::make_unique<aerochord::VisualizationModule>(vizQueue, vizCfg);
        viz->setHudSource(&midiGen, &mapping);
        viz->setControlTarget(&mapping);
        if (!viz->start())
            log.warning("main", "VisualizationModule falhou ao iniciar. Continuando sem visualização.");
    }
#endif

    log.info("main", "Pipeline ativo. Pressione Ctrl+C (ou 'q' na janela) para encerrar.");

    // -------------------------------------------------------------------------
    // Loop principal — aguarda sinal de parada; exibe estatísticas a cada 2 s
    // -------------------------------------------------------------------------
    int statTick = 0;
    while (g_running.load()) {
        juce::Thread::sleep(100);

#ifdef AEROCHORD_VISUALIZATION_ENABLED
        // Propagar stop da janela (tecla q/ESC) para todo o pipeline
        if (viz && !viz->isRunning())
            g_running.store(false);
#endif

        if (++statTick >= 20) {  // a cada 2 s (20 × 100 ms)
            statTick = 0;
            log.info("stats",
                "frames_cap=" + std::to_string(capture.framesCaptured()) +
                " frames_drop=" + std::to_string(capture.framesDropped()) +
                " lm_det=" + std::to_string(poseDetection.landmarksDetected()) +
                " gestures=" + std::to_string(gestureAnalysis.gesturesEmitted()) +
                " commands=" + std::to_string(mapping.commandsEmitted()) +
                " midi_sent=" + std::to_string(midiGen.packetsSent()) +
                " midi_drop=" + std::to_string(midiGen.packetsDropped()) +
                " lat_p50=" + [&]{ char b[16]; snprintf(b,sizeof(b),"%.1f",midiGen.latencyP50Ms()); return std::string(b); }() + "ms" +
                " lat_p95=" + [&]{ char b[16]; snprintf(b,sizeof(b),"%.1f",midiGen.latencyP95Ms()); return std::string(b); }() + "ms" +
                " octave=" + std::to_string(mapping.currentOctave()) +
                " note=" + std::to_string(mapping.activeNote()) +
                " midi2=" + (midiGen.isMidi2Mode() ? "yes" : "no"));
        }
    }

    // -------------------------------------------------------------------------
    // Encerrar pipeline (produtores primeiro)
    // -------------------------------------------------------------------------
    log.info("main", "Encerrando pipeline...");
    capture.stop();
    poseDetection.stop();
    gestureAnalysis.stop();
    mapping.stop();
    midiGen.stop();

#ifdef AEROCHORD_VISUALIZATION_ENABLED
    if (viz) viz->stop();
#endif

    log.info("main", "Aerochord encerrado.");
    return 0;
}
