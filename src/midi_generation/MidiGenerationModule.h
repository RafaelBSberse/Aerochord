#pragma once

#include "common/LockFreeQueue.h"
#include "common/PipelineTypes.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Forward-declare ALSA types para não expor <alsa/asoundlib.h> no header
#ifdef __linux__
struct _snd_seq;
typedef struct _snd_seq snd_seq_t;
#endif

namespace aerochord {

// =============================================================================
// MidiGenerationModule — Módulo 5: Geração e Envio de Mensagens MIDI
//
// Responsabilidades:
//   - Consumir MidiCommands da fila de entrada
//   - Realizar handshake MIDI-CI para detectar suporte a MIDI 2.0 no receptor
//   - Empacotar comandos em Universal MIDI Packets (UMP) 32/64 bits
//   - Despachar pacotes UMP via ALSA Sequencer (Linux) em modo MIDI 2.0
//   - Fallback automático para mensagens MIDI 1.0 de 7 bits quando necessário
//   - Inserir timestamps nos pacotes UMP para medição de latência end-to-end
//
// Transporte UMP (Linux):
//   Usa ALSA Sequencer com SND_SEQ_CLIENT_UMP_MIDI_2_0 (kernel ≥ 6.5,
//   alsa-lib ≥ 1.2.10). Cria uma porta virtual "Aerochord UMP" que
//   sintetizadores podem subscrever via `aconnect` ou JACK.
//
// MIDI 2.0 vs MIDI 1.0:
//   - Se o receptor confirmar suporte MIDI 2.0 via MIDI-CI, ou se a porta
//     ALSA UMP estiver ativa:
//       Note On/Off: velocity 16 bits (UMP Message Type 4)
//       Control Change: valor 32 bits
//       Pitch Bend: resolução 32 bits
//   - Caso contrário (fallback):
//       Trunca velocity e control values para 7 bits
//       Envia mensagens MIDI 1.0 via juce::MidiMessage
// =============================================================================

using MidiQueue = LockFreeQueue<MidiCommand, 128>;

class MidiGenerationModule : private juce::MidiInputCallback {
public:
    // -------------------------------------------------------------------------
    // Configuração
    // -------------------------------------------------------------------------
    struct Config {
        std::string outputDeviceName;            // vazio = usar primeiro dispositivo disponível
        bool        preferMidi2     = true;      // tentar handshake MIDI-CI e abrir porta UMP
        int         midiCiTimeoutMs = 500;       // timeout para resposta MIDI-CI
        uint8_t     midiChannel     = 0;         // canal MIDI [0,15] para RPN e setup inicial
        bool        evalMode        = false;     // gravar CSV de latência por evento
        std::string evalOutputDir   = ".";       // diretório de saída do CSV
        Config() = default;
    };

    // -------------------------------------------------------------------------
    // Construção / destruição
    // -------------------------------------------------------------------------
    explicit MidiGenerationModule(std::shared_ptr<MidiQueue> inputQueue);
    explicit MidiGenerationModule(std::shared_ptr<MidiQueue> inputQueue, Config config);
    ~MidiGenerationModule();

    MidiGenerationModule(const MidiGenerationModule&)            = delete;
    MidiGenerationModule& operator=(const MidiGenerationModule&) = delete;

    // -------------------------------------------------------------------------
    // Ciclo de vida
    // -------------------------------------------------------------------------
    bool start();   // Abre dispositivo MIDI, executa handshake, inicia thread
    void stop();

    bool isRunning()  const;
    bool isMidi2Mode() const;  // true após handshake bem-sucedido ou porta UMP ativa

    // -------------------------------------------------------------------------
    // Diagnóstico
    // -------------------------------------------------------------------------
    uint64_t packetsSent()    const;
    uint64_t packetsDropped() const;

    // Latência end-to-end (VideoFrame.timestamp → MIDI enviado)
    float    latencyP50Ms()   const;  // percentil 50 (mediana)
    float    latencyP95Ms()   const;  // percentil 95
    uint64_t latencySamples() const;  // total de amostras acumuladas

    // Lista dispositivos MIDI disponíveis (utilitário estático)
    static juce::StringArray listOutputDevices();

private:
    void sendLoop();

    // Envio MIDI 2.0 (UMP) via ALSA Sequencer
    bool sendUmp(const MidiCommand& cmd);

    // Envio MIDI 1.0 (fallback via juce::MidiMessage)
    bool sendMidi1(const MidiCommand& cmd);

    // Inicializa porta ALSA Sequencer em modo UMP (Linux)
    bool initAlsaUmp();
    void closeAlsaUmp();

    // Realiza handshake MIDI-CI e retorna true se MIDI 2.0 negociado
    bool performMidiCiHandshake();

    // Envia RPN para configurar Pitch Bend Sensitivity no receptor
    void sendPitchBendRangeRpn(float semitones, uint8_t channel);

    // Envia MIDI-CI Property Exchange Inquiry (stub — loga resposta se houver)
    void attemptPropertyExchange();

    // juce::MidiInputCallback — recebe resposta MIDI-CI do receptor
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;

    std::shared_ptr<MidiQueue> inputQueue_;
    Config config_;

    std::thread       thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> midi2Mode_{ false };

    // Output e Input MIDI via JUCE (MIDI 1.0 fallback)
    std::unique_ptr<juce::MidiOutput> midiOutput_;
    std::unique_ptr<juce::MidiInput>  midiInput_;

    // ALSA Sequencer UMP (Linux — MIDI 2.0 nativo)
#ifdef __linux__
    snd_seq_t* alsaSeq_{ nullptr };
    int        alsaPort_{ -1 };
#endif

    // Flag setada pelo callback quando MIDI-CI Reply é recebido
    std::atomic<bool> ciReplyReceived_{ false };

    // MUID gerado aleatoriamente por sessão (M2-101-UM §5.1.1)
    uint32_t muid_{ 0 };

    // Dados extraídos do MIDI-CI Discovery Reply
    std::vector<uint8_t> ciReplyData_;

    std::atomic<uint64_t> packetsSent_{ 0 };
    std::atomic<uint64_t> packetsDropped_{ 0 };

    // --- Medição de latência ---
    static constexpr size_t kLatencyWindowSize     = 512;  // potência de 2
    static constexpr size_t kLatencyUpdateInterval = 32;   // recalcular percentis a cada N envios
    std::array<float, kLatencyWindowSize> latencyWindow_{};
    std::atomic<size_t>   latencyWriteIdx_{ 0 };
    std::atomic<uint64_t> latencySamples_{ 0 };
    std::atomic<float>    latencyP50_{ 0.0f };
    std::atomic<float>    latencyP95_{ 0.0f };

    // --- Modo eval: gravação CSV ---
    std::unique_ptr<std::ofstream> evalFile_;
};

} // namespace aerochord
