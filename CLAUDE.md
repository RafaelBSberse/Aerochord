# CLAUDE.md — Projeto Aerochord

## O que é este projeto

Aerochord é um hiperinstrumento digital que transforma gestos de mão (capturados por webcam RGB) em eventos musicais MIDI 2.0 em tempo real. TCC de Engenharia de Software — está em fase de **proposta** (TCC 1 concluído); a implementação ocorre no TCC 2.

## Arquivos de conhecimento disponíveis

Leia estes arquivos ANTES de qualquer tarefa relacionada:

| Arquivo | Conteúdo | Quando ler |
|---------|----------|-----------|
| `.context/cap3_computacao_e_musica.md` | Fundamentos: MIDI 1.0/2.0, síntese sonora, teoria musical, escala cromática, articulações, efeitos | Qualquer tarefa envolvendo MIDI, áudio, teoria musical |
| `.context/cap5_proposta_aerochord.md` | Arquitetura modular, 5 módulos do pipeline, mapeamento gesto-som (Tabela 2), tecnologias (C++/JUCE/MediaPipe), desafios e mitigações | Qualquer tarefa de implementação |
| `.context/cap6_consideracoes_e_plano.md` | Plano de trabalho, 6 fases do TCC 2, cronograma, artefatos esperados | Tarefas de planejamento, escrita da monografia |

## Stack tecnológica

- **Linguagem:** C++17 ou superior
- **Framework MIDI/Áudio:** JUCE
- **Visão Computacional:** MediaPipe (detecção de landmarks de mãos)
- **Build:** CMake (multiplataforma)
- **Protocolo:** MIDI 2.0 com Universal MIDI Packets (UMP), fallback automático para MIDI 1.0

## Arquitetura: 5 módulos em pipeline multithreaded

```
Captura → Detecção de Pose → Análise de Gestos → Mapeamento → Geração MIDI
```

Cada módulo roda em **thread dedicada**, comunicando-se por **filas lock-free**.

### Módulos e suas responsabilidades

1. **Captura** — webcam RGB, buffering circular, abstração multiplataforma
2. **Detecção de Pose** — MediaPipe, 21 landmarks por mão, filtro de suavização
3. **Análise de Gestos** — FSM (Máquina de Estados Finitos), detecção de pinça, movimentos
4. **Mapeamento** — eventos de gesto → comandos MIDI, perfis configuráveis, calibragem via MIDI-CI
5. **Geração MIDI** — UMP, handshake MIDI-CI, fallback MIDI 1.0

## Mapeamento gesto-som atual (hipótese — sujeito a refinamento)

| Função | Mão | Gesto |
|--------|-----|-------|
| Seleção de oitava | Esquerda | Pinça + movimento vertical |
| Volume global | Esquerda | Mão aberta + movimento vertical |
| Disparo de nota | Direita | Pinça (polegar + indicador) em zona vertical |
| Parada de nota | Direita | Soltar a pinça |
| Timbre/Filtro | Direita | Distância dedo médio–palma (com pinça ativa) |
| Pitch Bend | Direita | Inclinação da mão frente/trás (com pinça ativa) |
| Vibrato | Direita | Oscilação lateral leve (com pinça ativa) |
| Velocity (intensidade) | Direita | Velocidade de fechamento da pinça |

## Metas de qualidade (não funcionais)

- Latência end-to-end: **< 30 ms**
- Hardware mínimo: **câmera RGB comum + CPU de consumo**
- Plataformas: **Windows, macOS, Linux**

## Convenções do projeto

- Sempre usar **filas lock-free** para comunicação entre módulos
- **Nunca bloquear** threads de visão ou de envio MIDI
- Inserir **timestamps nos pacotes UMP** imediatamente após detecção do gesto
- **Logging estruturado** em todos os módulos para debug e medição de latência
- Filtro de suavização (média móvel ponderada) nos landmarks **antes** de enviar para análise de gestos

## Diretórios proibidos (não ler sem necessidade)

- Nenhum diretório sensível identificado ainda. Atualizar conforme estrutura do projeto crescer.

## Status atual do projeto

- TCC 1: **Concluído** (proposta de engenharia, revisão teórica, arquitetura, mapeamento)
- TCC 2: **Em andamento** (implementação, testes, validação)
- Fase atual: Ver `cap6_consideracoes_e_plano.md` → Seção 6.3 para o plano de fases
