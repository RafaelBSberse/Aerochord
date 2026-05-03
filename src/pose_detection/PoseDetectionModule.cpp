#include "PoseDetectionModule.h"
#include "common/Logger.h"

#ifndef AEROCHORD_MEDIAPIPE_STUB
#  include "mediapipe/tasks/c/vision/hand_landmarker/hand_landmarker.h"
#  include <cstdlib>   // free()
#  include <cstring>   // strcmp
#endif

// ---------------------------------------------------------------------------
// Integração MediaPipe real (quando MEDIAPIPE_ROOT está disponível)
//
// A integração usa a C++ Tasks API do MediaPipe:
//   #include "mediapipe/tasks/cc/vision/hand_landmarker/hand_landmarker.h"
//   namespace mptasks = mediapipe::tasks::vision;
//
// Fluxo de inicialização (substituir bloco AEROCHORD_MEDIAPIPE_STUB):
//   auto options = std::make_unique<mptasks::HandLandmarkerOptions>();
//   options->base_options.model_asset_path = "hand_landmarker.task";
//   options->num_hands = config_.maxHands;
//   options->min_hand_detection_confidence = config_.minConfidence;
//   options->min_tracking_confidence = config_.minConfidence;
//   mediapipe_->landmarker = mptasks::HandLandmarker::Create(std::move(options));
//
// Fluxo de detecção por frame (substituir bloco AEROCHORD_MEDIAPIPE_STUB em detectionLoop):
//   auto image = mediapipe::Image(mediapipe::ImageFormat::SRGB, frameOpt->width,
//                                  frameOpt->height, frameOpt->data.data());
//   auto result = mediapipe_->landmarker->Detect(image, /*timestamp_ms=*/...);
//   for (int i = 0; i < result.hand_landmarks.size(); ++i) {
//       HandLandmarks lm = convertFromMediaPipe(result, i);
//       int slot = (lm.hand == HandLabel::LEFT) ? 0 : 1;
//       HandLandmarks smoothed = smooth(lm, prevLandmarks_[slot]);
//       prevLandmarks_[slot] = smoothed;
//       outputQueue_->push(smoothed);
//   }
// ---------------------------------------------------------------------------

