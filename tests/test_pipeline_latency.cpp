// test_pipeline_latency.cpp
//
// Valida dois requisitos do plano de implementação (Fase 3):
//   1. Timestamps são preservados de GestureEvent → MidiCommand.
//   2. A latência de processamento do MappingModule está abaixo de 5 ms (P95).
//   3. O encadeamento GestureAnalysis + Mapping adiciona menos de 10 ms (P95).
//
// Esses testes não exigem câmera nem dispositivo MIDI — cobrem a fatia de
// processamento interno do pipeline (gestos reconhecidos → comando enviado).

#include <gtest/gtest.h>
#include "gesture_analysis/GestureAnalysisModule.h"
#include "mapping/MappingModule.h"
#include "common/PipelineTypes.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

using namespace aerochord;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GestureEvent makeGesture(GestureType type, float primary = 0.5f,
                                 float vel = 0.8f) {
    GestureEvent ev;
    ev.type         = type;
    ev.hand         = (type == GestureType::LEFT_VOLUME_CONTROL ||
                       type == GestureType::LEFT_OCTAVE_SELECT)
                          ? HandLabel::LEFT : HandLabel::RIGHT;
    ev.primaryValue = primary;
    ev.velocity     = vel;
    ev.timestamp    = steady_clock::now();
    return ev;
}

// Polling com timeout; retorna nullopt se esgotado.
template<typename T, size_t Cap>
static std::optional<T> waitPop(std::shared_ptr<LockFreeQueue<T, Cap>> q,
                                 int maxWaitMs = 200) {
    const auto deadline = steady_clock::now() + milliseconds(maxWaitMs);
    while (steady_clock::now() < deadline) {
        if (auto v = q->pop()) return v;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return std::nullopt;
}

// Constrói HandLandmarks com distância de pinça controlada.
// thumbX e idxX no mesmo Y → dist = |thumbX - idxX|.
static HandLandmarks makePinchedHand(HandLabel hand, float thumbX, float idxX,
                                      float y = 0.5f) {
    HandLandmarks lm;
    lm.hand       = hand;
    lm.confidence = 0.95f;
    lm.timestamp  = steady_clock::now();
    for (auto& pt : lm.landmarks) pt = { 0.5f, y, 0.0f, 1.0f };
    lm.landmarks[static_cast<size_t>(LandmarkIndex::THUMB_TIP)]        = { thumbX, y, 0.0f, 1.0f };
    lm.landmarks[static_cast<size_t>(LandmarkIndex::INDEX_FINGER_TIP)] = { idxX,   y, 0.0f, 1.0f };
    return lm;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

class MappingLatencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        gQueue = std::make_shared<GestureQueue>();
        mQueue = std::make_shared<MidiQueue>();
        MappingModule::Config cfg;
        cfg.defaultOctave = 4;
        mapping = std::make_unique<MappingModule>(gQueue, mQueue, cfg);
        ASSERT_TRUE(mapping->start());
    }
    void TearDown() override { mapping->stop(); }

    std::shared_ptr<GestureQueue> gQueue;
    std::shared_ptr<MidiQueue>    mQueue;
    std::unique_ptr<MappingModule> mapping;
};

class FullChainLatencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        lmQueue = std::make_shared<LandmarkQueue>();
        gQueue  = std::make_shared<GestureQueue>();
        mQueue  = std::make_shared<MidiQueue>();

        GestureAnalysisModule::Config gaCfg;
        gaCfg.debounceFrames = 1;  // sem debounce para medir latência mínima
        gesture = std::make_unique<GestureAnalysisModule>(lmQueue, gQueue, gaCfg);

        MappingModule::Config mapCfg;
        mapCfg.defaultOctave = 4;
        mapping = std::make_unique<MappingModule>(gQueue, mQueue, mapCfg);

        ASSERT_TRUE(mapping->start());
        ASSERT_TRUE(gesture->start());
    }
    void TearDown() override {
        gesture->stop();
        mapping->stop();
    }

    std::shared_ptr<LandmarkQueue>         lmQueue;
    std::shared_ptr<GestureQueue>          gQueue;
    std::shared_ptr<MidiQueue>             mQueue;
    std::unique_ptr<GestureAnalysisModule> gesture;
    std::unique_ptr<MappingModule>         mapping;
};

// ---------------------------------------------------------------------------
// Testes de preservação de timestamp
// ---------------------------------------------------------------------------

TEST_F(MappingLatencyTest, TimestampPreservedFromGestureToMidi) {
    auto ev = makeGesture(GestureType::RIGHT_PINCH_START, 0.5f, 0.8f);
    const auto expectedTs = ev.timestamp;
    ASSERT_TRUE(gQueue->push(ev));

    auto cmd = waitPop(mQueue);
    ASSERT_TRUE(cmd.has_value()) << "MidiCommand não recebido dentro do timeout";
    EXPECT_EQ(cmd->timestamp, expectedTs)
        << "Timestamp deve ser preservado de GestureEvent para MidiCommand";
}

TEST_F(MappingLatencyTest, TimestampPreservedForContinuousEvents) {
    // Pitch bend e CC também devem preservar o timestamp original.
    auto ev = makeGesture(GestureType::RIGHT_PITCH_BEND, 0.3f);
    const auto expectedTs = ev.timestamp;
    ASSERT_TRUE(gQueue->push(ev));

    auto cmd = waitPop(mQueue);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->timestamp, expectedTs);
}

// ---------------------------------------------------------------------------
// Testes de latência de processamento do MappingModule
// ---------------------------------------------------------------------------

