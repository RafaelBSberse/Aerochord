# Capítulo 5 — Proposta de Solução: Aerochord

## Visão Geral do Capítulo

Este capítulo propõe a arquitetura de software, o modelo de interação e as tecnologias do Aerochord como hiperinstrumento digital. Cobre:
- **5.1** — Visão geral e requisitos do sistema (funcionais e não funcionais)
- **5.2** — Arquitetura de software proposta (5 módulos principais)
- **5.2.1** — Mapeamento Gesto–Som Preliminar (Tabela 2)
- **5.2.2** — Tecnologias e ferramentas (C++, JUCE, MediaPipe, CMake)
- **5.2.3** — Desafios de implementação e estratégias de mitigação

**Contexto:** Este capítulo serve como plano para a implementação no TCC 2. Tudo aqui é proposta a ser validada empiricamente.

---

## 5.1 Visão Geral e Requisitos do Sistema

### O que o Aerochord faz

Pipeline de processamento modular que:
1. Captura quadros de vídeo de uma webcam RGB
2. Extrai landmarks de mãos via MediaPipe
3. Reconhece gestos
4. Traduz gestos em mensagens MIDI 2.0 de alta resolução

### Requisitos Funcionais

| # | Requisito |
|---|-----------|
| RF1 | Capturar e processar em tempo real o feed de vídeo de uma webcam padrão |
| RF2 | Detectar e rastrear landmarks das mãos via MediaPipe |
| RF3 | Reconhecer gestos estáticos e dinâmicos: pinça (disparo de nota), posicionamento em áreas discretas (pitch), distância dedo médio–palma (controle de filtro) |
| RF4 | Traduzir gestos em mensagens MIDI 2.0: Note On/Off com velocity de alta resolução, Control Change de alta resolução, per-note controllers |
| RF5 | Suportar validação empírica de usabilidade via módulos desacoplados |

### Requisitos Não Funcionais

| # | Requisito | Meta |
|---|-----------|------|
| RNF1 | **Latência end-to-end** (captura do quadro → envio MIDI) | < 30 ms em hardware de consumo |
| RNF2 | **Acessibilidade** | Sem hardware além de câmera RGB comum; operar em CPU de consumo |
| RNF3 | **Multiplataforma** | C++ + JUCE → Windows, macOS, Linux |
| RNF4 | **Robustez e usabilidade** | Calibragem guiada, feedback visual em tempo real, tolerância a variações de iluminação e posição da câmera |

---

## 5.2 Arquitetura de Software Proposta

Arquitetura **modular multithreaded**, com módulos interconectados por **filas lock-free**.

### Pipeline de Dados (fluxo)

```
Webcam / Fonte de Vídeo
        ↓ quadros brutos
Módulo de Captura (thread dedicada)
        ↓ frames
Fila: Frames (lock-free)
        ↓ frames
Módulo de Detecção de Pose (thread dedicada)
        ↓ landmarks + confiança
Fila: Landmarks (lock-free)
        ↓ landmarks
Módulo de Análise de Gestos (thread dedicada)
        ↓ eventos de gestos
Fila: Eventos de Gestos (lock-free)
        ↓ eventos de alto nível
Módulo de Mapeamento (thread dedicada)
        ↓ eventos musicais
Fila: Eventos MIDI (lock-free)
        ↓ eventos MIDI 2.0 / 1.0
Módulo de Geração MIDI (thread dedicada)
        ↓ pacotes UMP / mensagens legadas
Dispositivo MIDI

(Lateral: Calibragem & Feedback ↔ Módulo de Mapeamento)
(Lateral: estatísticas de rastreamento ↔ módulos de Pose e Análise)
```

> Ver Figura 8 no PDF original para o diagrama visual da arquitetura.

---

### 5.2.0 Módulo de Captura

**Responsabilidade:** Aquisição de quadros de vídeo da webcam em tempo real

**Detalhes de implementação:**
- Camada de abstração para múltiplos backends:
  - Windows: DirectShow ou Media Foundation
  - macOS: AVFoundation
  - Linux: V4L2
- Interface única exposta ao restante do pipeline
- **Buffering circular** com tamanho configurável para minimizar jitter
- Thread dedicada à leitura de quadros

---

### 5.2.1a Módulo de Detecção de Pose

**Responsabilidade:** Extração de landmarks das mãos a partir dos quadros capturados

**Tecnologia:** MediaPipe (rastreamento de mãos em CPU de consumo)

**Saída:** Coordenadas normalizadas dos 21 landmarks por mão + métricas de confiança → inseridas em fila lock-free

**Filtro de suavização:** Média móvel ponderada aplicada para mitigar jitter antes do envio ao módulo de análise

---

### 5.2.1b Módulo de Análise de Gestos

**Responsabilidade:** Interpretar sequências/estados de landmarks como gestos de alto nível