namespace aerochord {

static constexpr std::string_view kModule = "PoseDetectionModule";

// ===========================================================================
// MediaPipeHandle
// ===========================================================================
struct PoseDetectionModule::MediaPipeHandle {
    bool initialized = false;
#ifndef AEROCHORD_MEDIAPIPE_STUB
    MpHandLandmarkerPtr landmarker = nullptr;
#endif
};

// ===========================================================================
// Construção / destruição
// ===========================================================================
PoseDetectionModule::PoseDetectionModule(std::shared_ptr<FrameQueue>    inputQueue,
                                         std::shared_ptr<LandmarkQueue> outputQueue)
    : PoseDetectionModule(std::move(inputQueue), std::move(outputQueue), Config{}) {}

PoseDetectionModule::PoseDetectionModule(std::shared_ptr<FrameQueue>    inputQueue,
                                         std::shared_ptr<LandmarkQueue> outputQueue,
                                         Config config)
    : inputQueue_(std::move(inputQueue))
    , outputQueue_(std::move(outputQueue))
    , config_(config)
    , mediapipe_(std::make_unique<MediaPipeHandle>())
{
}

PoseDetectionModule::~PoseDetectionModule() {
    stop();
}

// ===========================================================================
// Ciclo de vida
// ===========================================================================
bool PoseDetectionModule::start() {
    if (running_.load())
        return true;

    AEROCHORD_LOG_INFO(kModule, "Inicializando MediaPipe Hand Landmarker...");

#ifdef AEROCHORD_MEDIAPIPE_STUB
    AEROCHORD_LOG_WARN(kModule,
        "AEROCHORD_MEDIAPIPE_STUB ativo — landmarks sintéticos serão gerados. "
        "Passe -DMEDIAPIPE_ROOT=<path> ao CMake para usar o detector real.");
    mediapipe_->initialized = true;
#else
    // --- Integração real: inicializar HandLandmarker via C API ---
    // Garantir valores válidos (guard contra designated-init zero-value edge case)
    const int   numHands   = (config_.maxHands   > 0) ? config_.maxHands   : 2;
    const float confidence = (config_.minConfidence > 0.0f) ? config_.minConfidence : 0.5f;

    AEROCHORD_LOG_INFO(kModule,
        "Config: num_hands=" + std::to_string(numHands) +
        " confidence=" + std::to_string(confidence) +
        " model=" + config_.modelPath);

    HandLandmarkerOptions options{};
    options.base_options.model_asset_path = config_.modelPath.c_str();
    options.running_mode                  = VIDEO;
    options.num_hands                     = numHands;
    options.min_hand_detection_confidence = confidence;
    options.min_hand_presence_confidence  = confidence;
    options.min_tracking_confidence       = confidence;

    char* error_msg = nullptr;
    MpStatus status = MpHandLandmarkerCreate(&options, &mediapipe_->landmarker, &error_msg);
    if (status != kMpOk) {
        std::string err = error_msg ? error_msg : "(sem detalhe)";
        if (error_msg) { free(error_msg); error_msg = nullptr; }
        AEROCHORD_LOG_ERROR(kModule, "Falha ao criar HandLandmarker: " + err);
        mediapipe_->initialized = false;
    } else {
        mediapipe_->initialized = true;
        AEROCHORD_LOG_INFO(kModule, "HandLandmarker criado com modelo: " + config_.modelPath);
    }
#endif

    if (!mediapipe_->initialized) {
        AEROCHORD_LOG_ERROR(kModule, "Falha ao inicializar MediaPipe.");
        return false;
    }

    running_.store(true);
    thread_ = std::thread(&PoseDetectionModule::detectionLoop, this);

    AEROCHORD_LOG_INFO(kModule, "Thread de detecção iniciada.");
    return true;
}

void PoseDetectionModule::stop() {
    if (!running_.exchange(false))
        return;

    if (thread_.joinable())
        thread_.join();

#ifndef AEROCHORD_MEDIAPIPE_STUB
    if (mediapipe_ && mediapipe_->landmarker) {
        MpHandLandmarkerClose(mediapipe_->landmarker, nullptr);
        mediapipe_->landmarker = nullptr;
    }
#endif

    AEROCHORD_LOG_INFO(kModule, "Detecção de pose encerrada.");
}

bool PoseDetectionModule::isRunning() const { return running_.load(); }

void PoseDetectionModule::setVizQueue(std::shared_ptr<VizQueue> vizQueue) {
    vizQueue_ = std::move(vizQueue);
}

uint64_t PoseDetectionModule::framesProcessed()   const { return framesProcessed_.load(); }
uint64_t PoseDetectionModule::detectionFailures()  const { return detectionFailures_.load(); }
uint64_t PoseDetectionModule::landmarksDetected()  const { return landmarksDetected_.load(); }

// ===========================================================================
// Filtro EMA de suavização
// ===========================================================================
HandLandmarks PoseDetectionModule::smooth(const HandLandmarks& raw,
                                          const HandLandmarks& prev) const {
    HandLandmarks out = raw;
    const float alpha = config_.smoothingAlpha;
    const float beta  = 1.0f - alpha;

    for (size_t i = 0; i < HandLandmarks::NUM_LANDMARKS; ++i) {
        out.landmarks[i].x = alpha * raw.landmarks[i].x + beta * prev.landmarks[i].x;
        out.landmarks[i].y = alpha * raw.landmarks[i].y + beta * prev.landmarks[i].y;
        out.landmarks[i].z = alpha * raw.landmarks[i].z + beta * prev.landmarks[i].z;
        // visibility: mantém valor raw para não suavizar thresholds de confiança
    }
    return out;
}

// ===========================================================================
// Stub: landmarks sintéticos para mão direita em posição neutra
//
// Gera landmarks plausíveis para que a FSM do GestureAnalysisModule possa
// operar e ser testada independentemente da câmera e do MediaPipe real.
//
// Posição neutra: mão aberta à altura do peito, dedos apontando para cima.
// Coordenadas normalizadas [0,1]; pulso em (0.5, 0.6), ponta do indicador em (0.5, 0.2).
// ===========================================================================
#ifdef AEROCHORD_MEDIAPIPE_STUB
// ---------------------------------------------------------------------------
// Stub com animação: polegar + indicador oscilam entre posição aberta e pinçada.
//
// Ciclo de 120 frames (≈4 s a 30 fps):
//   frames  0-59  → abrindo a mão  (t: 0→1→0)
//   frames 60-119 → pinçando        (t: 0→1→0, mas na fase fechada)
//
// A animação garante que a FSM percorra IDLE → PINCH_HOLD → IDLE a cada ciclo,
// permitindo testar o pipeline completo sem câmera ou MediaPipe real.
// ---------------------------------------------------------------------------
static HandLandmarks makeSyntheticLandmarks(
        HandLabel hand,
        std::chrono::steady_clock::time_point ts,
        uint64_t frameCount)
{
    HandLandmarks lm{};
    lm.hand       = hand;
    lm.confidence = 0.9f;
    lm.timestamp  = ts;

    // --- Esqueleto base (landmarks fixos) ---
    lm.landmarks[0]  = { 0.50f, 0.60f, 0.00f, 1.0f };  // pulso
    lm.landmarks[1]  = { 0.43f, 0.55f, 0.00f, 1.0f };  // polegar CMC
    lm.landmarks[2]  = { 0.38f, 0.50f, 0.00f, 1.0f };  // polegar MCP
    lm.landmarks[3]  = { 0.34f, 0.45f, 0.00f, 1.0f };  // polegar IP
    // lm.landmarks[4] = polegar TIP — animado abaixo
    lm.landmarks[5]  = { 0.44f, 0.45f, 0.00f, 1.0f };  // indicador MCP
    lm.landmarks[6]  = { 0.44f, 0.37f, 0.00f, 1.0f };  // indicador PIP
    lm.landmarks[7]  = { 0.44f, 0.30f, 0.00f, 1.0f };  // indicador DIP
    // lm.landmarks[8] = indicador TIP — animado abaixo
    lm.landmarks[9]  = { 0.50f, 0.43f, 0.00f, 1.0f };  // médio MCP (Z oscila para pitch bend)
    lm.landmarks[10] = { 0.50f, 0.34f, 0.00f, 1.0f };
    lm.landmarks[11] = { 0.50f, 0.27f, 0.00f, 1.0f };
    lm.landmarks[12] = { 0.50f, 0.20f, 0.00f, 1.0f };
    lm.landmarks[13] = { 0.56f, 0.45f, 0.00f, 1.0f };
    lm.landmarks[14] = { 0.56f, 0.37f, 0.00f, 1.0f };
    lm.landmarks[15] = { 0.56f, 0.30f, 0.00f, 1.0f };
    lm.landmarks[16] = { 0.56f, 0.24f, 0.00f, 1.0f };
    lm.landmarks[17] = { 0.61f, 0.47f, 0.00f, 1.0f };
    lm.landmarks[18] = { 0.62f, 0.40f, 0.00f, 1.0f };
    lm.landmarks[19] = { 0.63f, 0.34f, 0.00f, 1.0f };
    lm.landmarks[20] = { 0.64f, 0.29f, 0.00f, 1.0f };

    // --- Animação: fase triangular em [0, 1] ---
    // Ciclo de 120 frames: metade fechando, metade abrindo
    constexpr uint64_t kCycle = 120;
    const float phase = static_cast<float>(frameCount % kCycle) / static_cast<float>(kCycle);
    // t sobe de 0→1 na primeira metade, desce de 1→0 na segunda
    const float t = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);

