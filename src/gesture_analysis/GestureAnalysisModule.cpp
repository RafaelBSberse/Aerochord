#include "GestureAnalysisModule.h"
#include "common/Logger.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace aerochord {

static constexpr std::string_view kModule = "GestureAnalysisModule";

// ---------------------------------------------------------------------------
// Construção / destruição
// ---------------------------------------------------------------------------
GestureAnalysisModule::GestureAnalysisModule(
    std::shared_ptr<LandmarkQueue> inputQueue,
    std::shared_ptr<GestureQueue>  outputQueue)
    : GestureAnalysisModule(std::move(inputQueue), std::move(outputQueue), Config{}) {}

GestureAnalysisModule::GestureAnalysisModule(
    std::shared_ptr<LandmarkQueue> inputQueue,
    std::shared_ptr<GestureQueue>  outputQueue,
    Config config)
    : inputQueue_(std::move(inputQueue))
    , outputQueue_(std::move(outputQueue))
    , config_(config)
{
}

GestureAnalysisModule::~GestureAnalysisModule() {
    stop();
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------
bool GestureAnalysisModule::start() {
    if (running_.load())
        return true;

    AEROCHORD_LOG_INFO(kModule, "Iniciando análise de gestos (FSM)...");
    running_.store(true);
    thread_ = std::thread(&GestureAnalysisModule::analysisLoop, this);
    AEROCHORD_LOG_INFO(kModule, "Thread de análise iniciada.");
    return true;
}

void GestureAnalysisModule::stop() {
    if (!running_.exchange(false))
        return;

    if (thread_.joinable())
        thread_.join();

    AEROCHORD_LOG_INFO(kModule, "Análise de gestos encerrada.");
}

bool GestureAnalysisModule::isRunning() const { return running_.load(); }

FsmState GestureAnalysisModule::currentState() const { return fsmState_; }
uint64_t GestureAnalysisModule::gesturesEmitted() const { return gesturesEmitted_.load(); }

// ---------------------------------------------------------------------------
// Métricas de landmark
// ---------------------------------------------------------------------------

float GestureAnalysisModule::pinchDistance(const HandLandmarks& lm) {
    const auto& thumb = lm.at(LandmarkIndex::THUMB_TIP);
    const auto& index = lm.at(LandmarkIndex::INDEX_FINGER_TIP);
    const float dx = thumb.x - index.x;
    const float dy = thumb.y - index.y;
    const float dz = thumb.z - index.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float GestureAnalysisModule::middleFingerPalmDistance(const HandLandmarks& lm) {
    const auto& tip   = lm.at(LandmarkIndex::MIDDLE_FINGER_TIP);
    const auto& wrist = lm.at(LandmarkIndex::WRIST);
    const float dx = tip.x - wrist.x;
    const float dy = tip.y - wrist.y;
    return std::sqrt(dx * dx + dy * dy);
}

float GestureAnalysisModule::handTiltAngle(const HandLandmarks& lm) {
    // Vetor pulso → MCP do dedo médio.
    // Z (profundidade) é normalizado pela distância XY do mesmo segmento,
    // produzindo um valor adimensional em [-1, 1] independente da escala da mão.
    // Z positivo (MediaPipe): ponta dos dedos mais longe da câmera = inclinação para frente.
    const auto& wrist = lm.at(LandmarkIndex::WRIST);
    const auto& mcp   = lm.at(LandmarkIndex::MIDDLE_FINGER_MCP);
    const float dx    = mcp.x - wrist.x;
    const float dy    = mcp.y - wrist.y;
    const float dz    = mcp.z - wrist.z;
    const float ref   = std::sqrt(dx * dx + dy * dy);
    if (ref < 1e-5f) return 0.0f;
    return std::clamp(dz / ref, -1.0f, 1.0f);
}

float GestureAnalysisModule::lateralOscillation(const HandLandmarks& lm) {
    // Posição X do MCP do dedo médio como proxy da posição lateral da mão
    return lm.at(LandmarkIndex::MIDDLE_FINGER_MCP).x;
}

float GestureAnalysisModule::verticalPosition(const HandLandmarks& lm) {
    // Posição Y normalizada do pulso; MediaPipe: Y cresce para baixo → inverter
    return 1.0f - lm.at(LandmarkIndex::WRIST).y;
}

float GestureAnalysisModule::pinchVerticalPosition(const HandLandmarks& lm) {
    // Ponto médio Y entre polegar e indicador — altura onde a pinça efetivamente
    // ocorre. MediaPipe: Y cresce para baixo → inverter para mão alta = valor alto.
    const auto& thumb = lm.at(LandmarkIndex::THUMB_TIP);
    const auto& index = lm.at(LandmarkIndex::INDEX_FINGER_TIP);
    return 1.0f - (thumb.y + index.y) * 0.5f;
}

// ---------------------------------------------------------------------------
// Velocity via janela temporal (velocityWindowMs)
//
// Calcula a velocidade de fechamento da pinça usando amostras dentro da
// janela configurada, em vez de um único delta entre frames consecutivos.
// Isso suaviza a leitura e produz uma velocity mais estável.
// ---------------------------------------------------------------------------
float GestureAnalysisModule::computePinchVelocity(
    std::chrono::steady_clock::time_point now) const
{
    if (velocityWindowCount_ < 2)
        return 0.5f;  // sem histórico suficiente: velocity padrão

    // Encontrar a amostra mais antiga dentro da janela temporal
    const auto windowLimit = now - std::chrono::milliseconds(config_.velocityWindowMs);
    size_t oldest = velocityWindowCount_ - 1;
    for (size_t i = 0; i < velocityWindowCount_; ++i) {
        if (velocityWindow_[i].time >= windowLimit) {
            oldest = i;
            break;
        }
    }

    const float distOld = velocityWindow_[oldest].dist;
    const float distNew = velocityWindow_[velocityWindowCount_ - 1].dist;

    if (distOld < 1e-5f)
        return 0.5f;

    // velocity = quanto fechou relativo à distância inicial
    return std::clamp((distOld - distNew) / distOld, 0.0f, 1.0f);
}

void GestureAnalysisModule::recordPinchSample(float dist,
    std::chrono::steady_clock::time_point time)
{
    // Descartar amostras fora da janela temporal
    const auto windowLimit = time - std::chrono::milliseconds(config_.velocityWindowMs);
    size_t writeIdx = 0;
    for (size_t i = 0; i < velocityWindowCount_; ++i) {
        if (velocityWindow_[i].time >= windowLimit) {
            velocityWindow_[writeIdx++] = velocityWindow_[i];
        }
    }
    velocityWindowCount_ = writeIdx;

    // Adicionar nova amostra
    if (velocityWindowCount_ < kVelocityWindowMax) {
        velocityWindow_[velocityWindowCount_++] = { dist, time };
    } else {
        // Deslocar para liberar espaço
        for (size_t i = 1; i < kVelocityWindowMax; ++i)
            velocityWindow_[i - 1] = velocityWindow_[i];
        velocityWindow_[kVelocityWindowMax - 1] = { dist, time };
    }
}

// ---------------------------------------------------------------------------
// Processamento FSM — Mão Direita
// ---------------------------------------------------------------------------
void GestureAnalysisModule::processRightHand(const HandLandmarks& lm) {
    const float dist = pinchDistance(lm);
    recordPinchSample(dist, lm.timestamp);

    switch (fsmState_) {
        case FsmState::IDLE: {
            if (dist < config_.pinchThreshold) {
                if (++debounceCounter_ >= config_.debounceFrames) {
                    fsmState_        = FsmState::RIGHT_PINCH_START;
                    debounceCounter_ = 0;
                    AEROCHORD_LOG_DEBUG(kModule, "FSM: IDLE → RIGHT_PINCH_START");

                    GestureEvent ev;
                    ev.type          = GestureType::RIGHT_PINCH_START;
                    ev.hand          = HandLabel::RIGHT;
                    ev.primaryValue  = pinchVerticalPosition(lm);
                    ev.velocity      = computePinchVelocity(lm.timestamp);
                    ev.timestamp     = lm.timestamp;

                    outputQueue_->push(ev);
                    ++gesturesEmitted_;
                }
            } else {
                debounceCounter_ = 0;
            }
            break;
        }

        case FsmState::RIGHT_PINCH_START:
            AEROCHORD_LOG_DEBUG(kModule, "FSM: RIGHT_PINCH_START → RIGHT_PINCH_HOLD");
            [[fallthrough]];
        case FsmState::RIGHT_PINCH_HOLD: {
            fsmState_ = FsmState::RIGHT_PINCH_HOLD;

            // Emite eventos contínuos enquanto a pinça está ativa

            // RIGHT_PITCH_BEND — inclinação frente/trás da mão
            {
                GestureEvent ev;
                ev.type         = GestureType::RIGHT_PITCH_BEND;
                ev.hand         = HandLabel::RIGHT;
                ev.primaryValue = handTiltAngle(lm);
                ev.timestamp    = lm.timestamp;
                outputQueue_->push(ev);
                ++gesturesEmitted_;
            }

            // RIGHT_TIMBRE_CONTROL — distância dedo médio → palma
            {
                GestureEvent ev;
                ev.type         = GestureType::RIGHT_TIMBRE_CONTROL;
                ev.hand         = HandLabel::RIGHT;
                ev.primaryValue = middleFingerPalmDistance(lm);
                ev.timestamp    = lm.timestamp;
                outputQueue_->push(ev);
                ++gesturesEmitted_;
            }

            // RIGHT_VIBRATO — taxa de oscilação lateral (delta de posição X/frame)
            {
                const float curX  = lateralOscillation(lm);
                const float delta = std::abs(curX - prevLateralX_);
                prevLateralX_ = curX;
                if (delta > config_.vibratoThreshold) {
                    GestureEvent ev;
                    ev.type         = GestureType::RIGHT_VIBRATO;
                    ev.hand         = HandLabel::RIGHT;
                    ev.primaryValue = std::min(delta / 0.1f, 1.0f);  // normaliza [0,1]
                    ev.timestamp    = lm.timestamp;
                    outputQueue_->push(ev);
                    ++gesturesEmitted_;
                }
            }

            // RIGHT_PINCH_POSITION — posição vertical da pinça para legato zone tracking
            {
                GestureEvent ev;
                ev.type         = GestureType::RIGHT_PINCH_POSITION;
                ev.hand         = HandLabel::RIGHT;
                ev.primaryValue = pinchVerticalPosition(lm);
                ev.timestamp    = lm.timestamp;
                outputQueue_->push(ev);
                ++gesturesEmitted_;
            }

            if (dist > config_.pinchThreshold + config_.pinchReleaseMargin) {
                if (++debounceCounter_ >= config_.debounceFrames) {
                    fsmState_        = FsmState::IDLE;
                    debounceCounter_ = 0;
                    AEROCHORD_LOG_DEBUG(kModule, "FSM: RIGHT_PINCH_HOLD → IDLE (release)");

                    GestureEvent ev;
                    ev.type      = GestureType::RIGHT_PINCH_RELEASE;
                    ev.hand      = HandLabel::RIGHT;
                    ev.timestamp = lm.timestamp;

                    outputQueue_->push(ev);
                    ++gesturesEmitted_;
                }
            } else {
                debounceCounter_ = 0;
            }
            break;
        }

        default:
            break;
    }

    prevPinchDist_ = dist;
}

// ---------------------------------------------------------------------------
// Processamento FSM — Mão Esquerda
// ---------------------------------------------------------------------------
void GestureAnalysisModule::processLeftHand(const HandLandmarks& lm) {
    const float dist  = pinchDistance(lm);
    const float vertY = verticalPosition(lm);

    if (dist < config_.pinchThreshold) {
        // Pinça esquerda → seleção de oitava
        // Emite somente quando há movimento vertical intencional (com debounce),
        // evitando disparos repetidos por repouso.
        const float dy = vertY - prevLeftY_;
        prevLeftY_ = vertY;

        if (std::abs(dy) > kOctaveVelocityThreshold) {
            ++leftOctaveDebounce_;
        } else {
            leftOctaveDebounce_ = 0;
        }

        if (leftOctaveDebounce_ >= config_.debounceFrames) {
            leftOctaveDebounce_ = 0;
            GestureEvent ev;
            ev.type         = GestureType::LEFT_OCTAVE_SELECT;
            ev.hand         = HandLabel::LEFT;
            ev.primaryValue = (dy > 0.0f) ? +1.0f : -1.0f;  // +1 = subir, -1 = descer
            ev.timestamp    = lm.timestamp;
            outputQueue_->push(ev);
            ++gesturesEmitted_;
        }
    } else {
        // Mão aberta — volume contínuo + detecção de sustain
        leftOctaveDebounce_ = 0;

        const float dy = std::abs(vertY - prevLeftY_);
        prevLeftY_ = vertY;

        // Emitir volume continuamente (CC#7)
        {
            GestureEvent ev;
            ev.type         = GestureType::LEFT_VOLUME_CONTROL;
            ev.hand         = HandLabel::LEFT;
            ev.primaryValue = vertY;
            ev.timestamp    = lm.timestamp;
            outputQueue_->push(ev);
            ++gesturesEmitted_;
        }

        // Detectar sustain: mão parada por N frames consecutivos → toggle CC#64
        if (dy < kStationaryThreshold) {
            ++leftStationaryCount_;
            if (leftStationaryCount_ == kSustainFrames) {
                sustainActive_ = !sustainActive_;
                GestureEvent ev;
                ev.type         = GestureType::LEFT_SUSTAIN;
                ev.hand         = HandLabel::LEFT;
                ev.primaryValue = sustainActive_ ? 1.0f : 0.0f;
                ev.timestamp    = lm.timestamp;
                outputQueue_->push(ev);
                ++gesturesEmitted_;
            }
        } else {
            leftStationaryCount_ = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Loop principal de análise (thread dedicada)
// ---------------------------------------------------------------------------
void GestureAnalysisModule::analysisLoop() {
    AEROCHORD_LOG_DEBUG(kModule, "analysisLoop() iniciado.");

    lastRightHandTime_ = std::chrono::steady_clock::now();

    while (running_.load()) {
        auto opt = inputQueue_->pop();
        if (!opt) {
            // Verificar timeout: se a mão direita sumiu e há nota ativa,
            // emitir release para evitar stuck notes.
            if (fsmState_ == FsmState::RIGHT_PINCH_HOLD ||
                fsmState_ == FsmState::RIGHT_PINCH_START)
            {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastRightHandTime_).count();
                if (elapsed >= kHandLostTimeoutMs) {
                    fsmState_        = FsmState::IDLE;
                    debounceCounter_ = 0;
                    AEROCHORD_LOG_DEBUG(kModule,
                        "FSM: PINCH → IDLE (mão direita perdida por " +
                        std::to_string(elapsed) + "ms)");

                    GestureEvent ev;
                    ev.type      = GestureType::RIGHT_PINCH_RELEASE;
                    ev.hand      = HandLabel::RIGHT;
                    ev.timestamp = now;
                    outputQueue_->push(ev);
                    ++gesturesEmitted_;
                }
            }
            std::this_thread::yield();
            continue;
        }

        const HandLandmarks& lm = *opt;

        if (lm.confidence < config_.minConfidence) {
            // Detecção de baixa confiança — fallback: manter estado atual da FSM
            // por até kMaxLowConfidenceFrames frames consecutivos (cap 5.2.3 Desafio 2).
            // Isso evita gaps audíveis durante perda temporária de detecção.
            ++lowConfidenceStreak_;
            if (lowConfidenceStreak_ >= kMaxLowConfidenceFrames) {
                // Perda prolongada — resetar debounce para evitar acumular
                // frames não-consecutivos quando a detecção retornar.
                debounceCounter_ = 0;
                leftOctaveDebounce_ = 0;

                // Se havia nota ativa (pinça sustentada), emitir release
                // para evitar stuck notes quando a mão sai da câmera.
                if (fsmState_ == FsmState::RIGHT_PINCH_HOLD ||
                    fsmState_ == FsmState::RIGHT_PINCH_START)
                {
                    fsmState_ = FsmState::IDLE;
                    AEROCHORD_LOG_DEBUG(kModule,
                        "FSM: PINCH → IDLE (detecção perdida)");

                    GestureEvent ev;
                    ev.type      = GestureType::RIGHT_PINCH_RELEASE;
                    ev.hand      = HandLabel::RIGHT;
                    ev.timestamp = lm.timestamp;
                    outputQueue_->push(ev);
                    ++gesturesEmitted_;
                }
            }
            continue;
        }

        lowConfidenceStreak_ = 0;  // detecção válida — resetar streak

        if (lm.hand == HandLabel::RIGHT) {
            lastRightHandTime_ = std::chrono::steady_clock::now();
            processRightHand(lm);
        } else if (lm.hand == HandLabel::LEFT)
            processLeftHand(lm);
    }

    AEROCHORD_LOG_DEBUG(kModule, "analysisLoop() encerrado.");
}

} // namespace aerochord