**Implementação:** Máquina de estados finitos (FSM) com:
- Condições de contexto e transições explícitas (ex: "pinça iniciada")
- Regras de transição inequívocas entre gestos conflitantes

**Métricas de reconhecimento:**
- Distância euclidiana entre pontos
- Vetores de velocidade
- Ângulos de inclinação

**Saída:** Eventos de alto nível encapsulados, enfileirados para o módulo de mapeamento

---

### 5.2.1c Módulo de Mapeamento

**Responsabilidade:** Traduzir eventos de gestos em comandos musicais

**Filosofias aplicadas:** movement-first e sound-first

**Características:**
- Camada de regras configuráveis: cada gesto de alto nível → parâmetros musicais (note number, velocity, etc.) levando em conta contexto corrente (oitava, volume)
- **Perfis de mapeamento mutáveis em tempo de execução** — facilita experimentos de usabilidade
- **Calibragem dinâmica** via Property Exchange (MIDI-CI quando disponível) — ajusta curvas de sensibilidade por dispositivo receptor

---

### 5.2.1d Módulo de Geração MIDI

**Responsabilidade:** Empacotar e despachar mensagens MIDI 2.0 via Universal MIDI Packets

**Framework:** JUCE (acesso multiplataforma às APIs de saída MIDI)

**Fluxo:**
1. Handshake via MIDI-CI → detecta suporte a MIDI 2.0 no receptor
2. Se suportado: envia eventos em UMP 32/64 bits com alta resolução
3. Se não suportado: converte automaticamente para MIDI 1.0 em 7 bits (transparente ao usuário)

**Thread separada**, consumindo eventos via fila lock-free — o envio não interfere no processamento de visão.

**Timestamps** inseridos nos pacotes UMP para medições de latência end-to-end.

---

## 5.2.1 Mapeamento Gesto–Som Preliminar

> Esta é uma **hipótese de design** sujeita a refinamentos nos testes com usuários do TCC 2.

### Tabela 2 — Mapeamento Gesto–Som Preliminar

| Função Musical | Mão | Gesto Proposto | Justificativa |
|---------------|-----|----------------|---------------|
| Seleção de Oitava | Esquerda | Pinça + movimento vertical | Gesto modal claro que separa controle de oitava de outras funções, evitando acionamentos acidentais |
| Controle de Volume Global | Esquerda | Mão aberta + movimento vertical | Mapeamento intuitivo (cima/baixo = mais/menos); usa gesto de "repouso" da mão |
| Disparo e Altura da Nota | Direita | Gesto de pinça (polegar + indicador) em zona vertical | Gesto ergonômico e preciso para "ativar" uma nota; altura da mão define o pitch intuitivamente |
| Parada de Nota | Direita | Soltar o gesto de pinça | Ação de "soltar" é a contraparte natural e imediata do gesto de disparo |
| Controle de Timbre/Filtro | Direita | Distância entre ponta do dedo médio e a palma (com pinça ativa) | Gesto natural de "abrir/fechar" a mão; robusto para rastreamento e de grande amplitude expressiva |
| Pitch Bend por Nota | Direita | Inclinação da mão para frente/trás (com pinça ativa) | Movimento ortogonal ao de seleção de nota — minimiza conflitos de controle |
| Vibrato Manual | Direita | Oscilação leve lateral (com pinça ativa) | Simula gesto físico de vibrato de instrumentos de corda — muito intuitivo |
| Intensidade da Nota (Velocity) | Direita | Velocidade de fechamento da pinça | Mapeamento direto que imita física de percussão (tocar mais rápido = som mais intenso) |

**Observações sobre o design:**
- Todos os gestos minimizam ambiguidade para facilitar uso por qualquer pessoa
- A mão esquerda é **minimamente utilizada** → abre margem para extensões futuras
- Distinção clara entre ações **discretas** (Note On/Off, seleção de oitava) e **contínuas** (volume, filtro, pitch bend)
- Parâmetros (thresholds, falsos positivos, ergonomia) serão ajustados nos testes de usabilidade do TCC 2

---

## 5.2.2 Tecnologias e Ferramentas Propostas

### C++ (linguagem principal)

**Por quê:**
- Controle de baixo nível sobre alocação de memória
- Otimizações em tempo de compilação
- Binários de alto desempenho com determinismo
- Bibliotecas nativas para threads, sincronização atômica e estruturas lock-free
- Compatibilidade multiplataforma e controle sobre fallback para MIDI 1.0

### JUCE (framework de áudio/MIDI)

**Por quê:**
- Abstrações multiplataforma para áudio e MIDI
- Suporte nativo a Universal MIDI Packet
- Integração com APIs nativas: CoreMIDI (macOS/iOS), Windows MIDI 2.0 API, ALSA/JACK (Linux)
- Gerencia handshake MIDI-CI e Property Exchange
- Encapsula lógica de fallback automático para mensagens legadas
- Histórico de adoção na indústria musical → interoperabilidade com DAWs

