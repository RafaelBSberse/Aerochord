#include <gtest/gtest.h>
#include "mapping/MappingModule.h"
#include "common/PipelineTypes.h"

#include <chrono>
#include <optional>
#include <thread>

using namespace aerochord;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GestureEvent makeGesture(GestureType type, float primaryValue = 0.5f,
                                 float velocity = 0.8f) {
    GestureEvent ev;
    ev.type         = type;
    ev.hand         = (type == GestureType::LEFT_OCTAVE_SELECT ||
                       type == GestureType::LEFT_VOLUME_CONTROL ||
                       type == GestureType::LEFT_SUSTAIN)
                          ? HandLabel::LEFT
                          : HandLabel::RIGHT;
    ev.primaryValue = primaryValue;
    ev.velocity     = velocity;
    ev.timestamp    = std::chrono::steady_clock::now();
    return ev;
}

// Envia evento e aguarda MidiCommand na fila de saída (polling até maxWaitMs)
static std::optional<MidiCommand> pushAndWait(
    std::shared_ptr<GestureQueue> gq,
    std::shared_ptr<MidiQueue>    mq,
    GestureEvent ev,
    int maxWaitMs = 100)
{
    gq->push(ev);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(maxWaitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto cmd = mq->pop();
        if (cmd) return cmd;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

static void drain(std::shared_ptr<MidiQueue> q) {
    while (q->pop()) {}
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class MappingTest : public ::testing::Test {
protected:
    std::shared_ptr<GestureQueue> gestureQ;
    std::shared_ptr<MidiQueue>    midiQ;
    std::unique_ptr<MappingModule> module;

    void SetUp() override {
        gestureQ = std::make_shared<GestureQueue>();
        midiQ    = std::make_shared<MidiQueue>();

        MappingModule::Config cfg;
        cfg.midiChannel   = 0;
        cfg.defaultOctave = 4;  // Dó central: oitava 4, nota 60 + semitom
        cfg.zonePadding   = 0.0f;  // sem padding: zonas ocupam todo [0, 1] nos testes legados

        module = std::make_unique<MappingModule>(gestureQ, midiQ, cfg);
        module->start();
    }

    void TearDown() override {
        module->stop();
    }
};

// ---------------------------------------------------------------------------
// NOTE_ON / NOTE_OFF
// ---------------------------------------------------------------------------

TEST_F(MappingTest, PinchStartEmitsNoteOn) {
    // primaryValue=0.5 → semitom 6 de 12 → nota = oitava*12 + 6 = 48+6 = 54
    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.5f));
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::NOTE_ON);
    EXPECT_EQ(cmd->channel, 0u);
    EXPECT_GE(cmd->noteNumber, 0u);
    EXPECT_LE(cmd->noteNumber, 127u);
}

TEST_F(MappingTest, PinchReleaseEmitsNoteOff) {
    // Primeiro, criar nota ativa
    pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.5f));
    drain(midiQ);

    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_RELEASE));
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::NOTE_OFF);
}

TEST_F(MappingTest, NoteOffUsesActiveNote) {
    // NOTE_ON com primaryValue=0.5
    auto noteOn = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.5f));
    ASSERT_TRUE(noteOn.has_value());
    const uint8_t expectedNote = noteOn->noteNumber;
    drain(midiQ);

    // NOTE_OFF deve usar o mesmo número de nota
    auto noteOff = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_RELEASE));
    ASSERT_TRUE(noteOff.has_value());
    EXPECT_EQ(noteOff->noteNumber, expectedNote);
}

// ---------------------------------------------------------------------------
// Oitava
// ---------------------------------------------------------------------------

