# Plano de Implementação Faseada — Aerochord (TCC 2)

## Estado atual (pré-implementação)

| Componente | Estado |
|---|---|
| LockFreeQueue, PipelineTypes, Logger | ✓ Completo |
| GestureAnalysisModule (FSM + métricas geométricas) | ✓ Completo |
| MappingModule (perfis + translators) | ✓ Completo |
| MidiGenerationModule (MIDI 1.0 via JUCE) | ✓ Funcional |
| CaptureModule (backend câmera) | ⚠️ Stub |
| PoseDetectionModule (MediaPipe) | ⚠️ Stub |
| MidiGenerationModule (MIDI 2.0 / UMP / MIDI-CI) | ⚠️ Stub |

---

## Fase 1 — Núcleo do Sistema (Semanas 1–5)

### 1.1 Módulo de Captura

**Objetivo:** Substituir stub por backend real de câmera multiplataforma.

**Arquivos a editar:**
- `src/capture/CaptureModule.h` — declarar `openCamera()` / `closeCamera()` como privados
- `src/capture/CaptureModule.cpp` — implementar `captureLoop()` com backend real via `#ifdef`:
  - Linux: V4L2 (`<linux/videodev2.h>`)
  - macOS: AVFoundation (Objective-C++)
  - Windows: Media Foundation (`mfapi.h`)

**Dependências:** Nenhuma — é o módulo de entrada do pipeline.

**Critério de conclusão:**
- `captureLoop()` lê frames reais da webcam e enfileira em `frameQueue`
- `framesDropped_` permanece 0 em condições normais (30 fps, 640×480)
- Log `AEROCHORD_LOG_INFO` confirma frames capturados por segundo

---

### 1.2 Módulo de Detecção de Pose

**Objetivo:** Integrar MediaPipe C++ para extração real de landmarks das mãos.

**Arquivos a editar:**
- `cmake/FetchDependencies.cmake` — ativar Opção A (pré-compilado via `MEDIAPIPE_ROOT`) ou Opção B (build Bazel)
- `src/pose_detection/PoseDetectionModule.h` — completar struct `MediaPipeHandle` com ponteiros reais
- `src/pose_detection/PoseDetectionModule.cpp` — substituir bloco `#ifdef AEROCHORD_MEDIAPIPE_STUB` pela inicialização real

**Fluxo de implementação:**
1. Compilar MediaPipe para Linux (plataforma primária de desenvolvimento)
2. Passar `-DMEDIAPIPE_ROOT=<path>` ao CMake
3. Inicializar `mp::tasks::vision::HandLandmarker` em `start()`
4. Suavização EMA já implementada — apenas alimentar com dados reais

**Dependências:** `CaptureModule` funcional (consome `frameQueue`).

**Critério de conclusão:**
- `detectionFailures_` < 5% dos frames em iluminação ambiente normal
- `confidence >= 0.5f` para mão a ~50 cm da câmera
- Landmarks visivelmente suavizados (sem tremor em mão parada)

**⚠️ Maior risco técnico do projeto.** Integração MediaPipe C++ é complexa: ABI, versão da biblioteca, Bazel vs. pré-compilado. Iniciar na semana 1; manter stub ativo para desenvolvimento paralelo dos outros módulos.

---

### 1.3 Módulo de Geração MIDI — MIDI 2.0

**Objetivo:** Implementar `sendUmp()` e `performMidiCiHandshake()` (MIDI 1.0 já funciona).

**Arquivos a editar:**
- `src/midi_generation/MidiGenerationModule.cpp`:
  - `performMidiCiHandshake()` — sequência Universal SysEx Discovery (type `0x0D`) + aguardar resposta com timeout `midiCiTimeoutMs`; se sem resposta → `midi2Mode_ = false`
  - `sendUmp()` — construir UMP 32/64 bits via `juce::MidiMessage` para NOTE_ON/OFF (velocity 32-bit), CC (32-bit), PITCH_BEND (32-bit)

**Dependências:** `MappingModule` funcional (consome `midiQueue`).

**Critério de conclusão:**
- Handshake detecta corretamente suporte a MIDI 2.0 no receptor
- Fallback transparente para MIDI 1.0 (sem intervenção do usuário)
- Note On/Off com velocity 32-bit enviados corretamente em receptor MIDI 2.0

---