    // Polegar TIP: aberto (0.31, 0.40) → pinçado (0.44, 0.25)
    lm.landmarks[4].x = 0.31f + t * (0.44f - 0.31f);
    lm.landmarks[4].y = 0.40f + t * (0.25f - 0.40f);
    lm.landmarks[4].z = 0.00f;
    lm.landmarks[4].visibility = 1.0f;

    // Indicador TIP: aberto (0.44, 0.23) → pinçado (0.44, 0.25)
    lm.landmarks[8].x = 0.44f;
    lm.landmarks[8].y = 0.23f + t * (0.25f - 0.23f);
    lm.landmarks[8].z = 0.00f;
    lm.landmarks[8].visibility = 1.0f;

    // Médio MCP — oscilação Z lenta para exercitar pitch bend (handTiltAngle)
    lm.landmarks[9].z = 0.05f * std::sin(
        static_cast<float>(frameCount) * (2.0f * 3.14159f / 90.0f));

    return lm;
}
#endif  // AEROCHORD_MEDIAPIPE_STUB

// ===========================================================================
// Conversor de resultado MediaPipe C API → HandLandmarks interno
// ===========================================================================
#ifndef AEROCHORD_MEDIAPIPE_STUB
static HandLandmarks convertFromMediaPipe(
    const HandLandmarkerResult& result,
    uint32_t index,
    std::chrono::steady_clock::time_point ts)
{
    HandLandmarks lm{};
    lm.timestamp = ts;

    // Handedness — MediaPipe reporta do ponto de vista da câmera (imagem espelhada).
    // "Left" da câmera = mão DIREITA do usuário (e vice-versa).
    if (index < result.handedness_count) {
        const struct Categories& hc = result.handedness[index];
        if (hc.categories_count > 0) {
            const char* name = hc.categories[0].category_name;
            lm.hand       = (name && std::strcmp(name, "Left") == 0)
                            ? HandLabel::RIGHT : HandLabel::LEFT;
            lm.confidence = hc.categories[0].score;
        }
    }

    // 21 landmarks normalizados [0,1]
    if (index < result.hand_landmarks_count) {
        const struct NormalizedLandmarks& nl = result.hand_landmarks[index];
        const uint32_t count = std::min<uint32_t>(
            nl.landmarks_count, static_cast<uint32_t>(HandLandmarks::NUM_LANDMARKS));
        for (uint32_t i = 0; i < count; ++i) {
            const struct NormalizedLandmark& src = nl.landmarks[i];
            lm.landmarks[i] = {
                src.x, src.y, src.z,
                src.has_visibility ? src.visibility : 1.0f
            };
        }
    }
    return lm;
}
#endif  // !AEROCHORD_MEDIAPIPE_STUB