TEST_F(MappingTest, OctaveSelectUpIncrements) {
    EXPECT_EQ(module->currentOctave(), 4);

    // Oitava +1 (primaryValue=+1.0 → delta +1)
    gestureQ->push(makeGesture(GestureType::LEFT_OCTAVE_SELECT, +1.0f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(module->currentOctave(), 5);
}

TEST_F(MappingTest, OctaveSelectDownDecrements) {
    gestureQ->push(makeGesture(GestureType::LEFT_OCTAVE_SELECT, -1.0f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(module->currentOctave(), 3);
}

TEST_F(MappingTest, OctaveSelectClampedAtMax) {
    // Forçar octave para 10 e tentar subir mais (base=4, +7 = 11 → clamped em 10)
    for (int i = 0; i < 7; ++i) {
        gestureQ->push(makeGesture(GestureType::LEFT_OCTAVE_SELECT, +1.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(module->currentOctave(), 10);  // clamped em 10
}

TEST_F(MappingTest, OctaveSelectClampedAtMin) {
    for (int i = 0; i < 6; ++i) {
        gestureQ->push(makeGesture(GestureType::LEFT_OCTAVE_SELECT, -1.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(module->currentOctave(), 0);  // clamped em 0
}

TEST_F(MappingTest, OctaveAffectsNoteNumber) {
    // Nota na oitava 4, primaryValue=0 → semitom 0 → nota = 4*12 = 48
    auto cmd4 = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.0f));
    ASSERT_TRUE(cmd4.has_value());
    const uint8_t note4 = cmd4->noteNumber;
    drain(midiQ);

    // Subir oitava → nota deve ser 12 semitons acima
    gestureQ->push(makeGesture(GestureType::LEFT_OCTAVE_SELECT, +1.0f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // O segundo NOTE_ON gera um NOTE_OFF preventivo (stuck note guard)
    // seguido do NOTE_ON real. Buscar o NOTE_ON por polling.
    gestureQ->push(makeGesture(GestureType::RIGHT_PINCH_START, 0.0f));

    std::optional<MidiCommand> cmd5;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
        auto c = midiQ->pop();
        if (c && c->type == MidiCommandType::NOTE_ON) {
            cmd5 = c;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(cmd5.has_value()) << "NOTE_ON não recebido após octave change";
    EXPECT_EQ(cmd5->noteNumber, note4 + 12);
}

// ---------------------------------------------------------------------------
// Control Change
// ---------------------------------------------------------------------------

TEST_F(MappingTest, VolumeControlEmitsCC7) {
    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::LEFT_VOLUME_CONTROL, 0.7f));
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::CONTROL_CHANGE);
    EXPECT_EQ(cmd->controlNumber, 7u);  // CC#7 = Channel Volume
}

TEST_F(MappingTest, TimbreControlEmitsCC74) {
    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_TIMBRE_CONTROL, 0.5f));
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::CONTROL_CHANGE);
    EXPECT_EQ(cmd->controlNumber, 74u);  // CC#74 = Brightness
}

TEST_F(MappingTest, VibratoEmitsCC1) {
    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_VIBRATO, 0.4f));
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::CONTROL_CHANGE);
    EXPECT_EQ(cmd->controlNumber, 1u);  // CC#1 = Modulation
}

// ---------------------------------------------------------------------------
// Pitch Bend
// ---------------------------------------------------------------------------

TEST_F(MappingTest, PitchBendWithoutActiveNoteFallsToChannelBend) {
    // Sem nota ativa: PER_NOTE_PITCH_BEND cai para PITCH_BEND (channel-wide)
    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PITCH_BEND, 0.5f));
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::PITCH_BEND);
}

TEST_F(MappingTest, PitchBendWithActiveNoteEmitsPerNotePitchBend) {
    // Criar nota ativa
    auto noteOn = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.5f));
    ASSERT_TRUE(noteOn.has_value());
    const uint8_t activeNote = noteOn->noteNumber;
    drain(midiQ);

    // Pitch bend com nota ativa → PER_NOTE_PITCH_BEND com noteNumber correto
    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PITCH_BEND, 0.3f));
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::PER_NOTE_PITCH_BEND);
    EXPECT_EQ(cmd->noteNumber, activeNote);
}

// ---------------------------------------------------------------------------
// Diagnóstico: commandsEmitted incrementa
// ---------------------------------------------------------------------------

