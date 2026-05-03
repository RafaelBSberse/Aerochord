#!/usr/bin/env bash
# =============================================================================
# run.sh — Inicia o Aerochord com FluidSynth como sintetizador
#
# Uso:
#   ./run.sh                  # webcam 0, sem eval
#   ./run.sh --device 1       # webcam 1
#   ./run.sh --eval           # gravar CSV de latência
#   ./run.sh --no-viz         # sem janela de visualização
#
# Requisitos:
#   sudo apt install fluidsynth fluid-soundfont-gm
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AEROCHORD_BIN="$SCRIPT_DIR/build/aerochord_artefacts/Debug/aerochord"
SOUNDFONT="/usr/share/sounds/sf2/FluidR3_GM.sf2"

# ---------------------------------------------------------------------------
# Verificações
# ---------------------------------------------------------------------------
if [[ ! -x "$AEROCHORD_BIN" ]]; then
    echo "[run.sh] Binário não encontrado: $AEROCHORD_BIN"
    echo "         Execute: cmake -B build && cmake --build build"
    exit 1
fi

if ! command -v fluidsynth &>/dev/null; then
    echo "[run.sh] FluidSynth não instalado."
    echo "         Execute: sudo apt install fluidsynth fluid-soundfont-gm"
    exit 1
fi

if [[ ! -f "$SOUNDFONT" ]]; then
    echo "[run.sh] SoundFont não encontrado: $SOUNDFONT"
    echo "         Execute: sudo apt install fluid-soundfont-gm"
    exit 1
fi

# ---------------------------------------------------------------------------
# Cleanup ao sair (mata processos filhos)
# ---------------------------------------------------------------------------
FLUID_PID=""
AEROCHORD_PID=""

cleanup() {
    echo ""
    echo "[run.sh] Encerrando..."
    [[ -n "$AEROCHORD_PID" ]] && kill "$AEROCHORD_PID" 2>/dev/null && wait "$AEROCHORD_PID" 2>/dev/null
    [[ -n "$FLUID_PID" ]]     && kill "$FLUID_PID" 2>/dev/null && wait "$FLUID_PID" 2>/dev/null
    echo "[run.sh] Encerrado."
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# 1. Iniciar FluidSynth em background
# ---------------------------------------------------------------------------
echo "[run.sh] Iniciando FluidSynth..."

# Detectar driver de áudio: pipewire > pulseaudio > alsa
AUDIO_DRIVER="alsa"
if pgrep -x pipewire &>/dev/null; then
    AUDIO_DRIVER="pipewire"
elif pgrep -x pulseaudio &>/dev/null; then
    AUDIO_DRIVER="pulseaudio"
fi

fluidsynth \
    -a "$AUDIO_DRIVER" \
    -m alsa_seq \
    -g 2.0 \
    -o midi.autoconnect=0 \
    --server \
    "$SOUNDFONT" &
FLUID_PID=$!

# Aguardar FluidSynth registrar porta ALSA
echo "[run.sh] Aguardando FluidSynth inicializar..."
for i in $(seq 1 30); do
    if aconnect -l 2>/dev/null | grep -q "FLUID Synth"; then
        break
    fi
    sleep 0.2
done

if ! aconnect -l 2>/dev/null | grep -q "FLUID Synth"; then
    echo "[run.sh] ERRO: FluidSynth não registrou porta ALSA em 6s."
    exit 1
fi

echo "[run.sh] FluidSynth ativo (PID $FLUID_PID, driver: $AUDIO_DRIVER)."

# ---------------------------------------------------------------------------
# 2. Descobrir o nome JUCE do dispositivo MIDI do FluidSynth
#
# JUCE no Linux lista portas ALSA seq como "<port_name>".
# FluidSynth registra uma porta chamada "Synth input port (PID:0)".
# Precisamos passar esse nome via --midi-out para que o Aerochord
# envie MIDI diretamente ao FluidSynth, não ao "Midi Through Port-0".
# ---------------------------------------------------------------------------
FLUID_PORT_NAME=$(aconnect -l | grep -A1 "FLUID Synth" | grep "Synth input" | sed "s/.*'\(.*\)'.*/\1/" | head -1)

if [[ -z "$FLUID_PORT_NAME" ]]; then
    echo "[run.sh] AVISO: Não encontrou porta de entrada do FluidSynth."
    echo "         O Aerochord usará o primeiro dispositivo MIDI disponível."
    MIDI_OUT_ARG=""
else
    echo "[run.sh] FluidSynth MIDI port: '$FLUID_PORT_NAME'"
    MIDI_OUT_ARG="--midi-out"
    MIDI_OUT_VAL="$FLUID_PORT_NAME"
fi

# ---------------------------------------------------------------------------
# 3. Iniciar Aerochord
# ---------------------------------------------------------------------------
echo "[run.sh] Iniciando Aerochord..."

if [[ -n "${MIDI_OUT_ARG:-}" ]]; then
    "$AEROCHORD_BIN" $MIDI_OUT_ARG "$MIDI_OUT_VAL" "$@" &
else
    "$AEROCHORD_BIN" "$@" &
fi
AEROCHORD_PID=$!

sleep 1

# ---------------------------------------------------------------------------
# 4. Exibir status
# ---------------------------------------------------------------------------
echo ""
echo "============================================"
echo "  Aerochord + FluidSynth ativos"
echo "  MIDI → $FLUID_PORT_NAME"
echo "  Áudio → $AUDIO_DRIVER"
echo ""
echo "  Coloque as mãos na frente da webcam."
echo "  Mão direita: pinça = toca nota"
echo "  Mão esquerda: altura = oitava/volume"
echo ""
echo "  Ctrl+C para encerrar"
echo "============================================"
echo ""

wait "$AEROCHORD_PID" 2>/dev/null || true
AEROCHORD_PID=""
