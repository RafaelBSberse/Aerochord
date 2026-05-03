#pragma once

#include "common/LockFreeQueue.h"
#include "common/PipelineTypes.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace aerochord {

// =============================================================================
// MappingModule — Módulo 4: Mapeamento Gesto → Comando MIDI
//
// Responsabilidades:
//   - Consumir GestureEvents da fila de entrada
//   - Manter estado musical corrente (oitava ativa, volume, nota sustentada)
//   - Traduzir gestos em MidiCommands via perfil de mapeamento configurável
//   - Publicar MidiCommands na fila de saída (lock-free, SPSC)
//
// Perfis de mapeamento:
//   Um perfil é um map de GestureType → função de tradução.
//   Perfis podem ser trocados em tempo de execução (loadProfile) para
//   experimentos de usabilidade sem reiniciar o pipeline.
//
// Estado musical mantido internamente:
//   - octave         : oitava corrente [0, 10]
//   - globalVolume   : volume global MIDI 2.0 [0, 0xFFFFFFFF]
//   - activeNoteNumber: nota sustentada corrente (ou -1 se nenhuma)
//   - midiChannel    : canal MIDI de saída [0, 15]
// =============================================================================

using GestureQueue = LockFreeQueue<GestureEvent,  64>;
using MidiQueue    = LockFreeQueue<MidiCommand,   128>;

// Função de tradução: recebe evento e estado → preenche e retorna MidiCommand
using TranslatorFn = std::function<MidiCommand(const GestureEvent&)>;

struct MappingProfile {
    std::string name;
    std::unordered_map<GestureType, TranslatorFn> translators;
};

class MappingModule {
public:
    // -------------------------------------------------------------------------
    // Configuração
    // -------------------------------------------------------------------------
    struct Config {
        uint8_t midiChannel     = 0;   // canal MIDI [0, 15]
        int     defaultOctave   = 4;   // oitava inicial (Dó central = oitava 4, nota 60)
        float   volumeCurve     = 1.0f; // expoente de curva de volume (1.0 = linear)
        float   pitchBendRange  = 2.0f; // semitons de range do pitch bend
        bool    legatoMode      = false; // legato: NOTE_ON antes do NOTE_OFF (sobreposição)
        float   zonePadding     = 0.10f; // gap nas extremidades: zonas ativas ocupam [p, 1-p]
        Config() = default;
    };

    // -------------------------------------------------------------------------
    // Construção / destruição
    // -------------------------------------------------------------------------
    explicit MappingModule(std::shared_ptr<GestureQueue> inputQueue,
                           std::shared_ptr<MidiQueue>    outputQueue);
    explicit MappingModule(std::shared_ptr<GestureQueue> inputQueue,
                           std::shared_ptr<MidiQueue>    outputQueue,
                           Config config);
    ~MappingModule();

    MappingModule(const MappingModule&)            = delete;
    MappingModule& operator=(const MappingModule&) = delete;

    // -------------------------------------------------------------------------
    // Ciclo de vida
    // -------------------------------------------------------------------------
    bool start();
    void stop();
    bool isRunning() const;

    // -------------------------------------------------------------------------
    // Gerenciamento de perfis (thread-safe via mutex)
    // -------------------------------------------------------------------------
    void loadProfile(MappingProfile profile);
    std::string currentProfileName() const;

    // -------------------------------------------------------------------------
    // Diagnóstico / estado musical
    // -------------------------------------------------------------------------
    int      currentOctave()   const;
    uint32_t currentVolume()   const;
    int      activeNote()      const;  // -1 se nenhuma nota ativa
    uint64_t commandsEmitted() const;

    // -------------------------------------------------------------------------
    // Controles em tempo real (thread-safe, chamáveis a partir de qualquer thread)
    // -------------------------------------------------------------------------
    void setLegatoMode(bool enabled);
    bool isLegatoMode() const;

    void setPitchBendRange(int semitones);   // [1, 12]
    int  pitchBendRange() const;

    void requestProgramChange(int program);  // [0, 127]; -1 = nenhum pendente
    int  currentProgram() const;

    float zonePadding() const { return config_.zonePadding; }

private:
    void mappingLoop();

    // Tradutores padrão para o perfil inicial (Tabela 2 do TCC)
    static MappingProfile buildDefaultProfile(const Config& cfg);

    // Conversão de posição vertical normalizada para número de nota MIDI
    int verticalZoneToNote(float normalizedY) const;

    std::shared_ptr<GestureQueue> inputQueue_;
    std::shared_ptr<MidiQueue>    outputQueue_;
    Config config_;

    std::thread       thread_;
    std::atomic<bool> running_{ false };

    // Estado musical (acessado apenas pela thread de mapeamento)
    int      octave_{ 4 };
    uint32_t volume_{ 0x7FFFFFFF };  // ~50% em 32 bits
    int      activeNote_{ -1 };

    MappingProfile profile_;
    mutable std::mutex profileMutex_;  // protege loadProfile() / leitura em mappingLoop()

    std::atomic<uint64_t> commandsEmitted_{ 0 };

    // Controles runtime (lidos pela mapping thread, escritos pela viz/main thread)
    std::atomic<bool> legatoMode_{ false };
    std::atomic<int>  pitchBendRange_{ 2 };     // semitons [1, 12]
    std::atomic<int>  pendingProgram_{ -1 };     // -1 = nenhum pendente
    std::atomic<int>  currentProgram_{ 0 };      // programa GM atual
};

} // namespace aerochord
