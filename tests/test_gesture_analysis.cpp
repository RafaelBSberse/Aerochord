#include <gtest/gtest.h>
#include "gesture_analysis/GestureAnalysisModule.h"
#include "common/PipelineTypes.h"

#include <chrono>
#include <optional>
#include <thread>

using namespace aerochord;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Constrói HandLandmarks com THUMB_TIP e INDEX_FINGER_TIP separados por |thumbX - idxX|
// Demais landmarks posicionados no centro da tela (0.5, y).
static HandLandmarks makeHand(HandLabel hand, float thumbX, float idxX, float y = 0.5f,
                               float confidence = 1.0f) {
    HandLandmarks lm;
    lm.hand       = hand;
    lm.confidence = confidence;
    lm.timestamp  = std::chrono::steady_clock::now();

    // Inicializar todos os pontos no centro
    for (auto& pt : lm.landmarks)
        pt = { 0.5f, y, 0.0f, 1.0f };

    // Posicionar polegar e indicador para controlar a distância de pinça
    lm.landmarks[static_cast<size_t>(LandmarkIndex::THUMB_TIP)]        = { thumbX, y, 0.0f, 1.0f };
    lm.landmarks[static_cast<size_t>(LandmarkIndex::INDEX_FINGER_TIP)] = { idxX,   y, 0.0f, 1.0f };
    return lm;
}

// Tenta fazer pop da fila de gestos com polling de até maxWaitMs
static std::optional<GestureEvent> waitPop(std::shared_ptr<GestureQueue> q,
                                            int maxWaitMs = 100) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(maxWaitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto ev = q->pop();
        if (ev) return ev;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

// Drena todos os eventos presentes na fila (sem esperar)
static void drain(std::shared_ptr<GestureQueue> q) {
    while (q->pop()) {}
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class GestureAnalysisTest : public ::testing::Test {
protected:
    std::shared_ptr<LandmarkQueue>  landmarkQ;
    std::shared_ptr<GestureQueue>   gestureQ;
    std::unique_ptr<GestureAnalysisModule> module;

    void SetUp() override {
        landmarkQ = std::make_shared<LandmarkQueue>();
        gestureQ  = std::make_shared<GestureQueue>();

        GestureAnalysisModule::Config cfg;
        cfg.debounceFrames     = 1;    // sem espera de múltiplos frames
        cfg.pinchThreshold     = 0.05f;
        cfg.pinchReleaseMargin = 0.02f;

        module = std::make_unique<GestureAnalysisModule>(landmarkQ, gestureQ, cfg);
        module->start();
    }

    void TearDown() override {
        module->stop();
    }
};

// ---------------------------------------------------------------------------
// Testes de mão direita
// ---------------------------------------------------------------------------

TEST_F(GestureAnalysisTest, RightPinchStartEmitted) {
    // Distância de pinça ~0.02 (< threshold 0.05) → deve emitir RIGHT_PINCH_START
    landmarkQ->push(makeHand(HandLabel::RIGHT, 0.49f, 0.51f));

    auto ev = waitPop(gestureQ);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, GestureType::RIGHT_PINCH_START);
    EXPECT_EQ(ev->hand, HandLabel::RIGHT);
}

TEST_F(GestureAnalysisTest, RightPinchHoldEmitsContinuousEvents) {
    // Primeiro frame: pinça fechada → RIGHT_PINCH_START
    landmarkQ->push(makeHand(HandLabel::RIGHT, 0.49f, 0.51f));
    auto ev1 = waitPop(gestureQ);
    ASSERT_TRUE(ev1.has_value());
    EXPECT_EQ(ev1->type, GestureType::RIGHT_PINCH_START);

    drain(gestureQ);

    // Segundo frame com pinça ainda fechada → deve emitir eventos contínuos (PITCH_BEND, TIMBRE_CONTROL)
    landmarkQ->push(makeHand(HandLabel::RIGHT, 0.49f, 0.51f));
    auto ev2 = waitPop(gestureQ);
    ASSERT_TRUE(ev2.has_value());
    // Qualquer evento contínuo é válido
    EXPECT_TRUE(ev2->type == GestureType::RIGHT_PITCH_BEND ||
                ev2->type == GestureType::RIGHT_TIMBRE_CONTROL ||
                ev2->type == GestureType::RIGHT_VIBRATO);
}

TEST_F(GestureAnalysisTest, RightPinchReleaseEmitted) {
    // 1. Fechar pinça
    landmarkQ->push(makeHand(HandLabel::RIGHT, 0.49f, 0.51f));
    auto start = waitPop(gestureQ);
    ASSERT_TRUE(start.has_value());
    EXPECT_EQ(start->type, GestureType::RIGHT_PINCH_START);
    drain(gestureQ);

    // 2. Abrir pinça (distância ~0.4 >> threshold+margin)
    landmarkQ->push(makeHand(HandLabel::RIGHT, 0.3f, 0.7f));

    // Pode ter eventos contínuos antes do release; aguardar o RELEASE
    GestureType lastType = GestureType::NONE;
    for (int i = 0; i < 20; ++i) {
        auto ev = waitPop(gestureQ, 20);
        if (!ev) break;
        lastType = ev->type;
        if (lastType == GestureType::RIGHT_PINCH_RELEASE) break;
    }
    EXPECT_EQ(lastType, GestureType::RIGHT_PINCH_RELEASE);
}

TEST_F(GestureAnalysisTest, LowConfidenceIgnored) {
    // Landmarks com confidence < 0.3 devem ser ignorados
    landmarkQ->push(makeHand(HandLabel::RIGHT, 0.49f, 0.51f, 0.5f, 0.1f));

    // Aguardar 60 ms — nenhum evento deve aparecer
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_FALSE(gestureQ->pop().has_value());
}

TEST_F(GestureAnalysisTest, OpenHandNoRightPinch) {
    // Mão direita aberta (distância grande) → não emite pinça
    landmarkQ->push(makeHand(HandLabel::RIGHT, 0.2f, 0.8f));

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_FALSE(gestureQ->pop().has_value());
}

// ---------------------------------------------------------------------------
// Testes de mão esquerda
// ---------------------------------------------------------------------------

TEST_F(GestureAnalysisTest, LeftVolumeControlEmitted) {
    // Mão esquerda aberta (distância grande) → LEFT_VOLUME_CONTROL contínuo
    landmarkQ->push(makeHand(HandLabel::LEFT, 0.2f, 0.8f));

    auto ev = waitPop(gestureQ);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, GestureType::LEFT_VOLUME_CONTROL);
    EXPECT_EQ(ev->hand, HandLabel::LEFT);
}

