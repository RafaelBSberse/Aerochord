#include "VisualizationModule.h"
#include "common/Logger.h"
#include "mapping/MappingModule.h"
#include "midi_generation/MidiGenerationModule.h"

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

namespace aerochord {

static constexpr std::string_view kModule = "VisualizationModule";

// ---------------------------------------------------------------------------
// Topologia de esqueleto — 20 conexões fixas do MediaPipe Hand
// ---------------------------------------------------------------------------
static constexpr std::pair<int, int> kHandConnections[] = {
    {0,  1}, {1,  2}, {2,  3}, {3,  4},   // polegar
    {0,  5}, {5,  6}, {6,  7}, {7,  8},   // indicador
    {0,  9}, {9, 10}, {10, 11}, {11, 12}, // médio
    {0, 13}, {13, 14}, {14, 15}, {15, 16},// anelar
    {0, 17}, {17, 18}, {18, 19}, {19, 20},// mínimo
    {5,  9}, {9, 13}, {13, 17}            // palma
};

// ---------------------------------------------------------------------------
// Funções estáticas de renderização (confinadas ao .cpp — sem vazamento OpenCV)
// ---------------------------------------------------------------------------

// lineColor: cor das conexões; dotColor: cor dos pontos
static void drawLandmarks(cv::Mat& img, const HandLandmarks& lm, int w, int h,
                          cv::Scalar lineColor, cv::Scalar dotColor) {
    for (auto [a, b] : kHandConnections) {
        cv::line(img,
            cv::Point(static_cast<int>(lm.landmarks[a].x * w),
                      static_cast<int>(lm.landmarks[a].y * h)),
            cv::Point(static_cast<int>(lm.landmarks[b].x * w),
                      static_cast<int>(lm.landmarks[b].y * h)),
            lineColor, 2, cv::LINE_AA);
    }
    for (size_t i = 0; i < HandLandmarks::NUM_LANDMARKS; ++i) {
        cv::circle(img,
            cv::Point(static_cast<int>(lm.landmarks[i].x * w),
                      static_cast<int>(lm.landmarks[i].y * h)),
            4, dotColor, cv::FILLED);
    }
}

// ---------------------------------------------------------------------------
// Cola visual: faixas horizontais mostrando zona de cada nota na oitava atual
// ---------------------------------------------------------------------------
static const char* kNoteNames[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static void drawNoteGuide(cv::Mat& img, const MappingModule* mapping) {
    if (!mapping) return;

    const int h = img.rows;
    const int w = img.cols;
    const int octave     = mapping->currentOctave();
    const int activeNote = mapping->activeNote();

    constexpr int kSemitones  = 12;
    constexpr int kLabelWidth = 52;  // largura do label na margem direita

    // As zonas ativas ocupam [p, 1-p] da altura — o restante são "dead zones"
    // de tolerância nas extremidades (clampadas para a nota mais próxima).
    const float p    = std::clamp(mapping->zonePadding(), 0.0f, 0.45f);
    const float span = 1.0f - 2.0f * p;

    // Sombrear levemente as faixas de tolerância no topo e base
    const int padPx = static_cast<int>(h * p);
    if (padPx > 0) {
        auto shade = [&](int y0, int y1) {
            cv::Mat zone = img(cv::Rect(0, y0, w - kLabelWidth, y1 - y0));
            cv::Mat overlay; zone.copyTo(overlay);
            overlay.setTo(cv::Scalar(0, 0, 0));
            cv::addWeighted(overlay, 0.35, zone, 0.65, 0, zone);
        };
        shade(0, padPx);
        shade(h - padPx, h);
    }

    for (int i = 0; i < kSemitones; ++i) {
        // Zona ativa i: normalizedY ∈ [p + i*w, p + (i+1)*w)  com w = span/12.
        // Na tela: screenY = h * (1 - normalizedY)  →  mão alta = topo = nota alta.
        const float yTopN    = 1.0f - (p + static_cast<float>(i + 1) / kSemitones * span);
        const float yBottomN = 1.0f - (p + static_cast<float>(i)     / kSemitones * span);
        const int yTop    = static_cast<int>(h * yTopN);
        const int yBottom = static_cast<int>(h * yBottomN);
        const int yMid    = (yTop + yBottom) / 2;

        const int  midiNote = octave * 12 + i;
        const bool isActive = (midiNote == activeNote);
        const bool isBlack  = (i == 1 || i == 3 || i == 6 || i == 8 || i == 10);

        // Linha-guia horizontal cruzando toda a tela (borda inferior de cada zona)
        const cv::Scalar lineColor = isBlack
            ? cv::Scalar(80, 80, 80)
            : cv::Scalar(120, 120, 120);
        cv::line(img, cv::Point(0, yBottom), cv::Point(w - kLabelWidth, yBottom),
                 lineColor, 1, cv::LINE_AA);

        // Faixa de destaque quando a nota está ativa
        if (isActive) {
            cv::Mat zone = img(cv::Rect(0, yTop, w - kLabelWidth, yBottom - yTop));
            cv::Mat overlay;
            zone.copyTo(overlay);
            overlay.setTo(cv::Scalar(0, 180, 80));
            cv::addWeighted(overlay, 0.25, zone, 0.75, 0, zone);
        }

        // Label na margem direita — fundo sólido para legibilidade
        const int labelX = w - kLabelWidth;
        cv::Scalar bgColor = isBlack ? cv::Scalar(40, 40, 40) : cv::Scalar(70, 70, 70);
        if (isActive)
            bgColor = cv::Scalar(0, 160, 80);
        cv::rectangle(img, cv::Point(labelX, yTop), cv::Point(w, yBottom),
                      bgColor, cv::FILLED);
        cv::line(img, cv::Point(labelX, yTop), cv::Point(w, yTop),
                 cv::Scalar(140, 140, 140), 1);

        // Nome da nota
        char label[8];
        snprintf(label, sizeof(label), "%s%d", kNoteNames[i], octave);
        cv::putText(img, label, cv::Point(labelX + 4, yMid + 5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4,
                    isActive ? cv::Scalar(255, 255, 255) : cv::Scalar(200, 200, 200),
                    1, cv::LINE_AA);
    }
}

static void drawHud(cv::Mat& img, float fps,
                    const MidiGenerationModule* midiGen,
                    const MappingModule*         mapping,
                    uint64_t drops) {
    char buf[128];

    snprintf(buf, sizeof(buf), "FPS: %.1f  Drop: %llu",
             fps, static_cast<unsigned long long>(drops));
    cv::putText(img, buf, cv::Point(8, 22),
                cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);

    const bool midi2 = midiGen ? midiGen->isMidi2Mode() : false;
    snprintf(buf, sizeof(buf), "MIDI: %s", midi2 ? "2.0" : "1.0");
    cv::putText(img, buf, cv::Point(8, 44),
                cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);

    if (mapping) {
        snprintf(buf, sizeof(buf), "Oct: %d  Note: %d",
                 mapping->currentOctave(), mapping->activeNote());
        cv::putText(img, buf, cv::Point(8, 66),
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
    }

    if (midiGen) {
        snprintf(buf, sizeof(buf), "Lat P50: %.1fms  P95: %.1fms",
                 midiGen->latencyP50Ms(), midiGen->latencyP95Ms());
        cv::putText(img, buf, cv::Point(8, 88),
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
    }
}

// ---------------------------------------------------------------------------
// Instrumentos GM selecionados (índice do trackbar → programa MIDI)
// ---------------------------------------------------------------------------
struct GmInstrument {
    int         program;  // General MIDI program number [0, 127]
    const char* name;
};

static constexpr GmInstrument kInstruments[] = {
    {  0, "Piano"          },   //  0 - Acoustic Grand Piano
    {  4, "E.Piano"        },   //  1 - Electric Piano 1
    {  6, "Harpsichord"    },   //  2 - Harpsichord
    { 12, "Marimba"        },   //  3 - Marimba
    { 19, "Organ"          },   //  4 - Church Organ
    { 24, "Nylon Guitar"   },   //  5 - Acoustic Guitar (nylon)
    { 27, "Jazz Guitar"    },   //  6 - Electric Guitar (jazz)
    { 32, "Acoustic Bass"  },   //  7 - Acoustic Bass
    { 40, "Violin"         },   //  8 - Violin
    { 42, "Cello"          },   //  9 - Cello
    { 48, "Strings"        },   // 10 - String Ensemble 1
    { 56, "Trumpet"        },   // 11 - Trumpet
    { 61, "Brass"          },   // 12 - Brass Section
    { 65, "Alto Sax"       },   // 13 - Alto Sax
    { 73, "Flute"          },   // 14 - Flute
    { 79, "Ocarina"        },   // 15 - Ocarina
    { 80, "Square Lead"    },   // 16 - Lead 1 (square)
    { 81, "Saw Lead"       },   // 17 - Lead 2 (sawtooth)
    { 88, "New Age Pad"    },   // 18 - Pad 1 (new age)
    { 91, "Sci-Fi Pad"     },   // 19 - Pad 4 (choir)
};

static constexpr int kNumInstruments = sizeof(kInstruments) / sizeof(kInstruments[0]);

// ---------------------------------------------------------------------------
// Layout do painel de controle (constantes compartilhadas entre draw e click)
// ---------------------------------------------------------------------------
static constexpr int kPanelHeight   = 44;

// Toggle switch legato
static constexpr int kToggleX       = 80;   // esquerda do toggle
static constexpr int kToggleW       = 50;
static constexpr int kToggleH       = 22;

// Setas do seletor de instrumento
static constexpr int kSelLeftX      = 250;
static constexpr int kSelArrowW     = 28;
static constexpr int kSelRightX     = 500;

// ---------------------------------------------------------------------------
// Desenho do painel de controle com toggle switch + select
// panelY: coordenada Y onde o painel começa no canvas
// ---------------------------------------------------------------------------
static void drawControlPanel(cv::Mat& img, const MappingModule* mapping,
                             int instrumentIdx, int panelY) {
    if (!mapping) return;

    const int w = img.cols;

    // Fundo sólido do painel
    cv::rectangle(img, cv::Point(0, panelY), cv::Point(w, panelY + kPanelHeight),
                  cv::Scalar(30, 30, 30), cv::FILLED);

    // Linha divisória no topo
    cv::line(img, cv::Point(0, panelY), cv::Point(w, panelY),
             cv::Scalar(80, 200, 200), 1);

    const int cy = panelY + kPanelHeight / 2;  // centro vertical do painel

    // --- Toggle switch (Legato) ---
    const bool legato = mapping->isLegatoMode();

    // Label
    cv::putText(img, "Legato", cv::Point(10, cy + 5),
                cv::FONT_HERSHEY_SIMPLEX, 0.48, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);

    // Trilha do toggle (pill shape)
    const int toggleY = cy - kToggleH / 2;
    const int radius  = kToggleH / 2;
    const cv::Scalar trackColor = legato ? cv::Scalar(0, 180, 80) : cv::Scalar(80, 80, 80);

    // Retângulo central + semicírculos nas pontas
    cv::rectangle(img,
        cv::Point(kToggleX + radius, toggleY),
        cv::Point(kToggleX + kToggleW - radius, toggleY + kToggleH),
        trackColor, cv::FILLED);
    cv::circle(img, cv::Point(kToggleX + radius, cy), radius, trackColor, cv::FILLED, cv::LINE_AA);
    cv::circle(img, cv::Point(kToggleX + kToggleW - radius, cy), radius, trackColor, cv::FILLED, cv::LINE_AA);

    // Bolinha do toggle (knob)
    const int knobX = legato
        ? (kToggleX + kToggleW - radius)
        : (kToggleX + radius);
    cv::circle(img, cv::Point(knobX, cy), radius - 2, cv::Scalar(240, 240, 240), cv::FILLED, cv::LINE_AA);

    // ON/OFF label
    const char* stateLabel = legato ? "ON" : "OFF";
    cv::putText(img, stateLabel, cv::Point(kToggleX + kToggleW + 6, cy + 5),
                cv::FONT_HERSHEY_SIMPLEX, 0.4,
                legato ? cv::Scalar(0, 255, 100) : cv::Scalar(140, 140, 140),
                1, cv::LINE_AA);

    // --- Seletor de instrumento (< Nome do Instrumento >) ---
    const int idx = std::clamp(instrumentIdx, 0, kNumInstruments - 1);

    // Seta esquerda [<]
    const cv::Scalar arrowColor = cv::Scalar(180, 200, 255);
    cv::rectangle(img,
        cv::Point(kSelLeftX, cy - 12),
        cv::Point(kSelLeftX + kSelArrowW, cy + 12),
        cv::Scalar(60, 60, 80), cv::FILLED);
    cv::rectangle(img,
        cv::Point(kSelLeftX, cy - 12),
        cv::Point(kSelLeftX + kSelArrowW, cy + 12),
        cv::Scalar(120, 130, 160), 1);
    cv::putText(img, "<", cv::Point(kSelLeftX + 8, cy + 6),
                cv::FONT_HERSHEY_SIMPLEX, 0.55, arrowColor, 2, cv::LINE_AA);

    // Nome do instrumento
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", kInstruments[idx].name);
    const int textW = cv::getTextSize(buf, cv::FONT_HERSHEY_SIMPLEX, 0.52, 1, nullptr).width;
    const int nameX = (kSelLeftX + kSelArrowW + kSelRightX) / 2 - textW / 2;
    cv::putText(img, buf, cv::Point(nameX, cy + 5),
                cv::FONT_HERSHEY_SIMPLEX, 0.52, cv::Scalar(220, 230, 255), 1, cv::LINE_AA);

    // GM number abaixo do nome
    snprintf(buf, sizeof(buf), "GM %d", kInstruments[idx].program);
    const int gmW = cv::getTextSize(buf, cv::FONT_HERSHEY_SIMPLEX, 0.32, 1, nullptr).width;
    const int gmX = (kSelLeftX + kSelArrowW + kSelRightX) / 2 - gmW / 2;
    cv::putText(img, buf, cv::Point(gmX, cy + 18),
                cv::FONT_HERSHEY_SIMPLEX, 0.32, cv::Scalar(140, 150, 180), 1, cv::LINE_AA);

    // Seta direita [>]
    cv::rectangle(img,
        cv::Point(kSelRightX, cy - 12),
        cv::Point(kSelRightX + kSelArrowW, cy + 12),
        cv::Scalar(60, 60, 80), cv::FILLED);
    cv::rectangle(img,
        cv::Point(kSelRightX, cy - 12),
        cv::Point(kSelRightX + kSelArrowW, cy + 12),
        cv::Scalar(120, 130, 160), 1);
    cv::putText(img, ">", cv::Point(kSelRightX + 7, cy + 6),
                cv::FONT_HERSHEY_SIMPLEX, 0.55, arrowColor, 2, cv::LINE_AA);
}

// ---------------------------------------------------------------------------
// Construção / destruição
// ---------------------------------------------------------------------------
VisualizationModule::VisualizationModule(std::shared_ptr<VizQueue> inputQueue)
    : VisualizationModule(std::move(inputQueue), Config{}) {}

VisualizationModule::VisualizationModule(std::shared_ptr<VizQueue> inputQueue,
                                         Config config)
    : inputQueue_(std::move(inputQueue))
    , config_(std::move(config))
{}

VisualizationModule::~VisualizationModule() {
    stop();
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------
bool VisualizationModule::start() {
    if (running_.load())
        return true;

    AEROCHORD_LOG_INFO(kModule, "Iniciando thread de visualização...");
    running_.store(true);
    thread_ = std::thread(&VisualizationModule::vizLoop, this);
    return true;
}

void VisualizationModule::stop() {
    if (!running_.exchange(false))
        return;

    if (thread_.joinable())
        thread_.join();

    AEROCHORD_LOG_INFO(kModule, "Visualização encerrada.");
}

bool VisualizationModule::isRunning() const { return running_.load(); }

void VisualizationModule::setHudSource(const MidiGenerationModule* midiGen,
                                       const MappingModule*         mapping) {
    midiGen_ = midiGen;
    mapping_ = mapping;
}

void VisualizationModule::setControlTarget(MappingModule* mapping) {
    controlTarget_ = mapping;
}

uint64_t VisualizationModule::framesRendered() const { return framesRendered_.load(); }
uint64_t VisualizationModule::framesDropped()  const { return framesDropped_.load(); }

// ---------------------------------------------------------------------------
// Mouse callback — controles interativos
// ---------------------------------------------------------------------------
void VisualizationModule::onMouse(int event, int x, int y, int /*flags*/, void* userdata) {
    if (event != cv::EVENT_LBUTTONDOWN)
        return;
    auto* self = static_cast<VisualizationModule*>(userdata);
    self->handleClick(x, y);
}

void VisualizationModule::handleClick(int x, int y) {
    if (!controlTarget_)
        return;

    // O painel fica abaixo do frame da câmera
    const int panelY = frameHeight_;
    if (y < panelY || y > panelY + kPanelHeight)
        return;  // clique fora do painel

    const int cy = panelY + kPanelHeight / 2;

    // Toggle legato — clique na região do switch
    if (x >= kToggleX && x <= kToggleX + kToggleW + 30
        && y >= cy - kToggleH / 2 - 4 && y <= cy + kToggleH / 2 + 4)
    {
        const bool current = controlTarget_->isLegatoMode();
        controlTarget_->setLegatoMode(!current);
        return;
    }

    // Seta esquerda do instrumento [<]
    if (x >= kSelLeftX && x <= kSelLeftX + kSelArrowW
        && y >= cy - 14 && y <= cy + 14)
    {
        if (instrumentIdx_ > 0) {
            --instrumentIdx_;
            controlTarget_->requestProgramChange(kInstruments[instrumentIdx_].program);
        }
        return;
    }

    // Seta direita do instrumento [>]
    if (x >= kSelRightX && x <= kSelRightX + kSelArrowW
        && y >= cy - 14 && y <= cy + 14)
    {
        if (instrumentIdx_ < kNumInstruments - 1) {
            ++instrumentIdx_;
            controlTarget_->requestProgramChange(kInstruments[instrumentIdx_].program);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Loop de visualização (thread dedicada)
//
// cv::namedWindow / cv::imshow / cv::waitKey DEVEM ser chamados nesta thread.
// cv::waitKey(1) é chamado incondicionalmente a cada iteração para processar
// eventos da GUI no X11/Wayland — se omitido, a janela congela.
// ---------------------------------------------------------------------------
void VisualizationModule::vizLoop() {
    AEROCHORD_LOG_DEBUG(kModule, "vizLoop() iniciado.");

    // WINDOW_GUI_NORMAL desabilita a barra Qt no topo (toolbar com zoom, save, etc.)
    cv::namedWindow(config_.windowName, cv::WINDOW_AUTOSIZE | cv::WINDOW_GUI_NORMAL);

    // Registrar mouse callback para controles interativos
    if (controlTarget_) {
        cv::setMouseCallback(config_.windowName, &VisualizationModule::onMouse, this);
    }

    const auto frameBudget =
        std::chrono::microseconds(1'000'000 / config_.displayFps);
    auto nextFrameTime = std::chrono::steady_clock::now();
    auto lastFpsTime   = nextFrameTime;
    uint64_t fpsCnt    = 0;
    float    currentFps = 0.0f;

    while (running_.load()) {
        // Processar eventos GUI — chamada obrigatória; retorna tecla pressionada
        const int key = cv::waitKey(1);
        if (key == 'q' || key == 27 /* ESC */) {
            AEROCHORD_LOG_INFO(kModule, "Tecla de saída detectada — encerrando.");
            running_.store(false);
            break;
        }

        auto vizOpt = inputQueue_->pop();
        if (!vizOpt) {
            std::this_thread::yield();
            continue;
        }

        auto& vf = *vizOpt;

        // Montar imagem de exibição.
        // O VizFrame já foi consumido da fila (move) — o buffer é nosso.
        cv::Mat cameraFrame;
        if (!vf.frame.data.empty()) {
            cameraFrame = cv::Mat(vf.frame.height, vf.frame.width, CV_8UC3,
                                  vf.frame.data.data());
        } else {
            // Câmera sem sinal (stub sem V4L2 ou plataforma não Linux)
            cameraFrame = cv::Mat::zeros(480, 640, CV_8UC3);
            cv::putText(cameraFrame, "Camera: sem sinal (stub ativo)",
                        cv::Point(10, 240), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                        cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
        }

        const int camW = cameraFrame.cols;
        const int camH = cameraFrame.rows;

        // Sobrepor landmarks de cada mão (quando MediaPipe estiver integrado)
        if (config_.drawSkeleton) {
            // Índice 0 = esquerda: conexões azuis, pontos ciano
            if (vf.landmarks[0].has_value())
                drawLandmarks(cameraFrame, *vf.landmarks[0], camW, camH,
                              cv::Scalar(255, 128, 0), cv::Scalar(255, 255, 0));
            // Índice 1 = direita: conexões verdes, pontos vermelhos
            if (vf.landmarks[1].has_value())
                drawLandmarks(cameraFrame, *vf.landmarks[1], camW, camH,
                              cv::Scalar(0, 255, 0), cv::Scalar(0, 0, 255));
        }

        // Desenhar HUD e note guide sobre o frame da câmera
        drawNoteGuide(cameraFrame, mapping_);
        drawHud(cameraFrame, currentFps, midiGen_, mapping_, framesDropped_.load());

        // Canvas estendido: câmera em cima + painel de controle embaixo (área separada)
        const bool hasPanel = (controlTarget_ != nullptr);
        const int canvasH = hasPanel ? camH + kPanelHeight : camH;
        cv::Mat display(canvasH, camW, CV_8UC3);

        // Copiar frame da câmera para a parte superior do canvas
        cameraFrame.copyTo(display(cv::Rect(0, 0, camW, camH)));

        // Atualizar FPS (a cada segundo)
        ++fpsCnt;
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - lastFpsTime).count();
        if (elapsed >= 1.0f) {
            currentFps = static_cast<float>(fpsCnt) / elapsed;
            fpsCnt     = 0;
            lastFpsTime = now;
        }

        // Atualizar dimensões do frame da câmera para o mouse callback
        frameWidth_  = camW;
        frameHeight_ = camH;

        // Desenhar painel de controle na área dedicada abaixo da câmera
        if (hasPanel)
            drawControlPanel(display, mapping_, instrumentIdx_, camH);

        cv::imshow(config_.windowName, display);
        ++framesRendered_;

        // Throttle: não renderizar acima de displayFps
        nextFrameTime += frameBudget;
        const auto sleepFor = nextFrameTime - std::chrono::steady_clock::now();
        if (sleepFor > std::chrono::microseconds(0))
            std::this_thread::sleep_for(sleepFor);
    }

    cv::destroyWindow(config_.windowName);
    AEROCHORD_LOG_DEBUG(kModule, "vizLoop() encerrado.");
}

} // namespace aerochord