// ===========================================================================
// Loop de detecção (thread dedicada)
// ===========================================================================
void PoseDetectionModule::detectionLoop() {
    AEROCHORD_LOG_DEBUG(kModule, "detectionLoop() iniciado.");

    while (running_.load()) {
        auto frameOpt = inputQueue_->pop();
        if (!frameOpt) {
            // Fila vazia — spin mínimo para não dominar CPU;
            // em hardware real o poll() da câmera já regula a taxa
            std::this_thread::yield();
            continue;
        }

#ifdef AEROCHORD_MEDIAPIPE_STUB
        // --- Stub: emite landmarks animados (ciclo aberto↔pinçado para teste funcional) ---
        HandLandmarks raw = makeSyntheticLandmarks(HandLabel::RIGHT, frameOpt->timestamp,
                                                   framesProcessed_.load());

        VizFrame vf;
        vf.frame = std::move(*frameOpt);

        if (raw.confidence >= config_.minConfidence) {
            HandLandmarks smoothed = smooth(raw, prevLandmarks_[1]);
            prevLandmarks_[1] = smoothed;
            smoothed.timestamp = vf.frame.timestamp;
            outputQueue_->push(smoothed);
            vf.landmarks[1] = smoothed;  // slot 1 = mão direita (stub emite direita)
        }
        ++framesProcessed_;

        // Publicar VizFrame
        if (vizQueue_) {
            if (!vizQueue_->push(std::move(vf)))
                AEROCHORD_LOG_DEBUG(kModule, "VizQueue cheia — frame descartado.");
        }

#else
        // --- Integração MediaPipe real via C API ---
        if (frameOpt->data.empty()) {
            ++detectionFailures_;
            ++framesProcessed_;
            continue;
        }

        // BGR (VideoFrame) → RGB (MediaPipe kMpImageFormatSrgb)
        // Buffer pré-alocado (reutilizado entre frames — sem heap alloc no hot path)
        const size_t pixelCount = static_cast<size_t>(frameOpt->width * frameOpt->height);
        const size_t rgbSize = pixelCount * 3;
        if (rgbBuffer_.size() != rgbSize)
            rgbBuffer_.resize(rgbSize);
        for (size_t i = 0; i < pixelCount; ++i) {
            rgbBuffer_[i*3 + 0] = frameOpt->data[i*3 + 2]; // R ← B
            rgbBuffer_[i*3 + 1] = frameOpt->data[i*3 + 1]; // G
            rgbBuffer_[i*3 + 2] = frameOpt->data[i*3 + 0]; // B ← R
        }

        // Criar MpImage (cópia interna — rgbBuffer_ pode ser reutilizado após a chamada)
        MpImagePtr mp_image = nullptr;
        char* img_err = nullptr;
        const int pixel_data_size = static_cast<int>(rgbSize);
        MpStatus img_status = MpImageCreateFromUint8Data(
            kMpImageFormatSrgb,
            frameOpt->width, frameOpt->height,
            rgbBuffer_.data(), pixel_data_size,
            &mp_image, &img_err);

        if (img_status != kMpOk || !mp_image) {
            if (img_err) {
                AEROCHORD_LOG_WARN(kModule, "MpImageCreate falhou: " + std::string(img_err));
                free(img_err);
            }
            ++detectionFailures_;
            ++framesProcessed_;
            // Mesmo assim, encaminhar o frame para visualização sem landmarks
            if (vizQueue_) {
                VizFrame vf;
                vf.frame = std::move(*frameOpt);
                vizQueue_->push(std::move(vf));
            }
            continue;
        }

        const int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            frameOpt->timestamp.time_since_epoch()).count();

        HandLandmarkerResult mp_result{};
        char* det_err = nullptr;
        MpStatus det_status = MpHandLandmarkerDetectForVideo(
            mediapipe_->landmarker, mp_image,
            nullptr, ts_ms,
            &mp_result, &det_err);

        MpImageFree(mp_image);

        if (det_status != kMpOk) {
            if (det_err) {
                AEROCHORD_LOG_WARN(kModule, "DetectForVideo falhou (ts=" +
                    std::to_string(ts_ms) + "): " + std::string(det_err));
                free(det_err);
            }
            ++detectionFailures_;
            ++framesProcessed_;
            // Mesmo assim, encaminhar o frame para visualização sem landmarks
            if (vizQueue_) {
                VizFrame vf;
                vf.frame = std::move(*frameOpt);
                vizQueue_->push(std::move(vf));
            }
            continue;
        }

        // Rotacionar flags de slots ativos por frame
        prevSlotActive_[0] = currSlotActive_[0];
        prevSlotActive_[1] = currSlotActive_[1];
        currSlotActive_[0] = false;
        currSlotActive_[1] = false;

        VizFrame vf;
        vf.frame = std::move(*frameOpt);

        // --- Coletar detecções válidas ---
        HandLandmarks dets[2];
        int numDets = 0;
        for (uint32_t i = 0; i < mp_result.hand_landmarks_count && numDets < 2; ++i) {
            HandLandmarks raw = convertFromMediaPipe(mp_result, i, vf.frame.timestamp);
            AEROCHORD_LOG_DEBUG(kModule,
                "MediaPipe det[" + std::to_string(i) + "]: hand=" +
                (raw.hand == HandLabel::LEFT ? "L" : raw.hand == HandLabel::RIGHT ? "R" : "?") +
                " conf=" + std::to_string(raw.confidence));
            if (raw.confidence >= config_.minConfidence && raw.hand != HandLabel::UNKNOWN)
                dets[numDets++] = raw;
        }
        if (mp_result.hand_landmarks_count == 0 && framesProcessed_.load() % 30 == 0) {
            AEROCHORD_LOG_DEBUG(kModule, "MediaPipe: nenhuma mão detectada neste frame.");
        }

        // --- Atribuir detecções a slots por proximidade espacial ---
        // Slot 0 = LEFT, Slot 1 = RIGHT
        // Usa posição do pulso (landmark 0) para matching.
        auto wristDistSq = [](const HandLandmarks& a, const HandLandmarks& b) -> float {
            float dx = a.landmarks[0].x - b.landmarks[0].x;
            float dy = a.landmarks[0].y - b.landmarks[0].y;
            return dx*dx + dy*dy;
        };

        int slotDet[2] = {-1, -1};  // slot → índice em dets[]

        if (numDets == 1) {
            bool assigned = false;
            // Tentar proximidade com slots ativos (dentro do limiar)
            if (prevSlotActive_[0] || prevSlotActive_[1]) {
                float bestDist = kMaxSlotDistSq;
                int bestSlot = -1;
                for (int s = 0; s < 2; ++s) {
                    if (!prevSlotActive_[s]) continue;
                    float d = wristDistSq(dets[0], prevLandmarks_[s]);
                    if (d < bestDist) { bestDist = d; bestSlot = s; }
                }
                if (bestSlot >= 0) {
                    slotDet[bestSlot] = 0;
                    assigned = true;
                }
            }
            if (!assigned) {
                // Sem match espacial — usar label bruto do MediaPipe
                int slot = (dets[0].hand == HandLabel::LEFT) ? 0 : 1;
                slotDet[slot] = 0;
            }
        } else if (numDets == 2) {
            bool anyActive = prevSlotActive_[0] || prevSlotActive_[1];
            if (anyActive) {
                // Testar ambas atribuições, escolher a de menor custo total.
                // A: det0→slot0, det1→slot1   B: det0→slot1, det1→slot0
                float costA = 0, costB = 0;
                for (int s = 0; s < 2; ++s) {
                    if (!prevSlotActive_[s]) continue;
                    costA += wristDistSq(dets[s],   prevLandmarks_[s]);
                    costB += wristDistSq(dets[1-s], prevLandmarks_[s]);
                }
                if (costA <= costB) {
                    slotDet[0] = 0; slotDet[1] = 1;
                } else {
                    slotDet[0] = 1; slotDet[1] = 0;
                }
            } else {
                // Sem histórico — usar labels brutos do MediaPipe
                int s0 = (dets[0].hand == HandLabel::LEFT) ? 0 : 1;
                int s1 = (dets[1].hand == HandLabel::LEFT) ? 0 : 1;
                if (s0 != s1) {
                    slotDet[s0] = 0;
                    slotDet[s1] = 1;
                } else {
                    // Ambas alegam o mesmo slot — desambiguar por posição X
                    // (menor X → LEFT = slot 0)
                    if (dets[0].landmarks[0].x <= dets[1].landmarks[0].x) {
                        slotDet[0] = 0; slotDet[1] = 1;
                    } else {
                        slotDet[0] = 1; slotDet[1] = 0;
                    }
                }
            }
        }

        // --- Suavizar e publicar ---
        for (int slot = 0; slot < 2; ++slot) {
            int di = slotDet[slot];
            if (di < 0) continue;
            HandLandmarks& raw = dets[di];
            raw.hand = (slot == 0) ? HandLabel::LEFT : HandLabel::RIGHT;
            HandLandmarks smoothed = prevSlotActive_[slot]
                ? smooth(raw, prevLandmarks_[slot]) : raw;
            prevLandmarks_[slot]  = smoothed;
            currSlotActive_[slot] = true;
            smoothed.timestamp    = vf.frame.timestamp;
            outputQueue_->push(smoothed);
            vf.landmarks[slot] = smoothed;
            ++landmarksDetected_;
        }
        MpHandLandmarkerCloseResult(&mp_result);
        ++framesProcessed_;

        if (vizQueue_) {
            if (!vizQueue_->push(std::move(vf)))
                AEROCHORD_LOG_DEBUG(kModule, "VizQueue cheia — frame descartado.");
        }
#endif
    }

    AEROCHORD_LOG_DEBUG(kModule, "detectionLoop() encerrado.");
}

} // namespace aerochord