TEST_F(GestureAnalysisTest, LeftOctaveSelectNotFiredAtRest) {
    // Mão esquerda com pinça mas sem movimento → não deve emitir LEFT_OCTAVE_SELECT
    const float y = 0.5f;
    for (int i = 0; i < 5; ++i)
        landmarkQ->push(makeHand(HandLabel::LEFT, 0.49f, 0.51f, y));

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // Drenar fila; verificar que nenhum LEFT_OCTAVE_SELECT foi emitido
    bool octaveFound = false;
    while (auto ev = gestureQ->pop()) {
        if (ev->type == GestureType::LEFT_OCTAVE_SELECT)
            octaveFound = true;
    }
    EXPECT_FALSE(octaveFound);
}

TEST_F(GestureAnalysisTest, LeftOctaveSelectUpEmitted) {
    // Frame 1: posição inicial
    landmarkQ->push(makeHand(HandLabel::LEFT, 0.49f, 0.51f, 0.5f));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    drain(gestureQ);

    // Frame 2: movimento vertical significativo para cima (Y aumenta = subir na tela)
    // verticalPosition inverte Y: 1 - wrist.y
    // Para subir oitava, dy > 0 → verticalPosition aumenta → wrist.y diminui
    landmarkQ->push(makeHand(HandLabel::LEFT, 0.49f, 0.51f, 0.5f - 0.05f));

    // Buscar LEFT_OCTAVE_SELECT entre os eventos gerados
    bool found = false;
    float pv   = 0.0f;
    for (int i = 0; i < 20; ++i) {
        auto ev = waitPop(gestureQ, 20);
        if (!ev) break;
        if (ev->type == GestureType::LEFT_OCTAVE_SELECT) {
            found = true;
            pv    = ev->primaryValue;
            break;
        }
    }
    ASSERT_TRUE(found) << "LEFT_OCTAVE_SELECT não foi emitido";
    EXPECT_GT(pv, 0.0f);  // subir oitava → +1
}

// ---------------------------------------------------------------------------
// Timeout: mão desaparece durante pinch hold → release automático
// ---------------------------------------------------------------------------

TEST_F(GestureAnalysisTest, HandLostDuringPinchEmitsRelease) {
    // Fechar pinça → RIGHT_PINCH_START
    landmarkQ->push(makeHand(HandLabel::RIGHT, 0.49f, 0.51f));
    auto ev = waitPop(gestureQ);
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->type, GestureType::RIGHT_PINCH_START);
    drain(gestureQ);

    // Simular mão desaparecendo: não enviar mais landmarks por > 200ms
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // O timeout deve ter emitido RIGHT_PINCH_RELEASE automaticamente
    bool foundRelease = false;
    for (int i = 0; i < 10; ++i) {
        auto e = waitPop(gestureQ, 20);
        if (!e) break;
        if (e->type == GestureType::RIGHT_PINCH_RELEASE) {
            foundRelease = true;
            break;
        }
    }
    EXPECT_TRUE(foundRelease) << "RIGHT_PINCH_RELEASE deveria ser emitido quando mão desaparece";
    EXPECT_EQ(module->currentState(), FsmState::IDLE);
}