TEST_F(MappingTest, CommandsEmittedCounter) {
    const uint64_t before = module->commandsEmitted();

    pushAndWait(gestureQ, midiQ, makeGesture(GestureType::LEFT_VOLUME_CONTROL, 0.5f));
    pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_TIMBRE_CONTROL, 0.5f));

    EXPECT_GT(module->commandsEmitted(), before);
}

// ---------------------------------------------------------------------------
// Sustain (CC#64)
// ---------------------------------------------------------------------------

TEST_F(MappingTest, SustainOnEmitsCC64) {
    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::LEFT_SUSTAIN, 1.0f));
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::CONTROL_CHANGE);
    EXPECT_EQ(cmd->controlNumber, 64u);  // CC#64 = Sustain Pedal
    EXPECT_GT(cmd->controlValue, 0u);    // on
}

TEST_F(MappingTest, SustainOffEmitsCC64Zero) {
    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::LEFT_SUSTAIN, 0.0f));
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::CONTROL_CHANGE);
    EXPECT_EQ(cmd->controlNumber, 64u);
    EXPECT_EQ(cmd->controlValue, 0u);    // off
}

// ---------------------------------------------------------------------------
// Escala cromática completa (12 semitons por oitava)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Legato zone tracking (RIGHT_PINCH_POSITION)
// ---------------------------------------------------------------------------

