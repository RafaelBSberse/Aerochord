#include "MappingModule.h"
#include "common/Logger.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace aerochord {

static constexpr std::string_view kModule = "MappingModule";

// ---------------------------------------------------------------------------
// Construção / destruição
// ---------------------------------------------------------------------------
MappingModule::MappingModule(std::shared_ptr<GestureQueue> inputQueue,
                             std::shared_ptr<MidiQueue>    outputQueue)
    : MappingModule(std::move(inputQueue), std::move(outputQueue), Config{}) {}

MappingModule::MappingModule(std::shared_ptr<GestureQueue> inputQueue,
                             std::shared_ptr<MidiQueue>    outputQueue,
                             Config config)
    : inputQueue_(std::move(inputQueue))
    , outputQueue_(std::move(outputQueue))
    , config_(config)
    , octave_(config.defaultOctave)
    , profile_(buildDefaultProfile(config))
    , legatoMode_(config.legatoMode)
    , pitchBendRange_(static_cast<int>(config.pitchBendRange))
{
}

MappingModule::~MappingModule() {
    stop();
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------
bool MappingModule::start() {
    if (running_.load())
        return true;

    AEROCHORD_LOG_INFO(kModule, "Iniciando mapeamento gesto→MIDI...");
    running_.store(true);
    thread_ = std::thread(&MappingModule::mappingLoop, this);
    AEROCHORD_LOG_INFO(kModule, "Thread de mapeamento iniciada.");
    return true;
}

void MappingModule::stop() {
    if (!running_.exchange(false))
        return;

    if (thread_.joinable())
        thread_.join();

    AEROCHORD_LOG_INFO(kModule, "Mapeamento encerrado.");
}

bool MappingModule::isRunning() const { return running_.load(); }

// ---------------------------------------------------------------------------
// Gerenciamento de perfis
// ---------------------------------------------------------------------------
void MappingModule::loadProfile(MappingProfile profile) {
    std::lock_guard<std::mutex> lock(profileMutex_);
    profile_ = std::move(profile);
    AEROCHORD_LOG_INFO(kModule, std::string("Perfil carregado: ") + profile_.name);
}

std::string MappingModule::currentProfileName() const {
    std::lock_guard<std::mutex> lock(profileMutex_);
    return profile_.name;
}

// ---------------------------------------------------------------------------
// Diagnóstico
// ---------------------------------------------------------------------------
int      MappingModule::currentOctave()    const { return octave_; }
uint32_t MappingModule::currentVolume()    const { return volume_; }
int      MappingModule::activeNote()       const { return activeNote_; }
uint64_t MappingModule::commandsEmitted()  const { return commandsEmitted_.load(); }

// ---------------------------------------------------------------------------
// Controles em tempo real
// ---------------------------------------------------------------------------
void MappingModule::setLegatoMode(bool enabled)    { legatoMode_.store(enabled); }
bool MappingModule::isLegatoMode() const           { return legatoMode_.load(); }

void MappingModule::setPitchBendRange(int semitones) {
    pitchBendRange_.store(std::clamp(semitones, 1, 12));
}
int MappingModule::pitchBendRange() const          { return pitchBendRange_.load(); }

void MappingModule::requestProgramChange(int program) {
    pendingProgram_.store(std::clamp(program, 0, 127));
}
int MappingModule::currentProgram() const          { return currentProgram_.load(); }

// ---------------------------------------------------------------------------
// Conversão de zona vertical → nota MIDI
// ---------------------------------------------------------------------------
int MappingModule::verticalZoneToNote(float normalizedY) const {
    // Divide a zona vertical em 12 semitons (escala cromática completa por oitava)
    // normalizedY ∈ [0, 1]. As zonas ativas ocupam [padding, 1 - padding];
    // posições fora desse intervalo são fixadas (clamp) na zona extrema mais
    // próxima — assim o usuário não precisa alcançar literalmente as bordas
    // do quadro para tocar as notas mais grave/aguda.
    constexpr int kSemitones = 12;
    const float p     = std::clamp(config_.zonePadding, 0.0f, 0.45f);
    const float span  = 1.0f - 2.0f * p;
    const float relY  = (normalizedY - p) / span;
    int semitone = static_cast<int>(relY * kSemitones);
    semitone = std::clamp(semitone, 0, kSemitones - 1);
    const int noteBase = octave_ * 12;
    return std::clamp(noteBase + semitone, 0, 127);
}

// ---------------------------------------------------------------------------
// Perfil padrão (Tabela 2 do TCC — hipótese inicial)
// ---------------------------------------------------------------------------
MappingProfile MappingModule::buildDefaultProfile(const Config& cfg) {
    MappingProfile p;
    p.name = "default";

    // UMP Groups: mão direita = Group 1, mão esquerda = Group 0 (cap 3, §3.2.2)
    constexpr uint8_t kGroupRight = 1;
    constexpr uint8_t kGroupLeft  = 0;

    // RIGHT_PINCH_START → NOTE_ON
    p.translators[GestureType::RIGHT_PINCH_START] = [cfg](const GestureEvent& ev) {
        MidiCommand cmd;
        cmd.type       = MidiCommandType::NOTE_ON;
        cmd.channel    = cfg.midiChannel;
        cmd.group      = kGroupRight;
        // noteNumber e velocity serão preenchidos pelo MappingModule com base no estado
        cmd.velocity   = static_cast<uint32_t>(ev.velocity * 0xFFFFFFFF);
        cmd.timestamp  = ev.timestamp;
        return cmd;
    };

    // RIGHT_PINCH_RELEASE → NOTE_OFF
    p.translators[GestureType::RIGHT_PINCH_RELEASE] = [cfg](const GestureEvent& ev) {
        MidiCommand cmd;
        cmd.type      = MidiCommandType::NOTE_OFF;
        cmd.channel   = cfg.midiChannel;
        cmd.group     = kGroupRight;
        cmd.velocity  = 0;
        cmd.timestamp = ev.timestamp;
        return cmd;
    };

    // RIGHT_PITCH_BEND → PER_NOTE_PITCH_BEND (MIDI 2.0) ou PITCH_BEND (MIDI 1.0 fallback)
    // Quando há nota ativa, usa per-note pitch bend (UMP MT4 Opcode 0x6) para que
    // o bend afete apenas a nota sustentada — sem conflito com notas futuras.
    // O noteNumber é preenchido pelo mappingLoop() com base no estado corrente.
    p.translators[GestureType::RIGHT_PITCH_BEND] = [cfg](const GestureEvent& ev) {
        MidiCommand cmd;
        cmd.type      = MidiCommandType::PER_NOTE_PITCH_BEND;
        cmd.channel   = cfg.midiChannel;
        cmd.group     = kGroupRight;
        // Escalar pelo range configurado (clampado a ±1 relativo ao max de 12 semitons)
        const float scale = std::clamp(cfg.pitchBendRange / 12.0f, 0.0f, 1.0f);
        cmd.pitchBend = static_cast<int32_t>(ev.primaryValue * scale * 0x7FFFFFFF);
        cmd.timestamp = ev.timestamp;
        return cmd;
    };

    // LEFT_VOLUME_CONTROL → CONTROL_CHANGE CC#7 (volume)
    // volumeCurve: expoente da curva de resposta.
    //   < 1.0 = curva logarítmica (mais sensível em volumes baixos)
    //   = 1.0 = linear
    //   > 1.0 = curva exponencial (mais sensível em volumes altos)
    p.translators[GestureType::LEFT_VOLUME_CONTROL] = [cfg](const GestureEvent& ev) {
        MidiCommand cmd;
        cmd.type          = MidiCommandType::CONTROL_CHANGE;
        cmd.channel       = cfg.midiChannel;
        cmd.group         = kGroupLeft;
        cmd.controlNumber = 7;  // CC#7 = Channel Volume
        const float v     = std::clamp(ev.primaryValue, 0.0f, 1.0f);
        const float curved = std::pow(v, cfg.volumeCurve);
        cmd.controlValue  = static_cast<uint32_t>(curved * 0xFFFFFFFFu);
        cmd.timestamp     = ev.timestamp;
        return cmd;
    };

    // RIGHT_TIMBRE_CONTROL → CONTROL_CHANGE CC#74 (Brightness / Filter Cutoff — convenção GM)
    p.translators[GestureType::RIGHT_TIMBRE_CONTROL] = [cfg](const GestureEvent& ev) {
        MidiCommand cmd;
        cmd.type          = MidiCommandType::CONTROL_CHANGE;
        cmd.channel       = cfg.midiChannel;
        cmd.group         = kGroupRight;
        cmd.controlNumber = 74;  // CC#74 = Brightness
        cmd.controlValue  = static_cast<uint32_t>(ev.primaryValue * 0xFFFFFFFF);
        cmd.timestamp     = ev.timestamp;
        return cmd;
    };

    // RIGHT_VIBRATO → CONTROL_CHANGE CC#1 (Modulation Wheel — convenção universal)
    p.translators[GestureType::RIGHT_VIBRATO] = [cfg](const GestureEvent& ev) {
        MidiCommand cmd;
        cmd.type          = MidiCommandType::CONTROL_CHANGE;
        cmd.channel       = cfg.midiChannel;
        cmd.group         = kGroupRight;
        cmd.controlNumber = 1;   // CC#1 = Modulation
        cmd.controlValue  = static_cast<uint32_t>(ev.primaryValue * 0xFFFFFFFF);
        cmd.timestamp     = ev.timestamp;
        return cmd;
    };

    // RIGHT_PINCH_POSITION → NOTE_ON (somente se zona mudou; filtrado pelo mappingLoop)
    p.translators[GestureType::RIGHT_PINCH_POSITION] = [cfg](const GestureEvent& ev) {
        MidiCommand cmd;
        cmd.type       = MidiCommandType::NOTE_ON;
        cmd.channel    = cfg.midiChannel;
        cmd.group      = kGroupRight;
        cmd.velocity   = static_cast<uint32_t>(0.7f * 0xFFFFFFFF);  // legato: velocity moderada fixa
        cmd.timestamp  = ev.timestamp;
        return cmd;
    };

    // LEFT_SUSTAIN → CONTROL_CHANGE CC#64 (Sustain Pedal)
    // primaryValue: 1.0 = sustain on, 0.0 = sustain off
    p.translators[GestureType::LEFT_SUSTAIN] = [cfg](const GestureEvent& ev) {
        MidiCommand cmd;
        cmd.type          = MidiCommandType::CONTROL_CHANGE;
        cmd.channel       = cfg.midiChannel;
        cmd.group         = kGroupLeft;
        cmd.controlNumber = 64;  // CC#64 = Sustain Pedal
        cmd.controlValue  = (ev.primaryValue > 0.5f) ? 0xFFFFFFFFu : 0u;
        cmd.timestamp     = ev.timestamp;
        return cmd;
    };

    return p;
}

// ---------------------------------------------------------------------------
// Loop de mapeamento (thread dedicada)
// ---------------------------------------------------------------------------
void MappingModule::mappingLoop() {
    AEROCHORD_LOG_DEBUG(kModule, "mappingLoop() iniciado.");

    while (running_.load()) {
        // Processar program change pendente (solicitado pela UI)
        {
            const int prog = pendingProgram_.exchange(-1);
            if (prog >= 0) {
                MidiCommand pc;
                pc.type          = MidiCommandType::PROGRAM_CHANGE;
                pc.channel       = config_.midiChannel;
                pc.group         = 1;  // mão direita (grupo principal)
                pc.controlNumber = static_cast<uint32_t>(prog);
                pc.timestamp     = std::chrono::steady_clock::now();
                if (outputQueue_->push(pc)) {
                    currentProgram_.store(prog);
                    ++commandsEmitted_;
                    AEROCHORD_LOG_INFO(kModule, "Program Change: " + std::to_string(prog));
                }
            }
        }

        auto opt = inputQueue_->pop();
        if (!opt) {
            std::this_thread::yield();
            continue;
        }

        const GestureEvent& ev = *opt;

        // LEFT_OCTAVE_SELECT não tem translator — atualiza estado e não emite MIDI
        if (ev.type == GestureType::LEFT_OCTAVE_SELECT) {
            const int prev = octave_;
            octave_ = std::clamp(octave_ + static_cast<int>(ev.primaryValue), 0, 10);
            if (octave_ != prev)
                AEROCHORD_LOG_INFO(kModule,
                    "Oitava: " + std::to_string(prev) + " → " + std::to_string(octave_));
            continue;
        }

        // RIGHT_PINCH_POSITION — legato zone tracking.
        // Só processa quando legato está ativo; caso contrário, ignora completamente
        // (comportamento original: notas só mudam ao soltar e refazer a pinça).
        if (ev.type == GestureType::RIGHT_PINCH_POSITION) {
            if (!legatoMode_.load())
                continue;  // legato desligado — ignorar posição

            if (activeNote_ < 0)
                continue;  // sem nota ativa — ignorar

            const int candidateNote = verticalZoneToNote(ev.primaryValue);
            if (candidateNote == activeNote_)
                continue;  // mesma zona — sem transição

            // Histerese: exigir que a posição esteja no interior 60% da zona
            // para evitar toggling rápido nas bordas entre duas zonas.
            // Posições nas regiões "dead-zone" (clampadas) sempre passam.
            constexpr int   kSemitones = 12;
            const float p              = std::clamp(config_.zonePadding, 0.0f, 0.45f);
            const float span           = 1.0f - 2.0f * p;
            if (ev.primaryValue >= p && ev.primaryValue <= 1.0f - p) {
                const float zoneWidth    = span / kSemitones;
                const int   zoneIndex    = std::clamp(
                    static_cast<int>((ev.primaryValue - p) / span * kSemitones),
                    0, kSemitones - 1);
                const float zoneMidpoint = p + (zoneIndex + 0.5f) * zoneWidth;
                const float distFromMid  = std::abs(ev.primaryValue - zoneMidpoint);
                if (distFromMid > zoneWidth * 0.3f)
                    continue;  // muito perto da borda — aguardar movimento mais decisivo
            }

            // Zona mudou e posição é estável → cair no fluxo normal de NOTE_ON
        }

        MidiCommand cmd;
        {
            std::lock_guard<std::mutex> lock(profileMutex_);
            auto it = profile_.translators.find(ev.type);
            if (it == profile_.translators.end())
                continue;  // gesto sem mapeamento no perfil atual
            cmd = it->second(ev);
        }

        // Enriquecer comando com estado musical corrente
        if (cmd.type == MidiCommandType::NOTE_ON) {
            const int prevNote = activeNote_;
            activeNote_    = verticalZoneToNote(ev.primaryValue);
            cmd.noteNumber = static_cast<uint8_t>(activeNote_);

            if (prevNote >= 0) {
                if (legatoMode_.load()) {
                    // Legato: enviar NOTE_ON da nova nota ANTES do NOTE_OFF da anterior.
                    // A sobreposição gera a articulação legato no sintetizador (cap 3.3.2).
                    if (!outputQueue_->push(cmd)) {
                        AEROCHORD_LOG_DEBUG(kModule, "Fila MIDI cheia — NOTE_ON legato descartado.");
                    } else {
                        ++commandsEmitted_;
                    }

                    MidiCommand offCmd;
                    offCmd.type       = MidiCommandType::NOTE_OFF;
                    offCmd.channel    = cmd.channel;
                    offCmd.group      = cmd.group;
                    offCmd.noteNumber = static_cast<uint8_t>(prevNote);
                    offCmd.velocity   = 0;
                    offCmd.timestamp  = ev.timestamp;
                    if (!outputQueue_->push(offCmd)) {
                        AEROCHORD_LOG_DEBUG(kModule, "Fila MIDI cheia — NOTE_OFF legato descartado.");
                    } else {
                        ++commandsEmitted_;
                    }
                    continue;  // NOTE_ON já enviado acima
                } else {
                    // Staccato (default): NOTE_OFF antes do NOTE_ON (previne stuck note)
                    MidiCommand offCmd;
                    offCmd.type       = MidiCommandType::NOTE_OFF;
                    offCmd.channel    = cmd.channel;
                    offCmd.group      = cmd.group;
                    offCmd.noteNumber = static_cast<uint8_t>(prevNote);
                    offCmd.velocity   = 0;
                    offCmd.timestamp  = ev.timestamp;
                    if (!outputQueue_->push(offCmd)) {
                        AEROCHORD_LOG_DEBUG(kModule, "Fila MIDI cheia — NOTE_OFF preventivo descartado.");
                    } else {
                        ++commandsEmitted_;
                    }
                }
            }
        } else if (cmd.type == MidiCommandType::CONTROL_CHANGE && cmd.controlNumber == 7) {
            // Atualizar estado interno de volume (CC#7 = Channel Volume)
            volume_ = cmd.controlValue;
        } else if (cmd.type == MidiCommandType::NOTE_OFF) {
            cmd.noteNumber  = (activeNote_ >= 0) ? static_cast<uint8_t>(activeNote_) : 60;
            activeNote_     = -1;
        } else if (cmd.type == MidiCommandType::PER_NOTE_PITCH_BEND) {
            // Per-note pitch bend: preencher noteNumber da nota ativa.
            // Se não há nota ativa, cai para channel pitch bend (MIDI 1.0 compatível).
            if (activeNote_ >= 0) {
                cmd.noteNumber = static_cast<uint8_t>(activeNote_);
            } else {
                cmd.type = MidiCommandType::PITCH_BEND;  // fallback channel-wide
            }
        }

        if (!outputQueue_->push(cmd)) {
            AEROCHORD_LOG_DEBUG(kModule, "Fila MIDI cheia — comando descartado.");
        } else {
            ++commandsEmitted_;
        }
    }

    AEROCHORD_LOG_DEBUG(kModule, "mappingLoop() encerrado.");
}

} // namespace aerochord
