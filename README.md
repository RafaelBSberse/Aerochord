# Aerochord

Hiperinstrumento digital que transforma gestos de mão (capturados por webcam RGB) em eventos musicais MIDI 2.0 em tempo real.

**TCC de Engenharia de Software** — implementação do TCC 2.

---

## Arquitetura

Pipeline multithreaded com 5 módulos principais e um módulo de visualização opcional, comunicados por filas lock-free SPSC:

```
Webcam
  │
  ▼
[Captura]            src/capture/             ← V4L2 / DirectShow / AVFoundation
  │  VideoFrame
  ▼
[Detecção de Pose]   src/pose_detection/      ← MediaPipe Hand Landmarker
  │  HandLandmarks
  ▼
[Análise de Gestos]  src/gesture_analysis/    ← FSM + EMA
  │  GestureEvent
  ▼
[Mapeamento]         src/mapping/             ← perfis configuráveis, legato, programa GM
  │  MidiCommand
  ▼
[Geração MIDI]       src/midi_generation/     ← JUCE + ALSA UMP (MIDI 2.0)
  │
  ▼
Sintetizador (FluidSynth, DAW, hardware)

(paralelo) [Visualização]  src/visualization/   ← OpenCV: preview, HUD e controles
```

Meta de latência end-to-end: **< 30 ms**.

---

## Pré-requisitos

| Dependência | Versão mínima | Obtida por |
|-------------|---------------|------------|
| CMake       | 3.22          | Sistema    |
| Compilador C++17 | GCC 11 / Clang 13 / MSVC 2019 | Sistema |
| JUCE        | 8.x           | FetchContent automático |
| MediaPipe C++ Tasks | qualquer | Manual — ver abaixo |
| OpenCV (core, highgui, imgproc) | 4.x | Sistema (`libopencv-dev`) — opcional |
| ALSA (Linux) | 1.2.10+      | `libasound2-dev` — opcional, para UMP nativo |
| FluidSynth + SoundFont | qualquer | `fluidsynth fluid-soundfont-gm` — para o `run.sh` |

### MediaPipe

MediaPipe não tem integração CMake oficial. Duas opções:

**Opção A — binários pré-compilados (recomendada):**
1. Baixar os artefatos do [MediaPipe Hand Landmarker](https://developers.google.com/mediapipe/solutions/vision/hand_landmarker) para C++.
2. Descompactar em `<algum-caminho>/mediapipe-sdk/`.
3. Passar `-DMEDIAPIPE_ROOT=<algum-caminho>/mediapipe-sdk` ao CMake.

**Opção B — build a partir do código-fonte:**
Seguir o [guia oficial do MediaPipe](https://google.github.io/mediapipe/getting_started/install.html) (requer Bazel).

> O modelo `assets/hand_landmarker.task` já vem versionado no repositório.

Sem `-DMEDIAPIPE_ROOT`, o projeto compila com um **stub** que mantém o pipeline ativo com landmarks zerados — útil para desenvolver e testar os módulos downstream.

---

## Build

```bash
# Stub MediaPipe (desenvolvimento)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Com MediaPipe real
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMEDIAPIPE_ROOT=/caminho/para/mediapipe-sdk

# Com testes
cmake -B build -DAEROCHORD_BUILD_TESTS=ON

# Compilar
cmake --build build -j$(nproc)
```

---

## Execução

### Recomendada — `run.sh` (Linux com FluidSynth)

```bash
sudo apt install fluidsynth fluid-soundfont-gm
./run.sh
```

O script sobe o FluidSynth, descobre a porta ALSA dele e inicia o Aerochord conectando a saída MIDI direto no sintetizador. Aceita os mesmos argumentos do binário (passados após o `./run.sh`).

### Direta

```bash
./build/aerochord_artefacts/Debug/aerochord [opções]
```

### Opções de linha de comando

| Flag             | Descrição                                                                 |
|------------------|---------------------------------------------------------------------------|
| `--device N`     | Índice da webcam (default: 0)                                             |
| `--midi-out NOME`| Nome (parcial) da porta MIDI de saída                                     |
| `--res WxH`      | Resolução desejada (ex: `1280x720`); default: máxima detectada            |
| `--width N`      | Largura individual (combina com `--height`)                               |
| `--height N`     | Altura individual                                                         |
| `--fps N`        | FPS desejado; default: maior FPS suportado pela câmera                    |
| `--no-viz`       | Desabilita janela OpenCV de visualização                                  |
| `--eval`         | Grava CSV de latência (`aerochord_eval_<timestamp>.csv`)                  |

### Testes

```bash
ctest --test-dir build --output-on-failure
```

---

## Janela de visualização e controles em runtime

Quando OpenCV está disponível, o Aerochord abre uma janela com:

- **Preview da webcam** com landmarks sobrepostos
- **Guia de notas** mostrando as 12 zonas verticais da escala cromática (com gap de tolerância no topo e base — a mão pode sair um pouco do quadro sem perder a nota mais grave/aguda)
- **HUD** com FPS, modo MIDI (1.0 / 2.0), nota ativa, frames descartados
- **Painel inferior** com:
  - Toggle de **legato** (ligar/desligar a sobreposição entre notas durante uma transição com pinça mantida)
  - Seletor de **instrumento** (20 vozes General MIDI: piano, e.piano, marimba, organ, etc.)

`Ctrl+C` no terminal encerra o pipeline limpo.

---

## Mapeamento gesto–som

| Função              | Mão       | Gesto                                              |
|---------------------|-----------|----------------------------------------------------|
| Seleção de oitava   | Esquerda  | Pinça + movimento vertical                         |
| Volume global       | Esquerda  | Mão aberta + movimento vertical                    |
| Disparo de nota     | Direita   | Pinça (polegar + indicador) em zona vertical       |
| Parada de nota      | Direita   | Soltar a pinça                                     |
| Legato              | Direita   | Mover entre zonas mantendo a pinça (toggle na UI)  |
| Timbre / Filtro     | Direita   | Distância dedo médio–palma (com pinça ativa)       |
| Pitch Bend          | Direita   | Inclinação frente/trás (com pinça ativa)           |
| Vibrato             | Direita   | Oscilação lateral leve (com pinça ativa)           |
| Velocity            | Direita   | Velocidade de fechamento da pinça                  |

Quando a mão sai do quadro durante um pinch hold, o Aerochord emite `NOTE_OFF` automaticamente após 200 ms — evita notas presas.

---

## Estrutura de diretórios

```
Aerochord/
├── CMakeLists.txt
├── cmake/
│   └── FetchDependencies.cmake     # JUCE, MediaPipe, ALSA, OpenCV
├── run.sh                          # Launcher Linux com FluidSynth
├── assets/
│   └── hand_landmarker.task        # Modelo MediaPipe (versionado)
├── src/
│   ├── main.cpp                    # Entry point — CLI e montagem do pipeline
│   ├── common/                     # Tipos, fila lock-free, logger, thread utils
│   ├── capture/                    # CaptureModule (V4L2 / DirectShow / AVFoundation)
│   ├── pose_detection/             # PoseDetectionModule (MediaPipe Tasks)
│   ├── gesture_analysis/           # GestureAnalysisModule (FSM + EMA)
│   ├── mapping/                    # MappingModule (perfis, legato, GM program)
│   ├── midi_generation/            # MidiGenerationModule (JUCE + ALSA UMP)
│   └── visualization/              # VisualizationModule (OpenCV + UI)
├── tests/                          # GoogleTest: queue, gesture, mapping, latency
└── tools/
    └── analyze_eval.py             # Análise dos CSVs gerados com --eval
```

---

## Convenções de código

- Filas lock-free (SPSC) para **toda** comunicação entre módulos
- Threads de visão e MIDI nunca bloqueadas (sem mutex no caminho crítico)
- Timestamps inseridos nos pacotes UMP **imediatamente** após detecção do gesto
- Logging estruturado em todos os módulos (`AEROCHORD_LOG_*`)
- Filtro EMA aplicado nos landmarks **antes** de enviar à análise de gestos
- Controles em runtime (UI → MappingModule) trocam estado via `std::atomic` — nunca mutex no hot path