TEST_F(MappingTest, SameZonePositionNoNewNote) {
    // Nota ativa em Y=0.5 → semitom 6 (zona 6/12)
    auto noteOn = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.5f));
    ASSERT_TRUE(noteOn.has_value());
    drain(midiQ);

    // Posição na mesma zona — não deve emitir nada
    gestureQ->push(makeGesture(GestureType::RIGHT_PINCH_POSITION, 0.5f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto cmd = midiQ->pop();
    EXPECT_FALSE(cmd.has_value()) << "Mesma zona não deveria gerar novo NOTE_ON";
}

TEST_F(MappingTest, NoActiveNoteIgnoresPosition) {
    // Sem nota ativa — RIGHT_PINCH_POSITION deve ser ignorado
    gestureQ->push(makeGesture(GestureType::RIGHT_PINCH_POSITION, 0.5f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto cmd = midiQ->pop();
    EXPECT_FALSE(cmd.has_value()) << "Sem nota ativa, posição deve ser ignorada";
}

TEST_F(MappingTest, ZoneChangeIgnoredWhenLegatoOff) {
    // legatoMode=false (default) — zone changes durante pinch hold devem ser ignorados
    auto noteOn = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.125f));
    ASSERT_TRUE(noteOn.has_value());
    drain(midiQ);

    // Posição claramente em outra zona: Y=0.875
    gestureQ->push(makeGesture(GestureType::RIGHT_PINCH_POSITION, 0.875f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto cmd = midiQ->pop();
    EXPECT_FALSE(cmd.has_value()) << "Com legato OFF, zone change nao deve gerar nota";
}

TEST_F(MappingTest, ZoneChangeWorksAfterRuntimeLegatoToggle) {
    // Começa com legato OFF (default)
    auto noteOn = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.125f));
    ASSERT_TRUE(noteOn.has_value());
    drain(midiQ);

    // Ligar legato em runtime
    module->setLegatoMode(true);

    // Agora zone change deve funcionar
    gestureQ->push(makeGesture(GestureType::RIGHT_PINCH_POSITION, 0.875f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<MidiCommand> cmds;
    while (auto c = midiQ->pop()) cmds.push_back(*c);
    ASSERT_GE(cmds.size(), 2u) << "Com legato ON, zone change deveria gerar NOTE_ON + NOTE_OFF";
}

TEST_F(MappingTest, HysteresisPreventsBoundaryToggling) {
    // Precisa de legato ON para que zone changes sejam processados
    module->setLegatoMode(true);

    // Nota ativa em Y=0.125 (centro da zona 1)
    auto noteOn = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.125f));
    ASSERT_TRUE(noteOn.has_value());
    drain(midiQ);

    // Posição exatamente na borda entre zona 1 e 2 (Y ≈ 2/12 ≈ 0.1667)
    // Zona 2 vai de 2/12 a 3/12 = [0.1667, 0.25], centro = 0.2083
    // Ponto na borda: Y=0.1667 → distFromMid = |0.1667 - 0.2083| = 0.0416
    // zoneWidth * 0.3 = (1/12)*0.3 = 0.025 → 0.0416 > 0.025 → histerese bloqueia
    gestureQ->push(makeGesture(GestureType::RIGHT_PINCH_POSITION, 0.1667f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto cmd = midiQ->pop();
    EXPECT_FALSE(cmd.has_value()) << "Posição na borda de zona deveria ser bloqueada por histerese";
}

// Fixture com legatoMode habilitado
class MappingLegatoTest : public ::testing::Test {
protected:
    std::shared_ptr<GestureQueue> gestureQ;
    std::shared_ptr<MidiQueue>    midiQ;
    std::unique_ptr<MappingModule> module;

    void SetUp() override {
        gestureQ = std::make_shared<GestureQueue>();
        midiQ    = std::make_shared<MidiQueue>();

        MappingModule::Config cfg;
        cfg.midiChannel   = 0;
        cfg.defaultOctave  = 4;
        cfg.legatoMode     = true;
        cfg.zonePadding    = 0.0f;

        module = std::make_unique<MappingModule>(gestureQ, midiQ, cfg);
        module->start();
    }

    void TearDown() override {
        module->stop();
    }
};

TEST_F(MappingLegatoTest, LegatoZoneChangeEmitsNoteOnBeforeNoteOff) {
    // Nota ativa em Y=0.125 (centro da zona 1)
    auto noteOn = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.125f));
    ASSERT_TRUE(noteOn.has_value());
    const uint8_t firstNote = noteOn->noteNumber;
    drain(midiQ);

    // Mudar para zona bem diferente: Y=0.875 (centro da zona 10)
    gestureQ->push(makeGesture(GestureType::RIGHT_PINCH_POSITION, 0.875f));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<MidiCommand> cmds;
    while (auto c = midiQ->pop()) cmds.push_back(*c);

    ASSERT_GE(cmds.size(), 2u) << "Legato zone change deveria gerar NOTE_ON + NOTE_OFF";

    // Legato: NOTE_ON new FIRST, then NOTE_OFF old
    size_t onIdx = SIZE_MAX, offIdx = SIZE_MAX;
    for (size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i].type == MidiCommandType::NOTE_ON  && onIdx == SIZE_MAX) onIdx = i;
        if (cmds[i].type == MidiCommandType::NOTE_OFF && offIdx == SIZE_MAX) offIdx = i;
    }
    ASSERT_NE(onIdx, SIZE_MAX) << "NOTE_ON esperado";
    ASSERT_NE(offIdx, SIZE_MAX) << "NOTE_OFF esperado";
    EXPECT_LT(onIdx, offIdx) << "Legato: NOTE_ON deve vir ANTES do NOTE_OFF";

    // NOTE_OFF deve ser da nota anterior
    EXPECT_EQ(cmds[offIdx].noteNumber, firstNote);
    // NOTE_ON deve ser da nova nota (diferente da anterior)
    EXPECT_NE(cmds[onIdx].noteNumber, firstNote);
}

// ---------------------------------------------------------------------------
// Padding nas extremidades (zonas ativas em [p, 1-p])
// ---------------------------------------------------------------------------