TEST_F(MappingLatencyTest, MappingProcessingP95BelowThreshold) {
    // Envia 200 eventos e mede o tempo entre push e recepção do MidiCommand.
    // P95 deve estar abaixo de 5 ms — cobre apenas o mapeamento (sem câmera/MediaPipe).
    constexpr int kEvents = 200;
    constexpr float kP95ThresholdMs = 5.0f;

    std::vector<float> latencies;
    latencies.reserve(kEvents);

    for (int i = 0; i < kEvents; ++i) {
        // Alternar entre NOTE_ON e NOTE_OFF para exercitar os dois translators
        GestureType type = (i % 2 == 0)
            ? GestureType::RIGHT_PINCH_START
            : GestureType::RIGHT_PINCH_RELEASE;

        auto ev = makeGesture(type, 0.5f, 0.8f);

        const auto t0 = steady_clock::now();
        ASSERT_TRUE(gQueue->push(ev)) << "Fila de gestos cheia no evento " << i;

        auto cmd = waitPop(mQueue, 50);
        const auto t1 = steady_clock::now();

        ASSERT_TRUE(cmd.has_value()) << "Timeout esperando MidiCommand no evento " << i;
        latencies.push_back(duration<float, std::milli>(t1 - t0).count());
    }

    std::sort(latencies.begin(), latencies.end());
    const float p50 = latencies[kEvents / 2];
    const float p95 = latencies[static_cast<size_t>(kEvents * 0.95)];

    EXPECT_LT(p95, kP95ThresholdMs)
        << "Latência P95 do MappingModule (" << p95 << " ms) excede limite de "
        << kP95ThresholdMs << " ms. P50=" << p50 << " ms.";
}

TEST_F(MappingLatencyTest, VolumeCurveApplied) {
    // Verifica que volumeCurve ≠ 1.0 altera o valor do CC#7.
    // Config padrão: volumeCurve = 1.0 (linear).
    auto evLinear = makeGesture(GestureType::LEFT_VOLUME_CONTROL, 0.5f);
    ASSERT_TRUE(gQueue->push(evLinear));
    auto cmdLinear = waitPop(mQueue);
    ASSERT_TRUE(cmdLinear.has_value());
    ASSERT_EQ(cmdLinear->controlNumber, 7u);

    // Valor esperado com curva linear: 0.5 * 0xFFFFFFFF ≈ 0x7FFFFFFF
    const uint32_t linearVal = cmdLinear->controlValue;
    EXPECT_GT(linearVal, 0u);

    // Criar novo módulo com volumeCurve = 2.0 (quadrática → valor menor para 0.5)
    mapping->stop();
    MappingModule::Config cfg2;
    cfg2.defaultOctave = 4;
    cfg2.volumeCurve   = 2.0f;
    mapping = std::make_unique<MappingModule>(gQueue, mQueue, cfg2);
    ASSERT_TRUE(mapping->start());

    auto evCurved = makeGesture(GestureType::LEFT_VOLUME_CONTROL, 0.5f);
    ASSERT_TRUE(gQueue->push(evCurved));
    auto cmdCurved = waitPop(mQueue);
    ASSERT_TRUE(cmdCurved.has_value());

    // pow(0.5, 2.0) = 0.25 < 0.5 → controlValue deve ser menor que linearVal
    EXPECT_LT(cmdCurved->controlValue, linearVal)
        << "volumeCurve=2.0 deve reduzir o controlValue para primaryValue=0.5";
}

// ---------------------------------------------------------------------------
// Teste de latência da cadeia GestureAnalysis + Mapping
// ---------------------------------------------------------------------------

TEST_F(FullChainLatencyTest, GestureToMidiChainP95BelowThreshold) {
    // Mede a latência da cadeia landmark → gesto → MidiCommand.
    // P95 deve ser abaixo de 10 ms (processamento puro, sem câmera).
    constexpr int   kEvents = 100;
    constexpr float kP95ThresholdMs = 10.0f;

    std::vector<float> latencies;
    latencies.reserve(kEvents);

    // Alternar entre mão aberta e pinçada para exercitar transições da FSM.
    // Usar debounceFrames=1 (configurado no SetUp) para medir latência mínima.
    for (int i = 0; i < kEvents; ++i) {
        HandLandmarks lm;
        if (i % 4 < 2) {
            // Mão aberta → LEFT_VOLUME_CONTROL (contínuo, sem debounce na mão esquerda)
            lm = makePinchedHand(HandLabel::LEFT, 0.0f, 0.5f);  // dist = 0.5 > threshold
        } else {
            // Pinça esquerda → aguarda LEFT_OCTAVE_SELECT (com debounce)
            lm = makePinchedHand(HandLabel::LEFT, 0.48f, 0.50f); // dist = 0.02 < threshold
            // Mover Y para simular gesto vertical (necessário para OCTAVE_SELECT)
            lm.landmarks[0].y = (i % 2 == 0) ? 0.3f : 0.7f;
        }
        lm.timestamp = steady_clock::now();

        const auto t0 = steady_clock::now();
        ASSERT_TRUE(lmQueue->push(lm)) << "Fila de landmarks cheia no evento " << i;

        auto cmd = waitPop(mQueue, 100);
        const auto t1 = steady_clock::now();

        if (!cmd.has_value()) continue;  // OCTAVE_SELECT não gera MidiCommand
        latencies.push_back(duration<float, std::milli>(t1 - t0).count());
    }

    ASSERT_GT(latencies.size(), 0u) << "Nenhum MidiCommand recebido — pipeline não funcionou";

    std::sort(latencies.begin(), latencies.end());
    const float p50 = latencies[latencies.size() / 2];
    const float p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];

    EXPECT_LT(p95, kP95ThresholdMs)
        << "Latência P95 da cadeia GestureAnalysis+Mapping (" << p95
        << " ms) excede limite de " << kP95ThresholdMs
        << " ms. P50=" << p50 << " ms.";
}