### 1.4 Módulo de Visualização

**Objetivo:** Janela em tempo real exibindo o frame da câmera com os 21 landmarks de cada mão sobrepostos — ferramenta de calibragem, validação e feedback visual (RNF4).

**Tecnologia:** OpenCV (`cv::imshow`) — já disponível como dependência transitiva do MediaPipe; sem custo adicional de build.

**Arquivos a criar:**
- `src/visualization/VisualizationModule.h` — classe `VisualizationModule`
- `src/visualization/VisualizationModule.cpp` — implementação
- `src/visualization/CMakeLists.txt` — target `aerochord_visualization` linkando `opencv_core`, `opencv_highgui`, `opencv_imgproc`
- Atualizar `CMakeLists.txt` raiz e `src/main.cpp` para incluir o módulo

**Integração no pipeline:**
```
PoseDetectionModule
    ↓ (callback opcional)
VisualizationModule  ←→  thread dedicada
    ↓
cv::imshow("Aerochord", frame_com_landmarks)
```

`PoseDetectionModule` recebe um ponteiro opcional para `VisualizationModule`. Após processar cada frame, chama `viz->pushFrame(frame, landmarks)` se o ponteiro não for nulo. O módulo de visualização roda em thread própria para não bloquear o pipeline.

**O que a janela deve exibir:**
- Frame da câmera (640×480, colorido)
- 21 landmarks por mão (círculos `cv::circle`) nas coordenadas desnormalizadas
- Conexões entre landmarks (esqueleto da mão) desenhadas com `cv::line`
- Texto sobreposto: FPS atual, confiança de detecção por mão, modo MIDI (1.0 / 2.0)
- Flag `--no-viz` desativa a janela (para execução headless ou avaliação com usuários)

**Dependências:** `PoseDetectionModule` funcional (com MediaPipe real).

**Critério de conclusão:**
- Janela abre e exibe câmera com landmarks visíveis sem atraso perceptível
- Fecha com tecla `q` ou `ESC` sem travar o pipeline
- Com `--no-viz`, o executável roda sem abrir janela alguma (modo produção)

**⚠️ Atenção:** `cv::imshow` deve ser chamado sempre pela mesma thread (restrição do OpenCV). A thread de visualização deve ser a única a chamar funções de GUI do OpenCV.

---

## Fase 2 — Lógica de Interação (Semanas 4–7)

**Status: substancialmente concluída.** `GestureAnalysisModule` e `MappingModule` já implementados.

**Gestos faltantes da Tabela 2** (a implementar):

| Gesto | Mão | Arquivo a editar |
|-------|-----|-----------------|
| Seleção de oitava (pinça + vertical) | Esquerda | `GestureAnalysisModule.cpp` — `processLeftHand()` |
| Controle de timbre (dedo médio–palma com pinça) | Direita | `GestureAnalysisModule.cpp` — `processRightHand()` |
| Vibrato (oscilação lateral com pinça) | Direita | `GestureAnalysisModule.cpp` — `processRightHand()` |

**Translators correspondentes:**
- `src/mapping/MappingModule.cpp` — adicionar em `buildDefaultProfile()` para cada novo `GestureType`

**Critério de conclusão:**
- Todos os 8 gestos da Tabela 2 reconhecidos e mapeados
- `RIGHT_PITCH_BEND` e `RIGHT_VIBRATO` sem ambiguidade (usar threshold de amplitude para distinguir)

---

## Fase 3 — Integração e Testes (Semanas 8–10)

**Objetivo:** Pipeline completo, medição de latência end-to-end, profiling.

**Arquivos a criar/editar:**
- `src/main.cpp` (linha 101) — implementar loop de estatísticas periódicas (latência P50/P95, frames/s, gestos/min)
- `tests/test_gesture_fsm.cpp` — testes unitários da FSM com landmarks sintéticos
- `tests/test_pipeline_latency.cpp` — mede delta entre `VideoFrame::timestamp` e `MidiCommand::timestamp`
- `tests/CMakeLists.txt` — adicionar targets de teste (ativar com `-DAEROCHORD_BUILD_TESTS=ON`)

**Atividades:**
1. Profiling com `perf stat` / `perf record` (Linux): identificar gargalo por módulo
2. Ajuste de `smoothingAlpha`, tamanho do buffer circular, prioridades de thread
3. Thread de envio MIDI com `SCHED_FIFO` (requer `CAP_SYS_NICE`) para reduzir jitter