TEST(MappingZonePaddingTest, BottomDeadZoneClampsToLowestNote) {
    auto gQ = std::make_shared<GestureQueue>();
    auto mQ = std::make_shared<MidiQueue>();
    MappingModule::Config cfg;
    cfg.defaultOctave = 4;
    cfg.zonePadding   = 0.10f;
    MappingModule mod(gQ, mQ, cfg);
    mod.start();

    // Y = 0.05 está abaixo do padding inferior (0.10) → clampa para a nota mais grave (semitom 0)
    GestureEvent ev;
    ev.type         = GestureType::RIGHT_PINCH_START;
    ev.hand         = HandLabel::RIGHT;
    ev.primaryValue = 0.05f;
    ev.timestamp    = std::chrono::steady_clock::now();
    gQ->push(ev);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    auto cmd = mQ->pop();
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::NOTE_ON);
    EXPECT_EQ(cmd->noteNumber, 4 * 12 + 0);  // Dó da oitava 4 (nota 48)

    mod.stop();
}

TEST(MappingZonePaddingTest, TopDeadZoneClampsToHighestNote) {
    auto gQ = std::make_shared<GestureQueue>();
    auto mQ = std::make_shared<MidiQueue>();
    MappingModule::Config cfg;
    cfg.defaultOctave = 4;
    cfg.zonePadding   = 0.10f;
    MappingModule mod(gQ, mQ, cfg);
    mod.start();

    // Y = 0.97 está acima do padding superior (0.90) → clampa para o semitom mais agudo (11)
    GestureEvent ev;
    ev.type         = GestureType::RIGHT_PINCH_START;
    ev.hand         = HandLabel::RIGHT;
    ev.primaryValue = 0.97f;
    ev.timestamp    = std::chrono::steady_clock::now();
    gQ->push(ev);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    auto cmd = mQ->pop();
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->type, MidiCommandType::NOTE_ON);
    EXPECT_EQ(cmd->noteNumber, 4 * 12 + 11);  // B da oitava 4 (nota 59)

    mod.stop();
}

// ---------------------------------------------------------------------------
// Controles runtime
// ---------------------------------------------------------------------------

TEST_F(MappingTest, SetLegatoModeRuntime) {
    EXPECT_FALSE(module->isLegatoMode());
    module->setLegatoMode(true);
    EXPECT_TRUE(module->isLegatoMode());
    module->setLegatoMode(false);
    EXPECT_FALSE(module->isLegatoMode());
}

TEST_F(MappingTest, ProgramChangeEmitted) {
    module->requestProgramChange(42);
    // O mapping loop processa o pendingProgram_ na próxima iteração
    // Enviar qualquer gesto para forçar o loop a iterar
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Program Change deve aparecer na fila MIDI
    std::vector<MidiCommand> cmds;
    while (auto c = midiQ->pop()) cmds.push_back(*c);

    bool foundPC = false;
    for (const auto& c : cmds) {
        if (c.type == MidiCommandType::PROGRAM_CHANGE) {
            EXPECT_EQ(c.controlNumber, 42u);
            foundPC = true;
        }
    }
    EXPECT_TRUE(foundPC) << "PROGRAM_CHANGE não encontrado na fila MIDI";
    EXPECT_EQ(module->currentProgram(), 42);
}

TEST_F(MappingTest, PitchBendRangeClamped) {
    module->setPitchBendRange(6);
    EXPECT_EQ(module->pitchBendRange(), 6);

    module->setPitchBendRange(0);
    EXPECT_EQ(module->pitchBendRange(), 1);  // clamped to min

    module->setPitchBendRange(24);
    EXPECT_EQ(module->pitchBendRange(), 12);  // clamped to max
}

// ---------------------------------------------------------------------------
// Escala cromática completa (12 semitons por oitava)
// ---------------------------------------------------------------------------

TEST_F(MappingTest, ChromaticScaleCoversFullOctave) {
    // primaryValue=0.99 na oitava 4 → semitom 11 → nota = 48+11 = 59 (B4)
    auto cmd = pushAndWait(gestureQ, midiQ, makeGesture(GestureType::RIGHT_PINCH_START, 0.99f));
    ASSERT_TRUE(cmd.has_value());
    // Com 12 semitons: int(0.99 * 12) = 11 → nota = 4*12 + 11 = 59
    EXPECT_EQ(cmd->noteNumber, 59u);
}