### MediaPipe (detecção de pose de mãos)

**Por quê:**
- Desempenho comprovado em CPU de consumo (LUGARESI et al., 2019)
- Precisão de rastreamento em tempo real
- Alinha ao pilar de acessibilidade (sem hardware especializado)
- Modular → permite inclusão futura de modelos customizados de IA para gestos específicos

### Infraestrutura de Build e Testes

| Ferramenta | Uso |
|-----------|-----|
| **CMake** | Build multiplataforma; targets para desktop e potencial extensão a embarcados/móveis |
| **perf** (Linux) | Profiling de tempo de execução, gargalos |
| **Instruments** (macOS) | Profiling |
| **Visual Studio Profiler** (Windows) | Profiling |
| Mecanismos de monitoramento em tempo real | Validação do pipeline e diagnóstico em testes |

---

## 5.2.3 Desafios de Implementação e Estratégias de Mitigação

### Desafio 1: Latência End-to-End

**Problema:** Cada módulo do pipeline adiciona latência. Meta: < 30 ms total.

**Estratégias:**
- Arquitetura **multithreaded** com threads dedicadas por módulo
- Comunicação por **filas lock-free** (operação não bloqueante)
- Threads de visão e envio MIDI priorizadas no sistema operacional
- **Timestamps nos pacotes UMP** imediatamente após detecção do gesto
- Ferramentas de profiling contínuo para identificar gargalos de CPU ou I/O
- Ajuste de parâmetros: tamanho de buffers circulares, taxa de captura de frames

---

### Desafio 2: Estabilidade do Rastreamento de Mãos

**Problema:** Condições adversas de iluminação, perda temporária de detecção, baixa confiança.

**Estratégias:**
- **Filtro de suavização** (média móvel ponderada ou filtro exponencial de baixa ordem) aplicado após extração de coordenadas pelo MediaPipe
- **Lógica de fallback:** usa dados anteriores para manter estado de gestos durante perda temporária de detecção → evita disparos acidentais
- **Thresholds dinâmicos de confiança** ajustados com base em estatísticas de runtime
- Documentação e medição de cenários de falha para refinamento no TCC 2

---

### Desafio 3: Ambiguidade na Interpretação de Gestos

**Problema:** Gestos de funções diferentes podem compartilhar parâmetros semelhantes (ex: pinça + inclinação vs. pinça + oscilação para vibrato).

**Estratégias:**
- **FSM (Máquina de Estados Finitos)** com transições inequívocas, levando em conta contexto atual (modo de operação, oitava selecionada, estado anterior)
- Condições de entrada/saída de cada estado baseadas em métricas claras (distância, velocidade, ângulo) + janelas temporais
- **Lógica de debounce** e verificação de consistência temporal antes de emitir eventos musicais
- Ajustes iterativos dos parâmetros em testes de usabilidade para equilibrar sensibilidade e robustez

---

### Desafio 4: Interoperabilidade com Dispositivos MIDI

**Problema:** Variações no suporte a MIDI 2.0 entre dispositivos; múltiplos transportes (USB, BLE, RTP, Web).

**Estratégias:**
- **Handshake MIDI-CI** → detecta capacidades e aplica fallback automático para MIDI 1.0
- Testes com diferentes sintetizadores e DAWs
- Simuladores de endpoints MIDI-CI durante o desenvolvimento
- Abstração de saída MIDI encapsulando particularidades de cada API
- Logging de interações de Property Exchange para identificar dispositivos com suporte parcial

---

### Desafio 5: Debug e Monitoração Contínua

**Estratégias:**
- **Logs estruturados** rastreando eventos de: detecção de pose, reconhecimento de gestos, decisões da FSM, envios de pacotes MIDI
- Ferramentas de profiling para análise de CPU, latências por etapa e contagem de threads ativas
- Testes de regressão automatizados em ambiente controlado
- Possível vetorização/aceleração SIMD em algoritmos críticos

---

## Conclusão do Capítulo 5

A arquitetura modular multithreaded com filas lock-free, combinada com C++/JUCE/MediaPipe, forma a base técnica para atingir os três pilares do Aerochord:

| Pilar | Como é atendido |
|-------|----------------|
| **Baixa latência** | Multithreading, lock-free queues, timestamps UMP, profiling contínuo |
| **Acessibilidade** | Apenas webcam RGB + CPU de consumo; sem hardware especializado |
| **Alta expressividade** | MIDI 2.0 com UMP, per-note controllers, 32 bits de resolução, calibragem via Property Exchange |

Este plano serve de base para implementação e validação experimental no TCC 2.