**Critério de conclusão:**
- Latência end-to-end medida < 30 ms em P95 no hardware-alvo
- Nenhum deadlock ou busy-wait após 10 min de execução contínua
- Testes unitários da FSM passando (`ctest`)

**⚠️ Risco:** `SCHED_FIFO` requer permissão especial no Linux. Alternativa: `nice -n -10` ou tuning do buffer.

---

## Fase 4 — Avaliação com Usuários (Semanas 11–12)

**Objetivo:** Coletar métricas objetivas (latência, precisão) e subjetivas (SUS, NASA-TLX).

**Arquivos a criar/editar:**
- `src/main.cpp` — modo `--eval`: grava CSV com colunas `timestamp_capture, timestamp_midi, gesture_type, note, velocity, latency_ms`
- `tools/log_analyzer.py` (opcional) — parser CSV para cálculo estatístico (média, P95, taxa de acerto)

**Critério de conclusão:**
- Sessões com ≥ 5 participantes (músicos sem treino formal + pessoas com mobilidade reduzida)
- CSV com todas as colunas necessárias para análise estatística

---

## Fase 5 — Análise e Escrita (Semanas 13–14)

Sem novos arquivos de código. Análise dos CSVs gerados na Fase 4, comparação com metas (latência < 30 ms, SUS ≥ 70), redação do capítulo de Resultados e Discussão.

---

## Fase 6 — Monografia Final e Defesa (Semanas 15–16)

Sem código. Revisão do documento, incorporação de feedback do orientador, elaboração da apresentação.

---

## Matriz de Riscos Técnicos

| Risco | Probabilidade | Impacto | Mitigação |
|---|---|---|---|
| Integração MediaPipe C++ (ABI, build) | Alta | Alto | Começar semana 1; manter stub para desenvolvimento paralelo |
| Latência end-to-end > 30 ms | Média | Alto | Profiling antecipado na Fase 3; design lock-free já mitiga |
| Falta de dispositivo MIDI 2.0 para teste | Média | Médio | Usar loopMIDI + soft-synth compatível; MIDI 1.0 como fallback |
| Ambiguidade FSM entre PITCH_BEND e VIBRATO | Baixa | Médio | Distinguir por amplitude de oscilação; ajustar threshold iterativamente |
| V4L2 instável com webcam específica | Baixa | Médio | Testar com câmera usada no projeto antes do desenvolvimento |

---

## Ordem de Implementação Recomendada

```
Semana 1–2 : CaptureModule — backend V4L2 (Linux)
Semana 2–4 : PoseDetectionModule — integração MediaPipe
Semana 3–4 : VisualizationModule — janela OpenCV com landmarks (depende de MediaPipe)
Semana 3–5 : MidiGenerationModule — MIDI-CI handshake + sendUmp()
Semana 4–6 : GestureAnalysisModule — gestos faltantes da Tabela 2
Semana 6–7 : MappingModule — translators para novos gestos
Semana 8–10: Integração + profiling + testes
Semana 11–12: Avaliação com usuários
Semana 13–16: Análise, escrita e defesa
```

## Arquivos Críticos

| Arquivo | Criticidade |
|---|---|
| `src/capture/CaptureModule.cpp` | Entrada do pipeline — sem câmera nada funciona |
| `src/pose_detection/PoseDetectionModule.cpp` | Maior risco técnico do projeto |
| `cmake/FetchDependencies.cmake` | Controla como MediaPipe é integrado |
| `src/midi_generation/MidiGenerationModule.cpp` | MIDI 2.0 é o diferencial acadêmico do TCC |
| `src/gesture_analysis/GestureAnalysisModule.cpp` | FSM central; 3 gestos da Tabela 2 ainda faltam |
| `src/visualization/VisualizationModule.cpp` | Feedback visual + validação da detecção (RNF4) |

## Verificação Final (pipeline completo)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DMEDIAPIPE_ROOT=/path/to/mediapipe
cmake --build build -j$(nproc)
./build/aerochord_artefacts/Release/aerochord --device 0 --midi-out "loopMIDI"
# Esperado: landmarks detectados, gestos reconhecidos, notas MIDI enviadas, latência < 30 ms no log
```
