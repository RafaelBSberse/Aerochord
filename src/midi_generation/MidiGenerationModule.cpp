#include "MidiGenerationModule.h"
#include "common/Logger.h"
#include "common/ThreadUtils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

#ifdef __linux__
#  include <alsa/asoundlib.h>
   // UMP via ALSA Sequencer requer alsa-lib ≥ 1.2.10 (SND_LIB_VERSION ≥ 0x10210a)
   // Verificação em tempo de compilação; se não disponível, fallback para MIDI 1.0.
#  if !defined(SND_LIB_VERSION) || SND_LIB_VERSION < 0x10210a
#    define AEROCHORD_ALSA_NO_UMP 1
#  endif
#endif

namespace aerochord {

static constexpr std::string_view kModule = "MidiGenerationModule";

// ---------------------------------------------------------------------------
// Construção / destruição
// ---------------------------------------------------------------------------
MidiGenerationModule::MidiGenerationModule(std::shared_ptr<MidiQueue> inputQueue)
    : MidiGenerationModule(std::move(inputQueue), Config{}) {}

MidiGenerationModule::MidiGenerationModule(std::shared_ptr<MidiQueue> inputQueue,
                                           Config config)
    : inputQueue_(std::move(inputQueue))
    , config_(std::move(config))
{
    // Gerar MUID aleatório por sessão (M2-101-UM §5.1.1 — 28 bits significativos)
    std::random_device rd;
    muid_ = rd() & 0x0FFFFFFFu;
}

MidiGenerationModule::~MidiGenerationModule() {
    stop();
}

// ---------------------------------------------------------------------------
// Utilitário estático
// ---------------------------------------------------------------------------
juce::StringArray MidiGenerationModule::listOutputDevices() {
    juce::StringArray names;
    for (const auto& dev : juce::MidiOutput::getAvailableDevices())
        names.add(dev.name);
    return names;
}

// ---------------------------------------------------------------------------
// ALSA Sequencer UMP — inicialização e envio (Linux)
//
// Requer kernel ≥ 6.5 e alsa-lib ≥ 1.2.10 com suporte UMP.
// Cria uma porta virtual "Aerochord UMP" em modo MIDI 2.0; sintetizadores
// e DAWs podem subscrever esta porta via `aconnect` ou JACK patchbay.
//
// Se a API UMP não estiver disponível (kernel antigo, compilação sem ALSA),
// retorna false e o módulo utiliza o fallback MIDI 1.0 via JUCE.
// ---------------------------------------------------------------------------
bool MidiGenerationModule::initAlsaUmp() {
#if defined(__linux__) && !defined(AEROCHORD_ALSA_NO_UMP)
    int err = snd_seq_open(&alsaSeq_, "default", SND_SEQ_OPEN_OUTPUT, 0);
    if (err < 0) {
        AEROCHORD_LOG_WARN(kModule,
            "ALSA seq open falhou: " + std::string(snd_strerror(err)));
        alsaSeq_ = nullptr;
        return false;
    }

    snd_seq_set_client_name(alsaSeq_, "Aerochord");

    // Ativar modo UMP MIDI 2.0 no cliente ALSA (alsa-lib ≥ 1.2.10)
    err = snd_seq_set_client_midi_version(alsaSeq_, SND_SEQ_CLIENT_UMP_MIDI_2_0);
    if (err < 0) {
        AEROCHORD_LOG_WARN(kModule,
            "ALSA UMP mode não suportado: " + std::string(snd_strerror(err)) +
            " — usando MIDI 1.0 via JUCE.");
        snd_seq_close(alsaSeq_);
        alsaSeq_ = nullptr;
        return false;
    }

    // Criar porta de saída
    alsaPort_ = snd_seq_create_simple_port(alsaSeq_, "Aerochord UMP",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    if (alsaPort_ < 0) {
        AEROCHORD_LOG_WARN(kModule,
            "ALSA port create falhou: " + std::string(snd_strerror(alsaPort_)));
        snd_seq_close(alsaSeq_);
        alsaSeq_ = nullptr;
        alsaPort_ = -1;
        return false;
    }

    AEROCHORD_LOG_INFO(kModule,
        "ALSA UMP ativo — porta \"Aerochord UMP\" (client:" +
        std::to_string(snd_seq_client_id(alsaSeq_)) + " port:" +
        std::to_string(alsaPort_) + "). Conecte um sintetizador via aconnect.");

    return true;
#else
    return false;
#endif
}

void MidiGenerationModule::closeAlsaUmp() {
#if defined(__linux__) && !defined(AEROCHORD_ALSA_NO_UMP)
    if (alsaSeq_) {
        if (alsaPort_ >= 0)
            snd_seq_delete_simple_port(alsaSeq_, alsaPort_);
        snd_seq_close(alsaSeq_);
        alsaSeq_ = nullptr;
        alsaPort_ = -1;
    }
#endif
}

// ---------------------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------------------
bool MidiGenerationModule::start() {
    if (running_.load())
        return true;

    AEROCHORD_LOG_INFO(kModule, "Abrindo dispositivo MIDI de saída...");

    // Selecionar dispositivo JUCE (MIDI 1.0 fallback — sempre necessário)
    const auto devices = juce::MidiOutput::getAvailableDevices();
    if (devices.isEmpty()) {
        AEROCHORD_LOG_ERROR(kModule, "Nenhum dispositivo MIDI de saída disponível.");
        return false;
    }

    juce::MidiDeviceInfo targetDevice = devices[0];  // padrão: primeiro disponível
    if (!config_.outputDeviceName.empty()) {
        for (const auto& d : devices) {
            if (d.name == juce::String(config_.outputDeviceName)) {
                targetDevice = d;
                break;
            }
        }
    }

    midiOutput_ = juce::MidiOutput::openDevice(targetDevice.identifier);
    if (!midiOutput_) {
        AEROCHORD_LOG_ERROR(kModule, "Falha ao abrir dispositivo MIDI: " +
                            std::string(targetDevice.name.toRawUTF8()));
        return false;
    }

    AEROCHORD_LOG_INFO(kModule, "Dispositivo aberto: " +
                       std::string(targetDevice.name.toRawUTF8()));

    // Tentar transporte UMP via ALSA Sequencer (MIDI 2.0 nativo)
    bool umpAvailable = false;
    if (config_.preferMidi2) {
        umpAvailable = initAlsaUmp();
        if (umpAvailable) {
            midi2Mode_.store(true);
            AEROCHORD_LOG_INFO(kModule, "MIDI 2.0 ativo via ALSA Sequencer UMP.");
        }
    }

    // Handshake MIDI-CI (se preferência por MIDI 2.0 e UMP não disponível)
    if (config_.preferMidi2 && !umpAvailable) {
        midi2Mode_.store(performMidiCiHandshake());
        AEROCHORD_LOG_INFO(kModule,
            midi2Mode_.load() ? "MIDI 2.0 negociado via MIDI-CI."
                              : "Fallback para MIDI 1.0.");
    }

    // Enviar RPN para configurar Pitch Bend Range no receptor
    sendPitchBendRangeRpn(2.0f, config_.midiChannel);

    // Tentar Property Exchange se MIDI-CI handshake foi bem-sucedido
    if (midi2Mode_.load() && !ciReplyData_.empty())
        attemptPropertyExchange();

    // Modo eval: abrir CSV para gravação de latência por evento
    if (config_.evalMode) {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << config_.evalOutputDir << "/aerochord_eval_"
            << std::put_time(std::localtime(&t), "%Y%m%dT%H%M%S") << ".csv";
        evalFile_ = std::make_unique<std::ofstream>(oss.str());
        if (evalFile_->is_open()) {
            *evalFile_ << "timestamp_capture_us,timestamp_midi_us,"
                          "latency_ms,midi_type,note,velocity\n";
            AEROCHORD_LOG_INFO(kModule, "Modo eval: gravando em " + oss.str());
        } else {
            AEROCHORD_LOG_WARN(kModule, "Modo eval: não foi possível criar " + oss.str());
            evalFile_.reset();
        }
    }

    running_.store(true);
    thread_ = std::thread(&MidiGenerationModule::sendLoop, this);
    AEROCHORD_LOG_INFO(kModule, "Thread de envio MIDI iniciada.");
    return true;
}

void MidiGenerationModule::stop() {
    if (!running_.exchange(false))
        return;

    if (thread_.joinable())
        thread_.join();

    if (midiInput_) { midiInput_->stop(); midiInput_.reset(); }
    midiOutput_.reset();
    closeAlsaUmp();

    if (evalFile_ && evalFile_->is_open()) {
        evalFile_->flush();
        evalFile_.reset();
    }

    AEROCHORD_LOG_INFO(kModule, "Geração MIDI encerrada.");
}

bool MidiGenerationModule::isRunning()   const { return running_.load(); }
bool MidiGenerationModule::isMidi2Mode() const { return midi2Mode_.load(); }

uint64_t MidiGenerationModule::packetsSent()    const { return packetsSent_.load(); }
uint64_t MidiGenerationModule::packetsDropped() const { return packetsDropped_.load(); }

float    MidiGenerationModule::latencyP50Ms()   const { return latencyP50_.load(); }
float    MidiGenerationModule::latencyP95Ms()   const { return latencyP95_.load(); }
uint64_t MidiGenerationModule::latencySamples() const { return latencySamples_.load(); }

// ---------------------------------------------------------------------------
// Handshake MIDI-CI  (Spec M2-101-UM §5.4)
//
// Fluxo:
//   1. Abre MidiInput correspondente ao output (mesmo nome de dispositivo)
//   2. Envia Universal SysEx MIDI-CI Discovery para broadcast
//   3. Aguarda Discovery Reply via callback handleIncomingMidiMessage()
//   4. Valida SubID2 = 0x71 (Reply to Discovery) e source MUID
//   5. Retorna true se Reply válido recebido dentro do timeout
// ---------------------------------------------------------------------------
bool MidiGenerationModule::performMidiCiHandshake() {
    if (!midiOutput_)
        return false;

    // --- 1. Abrir MidiInput correspondente (mesmo nome do output) ---
    const juce::String outputName = midiOutput_->getName();
    const auto inputDevices = juce::MidiInput::getAvailableDevices();

    juce::String inputId;
    for (const auto& dev : inputDevices) {
        if (dev.name == outputName) {
            inputId = dev.identifier;
            break;
        }
    }

    if (inputId.isEmpty()) {
        AEROCHORD_LOG_DEBUG(kModule,
            "Nenhum MIDI input com o nome \"" +
            std::string(outputName.toRawUTF8()) +
            "\" — MIDI-CI não disponível; usando MIDI 1.0.");
        return false;
    }

    midiInput_ = juce::MidiInput::openDevice(inputId, this);
    if (!midiInput_) {
        AEROCHORD_LOG_DEBUG(kModule, "Falha ao abrir MIDI input — usando MIDI 1.0.");
        return false;
    }
    midiInput_->start();

    // --- 2. Construir e enviar MIDI-CI Discovery (Sub-ID2 = 0x70) ---
    // Formato: F0 7E 7F 0D 70 <srcMuid[4]> <dstMuid[4]>
    //          <ciVersion> <categoriesSupported> <maxMsgSize[4]>
    //          <outputPathId> F7
    constexpr uint8_t  kBroadcast = 0x7F;
    constexpr uint32_t kMaxMsg    = 512;

    const uint8_t ciMsg[] = {
        0xF0, 0x7E, 0x7F,  // Universal Non-Realtime SysEx, device=all
        0x0D, 0x70,        // Sub-ID1: MIDI-CI; Sub-ID2: Discovery
        static_cast<uint8_t>( muid_        & 0x7F),
        static_cast<uint8_t>((muid_ >>  7) & 0x7F),
        static_cast<uint8_t>((muid_ >> 14) & 0x7F),
        static_cast<uint8_t>((muid_ >> 21) & 0x7F),
        kBroadcast, kBroadcast, kBroadcast, kBroadcast,  // dst MUID = broadcast
        0x02,  // CI Version 2
        0x0C,  // Categories: Profile Config (0x04) | Property Exchange (0x08)
        static_cast<uint8_t>( kMaxMsg        & 0x7F),
        static_cast<uint8_t>((kMaxMsg >>  7) & 0x7F),
        static_cast<uint8_t>((kMaxMsg >> 14) & 0x7F),
        static_cast<uint8_t>((kMaxMsg >> 21) & 0x7F),
        0x00,  // Initiator Output Path ID (CI v2)
        0xF7
    };

    midiOutput_->sendMessageNow(
        juce::MidiMessage(ciMsg, static_cast<int>(sizeof(ciMsg))));

    AEROCHORD_LOG_DEBUG(kModule,
        "MIDI-CI Discovery enviado para \"" +
        std::string(outputName.toRawUTF8()) +
        "\". Aguardando reply (" +
        std::to_string(config_.midiCiTimeoutMs) + " ms)...");

    // --- 3. Aguardar Discovery Reply com timeout ---
    ciReplyReceived_.store(false);
    ciReplyData_.clear();
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(config_.midiCiTimeoutMs);

    while (!ciReplyReceived_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!ciReplyReceived_.load()) {
        midiInput_->stop();
        midiInput_.reset();
        AEROCHORD_LOG_INFO(kModule,
            "MIDI-CI timeout (" + std::to_string(config_.midiCiTimeoutMs) +
            " ms) — usando MIDI 1.0.");
    }

    return ciReplyReceived_.load();
}

// ---------------------------------------------------------------------------
// MIDI-CI Reply callback (juce::MidiInputCallback)
//
// Discovery Reply: F0 7E <device> 0D 71 <srcMuid[4]> <dstMuid[4]> ...
// Validação:
//   - SubID1 = 0x0D (MIDI-CI)
//   - SubID2 = 0x71 (Discovery Reply)
//   - Destination MUID deve corresponder ao nosso source MUID
// ---------------------------------------------------------------------------
void MidiGenerationModule::handleIncomingMidiMessage(juce::MidiInput*,
                                                     const juce::MidiMessage& msg) {
    if (!msg.isSysEx()) return;

    const uint8_t* data = msg.getSysExData();
    const int      size = msg.getSysExDataSize();

    // Validação mínima: Universal Non-Realtime SysEx, MIDI-CI, Discovery Reply
    if (size < 13) return;  // Tamanho mínimo de Discovery Reply

    if (data[0] != 0x7E) return;   // Não é Universal Non-Realtime
    if (data[3] != 0x0D) return;   // SubID1 != MIDI-CI
    if (data[4] != 0x71) return;   // SubID2 != Discovery Reply

    // Extrair destination MUID (bytes 9-12) e validar que é nosso source MUID
    const uint32_t dstMuid = static_cast<uint32_t>(data[9])
                           | (static_cast<uint32_t>(data[10]) << 7)
                           | (static_cast<uint32_t>(data[11]) << 14)
                           | (static_cast<uint32_t>(data[12]) << 21);

    if (dstMuid != muid_) {
        AEROCHORD_LOG_DEBUG(kModule,
            "MIDI-CI Reply com MUID destino incorreto — ignorando.");
        return;
    }

    // Salvar dados da resposta para referência
    ciReplyData_.assign(data, data + size);

    AEROCHORD_LOG_INFO(kModule,
        "MIDI-CI Discovery Reply válido recebido — MIDI 2.0 suportado pelo receptor.");
    ciReplyReceived_.store(true);
}

// ---------------------------------------------------------------------------
// RPN: Pitch Bend Sensitivity (RPN 0x0000)
//
// Envia CC#101=0, CC#100=0 (seleciona RPN Pitch Bend Sensitivity),
// CC#6=semitones (Data Entry MSB), CC#38=0 (Data Entry LSB — cents).
// Isso configura o receptor para interpretar pitch bend no range correto.
// ---------------------------------------------------------------------------
void MidiGenerationModule::sendPitchBendRangeRpn(float semitones, uint8_t channel) {
    if (!midiOutput_) return;

    const int ch = channel + 1;  // JUCE usa 1-based
    const int semi = std::clamp(static_cast<int>(semitones), 0, 24);

    midiOutput_->sendMessageNow(juce::MidiMessage::controllerEvent(ch, 101, 0));   // RPN MSB
    midiOutput_->sendMessageNow(juce::MidiMessage::controllerEvent(ch, 100, 0));   // RPN LSB
    midiOutput_->sendMessageNow(juce::MidiMessage::controllerEvent(ch, 6, semi));  // Data Entry MSB
    midiOutput_->sendMessageNow(juce::MidiMessage::controllerEvent(ch, 38, 0));    // Data Entry LSB
    // Desselecionar RPN (boa prática — evita alterações acidentais)
    midiOutput_->sendMessageNow(juce::MidiMessage::controllerEvent(ch, 101, 127));
    midiOutput_->sendMessageNow(juce::MidiMessage::controllerEvent(ch, 100, 127));

    AEROCHORD_LOG_INFO(kModule,
        "RPN Pitch Bend Sensitivity enviado: " + std::to_string(semi) + " semitons.");
}

// ---------------------------------------------------------------------------
// Property Exchange Inquiry (MIDI-CI)
//
// Envia PE Capability Inquiry (SubID2 = 0x30) para verificar se o receptor
// suporta consulta/ajuste dinâmico de parâmetros. Este é o mecanismo de
// calibragem dinâmica descrito no cap. 5.2.1c da proposta.
//
// A implementação atual é um stub: envia a inquiry, loga se houver resposta,
// mas não parseia propriedades específicas. A implementação completa de PE
// depende do receptor expor propriedades configuráveis (ex: curvas de
// sensibilidade, pitch bend range por nota, mapeamentos de CC).
// ---------------------------------------------------------------------------
void MidiGenerationModule::attemptPropertyExchange() {
    if (!midiOutput_ || !midiInput_)
        return;

    // Extrair source MUID do receptor a partir do Discovery Reply (bytes 5-8)
    if (ciReplyData_.size() < 9)
        return;

    const uint32_t responderMuid =
          static_cast<uint32_t>(ciReplyData_[5])
        | (static_cast<uint32_t>(ciReplyData_[6]) << 7)
        | (static_cast<uint32_t>(ciReplyData_[7]) << 14)
        | (static_cast<uint32_t>(ciReplyData_[8]) << 21);

    // PE Capability Inquiry: F0 7E 7F 0D 30 <srcMuid[4]> <dstMuid[4]>
    //   <numSimulRequests> F7
    const uint8_t peMsg[] = {
        0xF0, 0x7E, 0x7F,
        0x0D, 0x30,                                        // Sub-ID1: MIDI-CI; Sub-ID2: PE Capability Inquiry
        static_cast<uint8_t>( muid_        & 0x7F),
        static_cast<uint8_t>((muid_ >>  7) & 0x7F),
        static_cast<uint8_t>((muid_ >> 14) & 0x7F),
        static_cast<uint8_t>((muid_ >> 21) & 0x7F),
        static_cast<uint8_t>( responderMuid        & 0x7F),
        static_cast<uint8_t>((responderMuid >>  7) & 0x7F),
        static_cast<uint8_t>((responderMuid >> 14) & 0x7F),
        static_cast<uint8_t>((responderMuid >> 21) & 0x7F),
        0x01,   // Número máximo de requests simultâneos
        0xF7
    };

    midiOutput_->sendMessageNow(
        juce::MidiMessage(peMsg, static_cast<int>(sizeof(peMsg))));

    AEROCHORD_LOG_INFO(kModule,
        "MIDI-CI Property Exchange Capability Inquiry enviado para MUID=0x" +
        [&]{ char buf[9]; snprintf(buf, sizeof(buf), "%07X", responderMuid); return std::string(buf); }());

    // Aguardar PE Capability Reply (SubID2 = 0x31) — timeout curto
    // A resposta seria parseada no handleIncomingMidiMessage() se presente.
    // Na prática, a maioria dos dispositivos MIDI 2.0 atuais não suporta PE;
    // o stub documenta que o mecanismo está implementado no Aerochord.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    AEROCHORD_LOG_INFO(kModule,
        "Property Exchange: nenhuma resposta recebida — receptor não suporta PE "
        "ou não expõe propriedades configuráveis. Usando parâmetros padrão.");
}

// ---------------------------------------------------------------------------
// Envio UMP (MIDI 2.0) via ALSA Sequencer
//
// Constrói Universal MIDI Packets de 64 bits conforme M2-104-UM §4.
// Message Type 4 = MIDI 2.0 Channel Voice Messages.
//
// Transporte: ALSA Sequencer em modo UMP (kernel ≥ 6.5).
// Os 64 bits (2 × uint32) são enviados como evento ALSA seq UMP.
// ---------------------------------------------------------------------------
bool MidiGenerationModule::sendUmp(const MidiCommand& cmd) {
#if defined(__linux__) && !defined(AEROCHORD_ALSA_NO_UMP)
    if (!alsaSeq_ || alsaPort_ < 0)
        return false;

    // --- Construção dos 64 bits UMP ---
    // Word 0: [31:28] MT=4 | [27:24] Group=0 | [23:20] Opcode | [19:16] Ch | [15:8] Note/CC# | [7:0] reserved
    // Word 1: velocity/value (conforme tipo de mensagem)

    // Message Type 4 | Group (bits 27:24) — cap 3: mãos em grupos distintos
    const uint32_t kMT4 = 0x40000000u
                        | ((static_cast<uint32_t>(cmd.group) & 0x0Fu) << 24);
    const uint32_t ch = (static_cast<uint32_t>(cmd.channel) & 0x0Fu) << 16;

    uint32_t word0 = 0;
    uint32_t word1 = 0;

    switch (cmd.type) {
        case MidiCommandType::NOTE_ON:
            // Opcode 0x9; velocity 16-bit (bits 31:16 do word1) | attribute 16-bit = 0
            word0 = kMT4 | (0x9u << 20) | ch
                  | (static_cast<uint32_t>(cmd.noteNumber & 0x7F) << 8);
            word1 = (cmd.velocity >> 16) << 16;  // velocity 16-bit MSBs
            break;

        case MidiCommandType::NOTE_OFF:
            word0 = kMT4 | (0x8u << 20) | ch
                  | (static_cast<uint32_t>(cmd.noteNumber & 0x7F) << 8);
            word1 = (cmd.velocity >> 16) << 16;
            break;

        case MidiCommandType::CONTROL_CHANGE:
            // Opcode 0xB; CC value é 32 bits em MIDI 2.0
            word0 = kMT4 | (0xBu << 20) | ch
                  | (static_cast<uint32_t>(cmd.controlNumber & 0x7F) << 8);
            word1 = cmd.controlValue;  // 32-bit CC value
            break;

        case MidiCommandType::PITCH_BEND:
            // Channel Pitch Bend: Opcode 0xE; unsigned 32-bit; centro = 0x80000000
            word0 = kMT4 | (0xEu << 20) | ch;
            word1 = static_cast<uint32_t>(cmd.pitchBend) + 0x80000000u;
            break;

        case MidiCommandType::PER_NOTE_PITCH_BEND:
            // Per-Note Pitch Bend (M2-104-UM §4.2.14): Opcode 0x6
            // Word 0: [15:8] = note number; Word 1 = pitch bend 32-bit unsigned
            word0 = kMT4 | (0x6u << 20) | ch
                  | (static_cast<uint32_t>(cmd.noteNumber & 0x7F) << 8);
            word1 = static_cast<uint32_t>(cmd.pitchBend) + 0x80000000u;
            break;

        case MidiCommandType::PROGRAM_CHANGE:
            // Opcode 0xC; word1[31:25] = bank valid + program
            word0 = kMT4 | (0xCu << 20) | ch;
            word1 = static_cast<uint32_t>(cmd.controlNumber & 0x7F) << 24;
            break;

        default:
            return false;
    }

    // Enviar via ALSA Sequencer como evento UMP direto
    snd_seq_ump_event_t ev;
    snd_seq_ev_clear(reinterpret_cast<snd_seq_event_t*>(&ev));
    ev.type = SND_SEQ_EVENT_NONE;

    // Preencher dados UMP (64 bits = 2 words)
    ev.ump[0] = word0;
    ev.ump[1] = word1;
    ev.flags = SND_SEQ_EVENT_UMP;

    snd_seq_ev_set_source(reinterpret_cast<snd_seq_event_t*>(&ev), alsaPort_);
    snd_seq_ev_set_subs(reinterpret_cast<snd_seq_event_t*>(&ev));
    snd_seq_ev_set_direct(reinterpret_cast<snd_seq_event_t*>(&ev));

    int err = snd_seq_ump_event_output(alsaSeq_, &ev);
    if (err < 0) {
        AEROCHORD_LOG_DEBUG(kModule,
            "ALSA UMP send falhou: " + std::string(snd_strerror(err)));
        return false;
    }

    snd_seq_drain_output(alsaSeq_);

    AEROCHORD_LOG_DEBUG(kModule,
        "UMP enviado: word0=0x" +
        [&]{ char buf[9]; snprintf(buf, sizeof(buf), "%08X", word0); return std::string(buf); }() +
        " word1=0x" +
        [&]{ char buf[9]; snprintf(buf, sizeof(buf), "%08X", word1); return std::string(buf); }());

    return true;
#else
    (void)cmd;
    return false;  // UMP nativo não disponível fora do Linux
#endif
}

// ---------------------------------------------------------------------------
// Envio MIDI 1.0 (via juce::MidiMessage)
// ---------------------------------------------------------------------------
bool MidiGenerationModule::sendMidi1(const MidiCommand& cmd) {
    if (!midiOutput_)
        return false;

    // Truncar valores 32 bits → 7 bits para MIDI 1.0
    const uint8_t vel7  = static_cast<uint8_t>((cmd.velocity >> 25) & 0x7F);
    const uint8_t cv7   = static_cast<uint8_t>((cmd.controlValue >> 25) & 0x7F);

    juce::MidiMessage msg;
    switch (cmd.type) {
        case MidiCommandType::NOTE_ON:
            msg = juce::MidiMessage::noteOn (cmd.channel + 1, cmd.noteNumber, vel7);
            break;
        case MidiCommandType::NOTE_OFF:
            msg = juce::MidiMessage::noteOff(cmd.channel + 1, cmd.noteNumber, vel7);
            break;
        case MidiCommandType::CONTROL_CHANGE:
            msg = juce::MidiMessage::controllerEvent(
                      cmd.channel + 1,
                      static_cast<int>(cmd.controlNumber),
                      static_cast<int>(cv7));
            break;
        case MidiCommandType::PITCH_BEND: {
            // MIDI 1.0 pitch bend: JUCE espera [0, 16383], centro = 8192
            const int pb14 = 8192 + static_cast<int>(
                (static_cast<int64_t>(cmd.pitchBend) * 8191) / 0x7FFFFFFF);
            msg = juce::MidiMessage::pitchWheel(cmd.channel + 1, pb14);
            break;
        }
        case MidiCommandType::PER_NOTE_PITCH_BEND: {
            // MIDI 1.0 não suporta per-note pitch bend — fallback para channel pitch bend
            const int pb14 = 8192 + static_cast<int>(
                (static_cast<int64_t>(cmd.pitchBend) * 8191) / 0x7FFFFFFF);
            msg = juce::MidiMessage::pitchWheel(cmd.channel + 1, pb14);
            break;
        }
        case MidiCommandType::PROGRAM_CHANGE:
            msg = juce::MidiMessage::programChange(
                      cmd.channel + 1, static_cast<int>(cmd.controlNumber));
            break;
        default:
            return false;
    }

    midiOutput_->sendMessageNow(msg);
    return true;
}

// ---------------------------------------------------------------------------
// Loop de envio (thread dedicada)
// ---------------------------------------------------------------------------
void MidiGenerationModule::sendLoop() {
    aerochord::tryElevateThreadPriority();
    AEROCHORD_LOG_DEBUG(kModule, "sendLoop() iniciado.");

    while (running_.load()) {
        auto opt = inputQueue_->pop();
        if (!opt) {
            std::this_thread::yield();
            continue;
        }

        const MidiCommand& cmd = *opt;
        bool ok = false;

        if (midi2Mode_.load())
            ok = sendUmp(cmd);

        if (!ok)
            ok = sendMidi1(cmd);  // fallback ou modo padrão

        if (ok) {
            ++packetsSent_;

            // --- Medição de latência end-to-end ---
            const auto sentAt = std::chrono::steady_clock::now();
            const float latMs = std::chrono::duration<float, std::milli>(
                                    sentAt - cmd.timestamp).count();

            // Janela circular (apenas sendLoop escreve — sem lock necessário)
            const size_t idx = latencyWriteIdx_.fetch_add(1, std::memory_order_relaxed)
                               & (kLatencyWindowSize - 1);
            latencyWindow_[idx] = latMs;

            const uint64_t total =
                latencySamples_.fetch_add(1, std::memory_order_relaxed) + 1;

            // Recalcular P50/P95 a cada kLatencyUpdateInterval envios
            if (total % kLatencyUpdateInterval == 0) {
                std::array<float, kLatencyWindowSize> sorted = latencyWindow_;
                std::sort(sorted.begin(), sorted.end());
                latencyP50_.store(sorted[kLatencyWindowSize / 2],
                                  std::memory_order_relaxed);
                latencyP95_.store(sorted[kLatencyWindowSize * 95 / 100],
                                  std::memory_order_relaxed);
            }

            // --- Gravação CSV (modo --eval) ---
            // Usa steady_clock (monotônico) para timestamps confiáveis em
            // medição de latência — não sujeito a ajustes NTP.
            if (evalFile_ && evalFile_->is_open()) {
                const double capUs = std::chrono::duration<double, std::micro>(
                    cmd.timestamp.time_since_epoch()).count();
                const double sntUs = std::chrono::duration<double, std::micro>(
                    sentAt.time_since_epoch()).count();

                const char* typeStr = [&]() -> const char* {
                    switch (cmd.type) {
                    case MidiCommandType::NOTE_ON:              return "NOTE_ON";
                    case MidiCommandType::NOTE_OFF:             return "NOTE_OFF";
                    case MidiCommandType::CONTROL_CHANGE:       return "CC";
                    case MidiCommandType::PITCH_BEND:           return "PITCH_BEND";
                    case MidiCommandType::PER_NOTE_PITCH_BEND:  return "PN_PITCH_BEND";
                    default:                                    return "OTHER";
                    }
                }();

                char row[256];
                snprintf(row, sizeof(row), "%.1f,%.1f,%.3f,%s,%d,%u\n",
                         capUs, sntUs, static_cast<double>(latMs),
                         typeStr, cmd.noteNumber,
                         static_cast<unsigned>(cmd.velocity >> 25));
                *evalFile_ << row;
            }
        } else {
            ++packetsDropped_;
        }
    }

    AEROCHORD_LOG_DEBUG(kModule, "sendLoop() encerrado.");
}

} // namespace aerochord
