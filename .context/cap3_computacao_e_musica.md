# Capítulo 3 — Computação e Música

## Visão Geral

Este capítulo apresenta a base teórica para o desenvolvimento computacional do Aerochord, cobrindo três eixos principais:
- **Seção 3.1** — Fundamentos da computação musical (representação digital do som, síntese sonora, processamento de áudio)
- **Seção 3.2** — Protocolo MIDI (fundamentos do MIDI 1.0, arquitetura do MIDI 2.0, capacidades expressivas, persistência e justificativa de uso no Aerochord)
- **Seção 3.3** — Teoria musical aplicada ao mapeamento gestual (escala cromática, oitavas, articulações, efeitos de expressividade)

---

## 3.1 Fundamentos da Computação Musical

A computação musical aplica algoritmos e modelos computacionais para criar, analisar e modificar sons. Abrange desde a geração programada de timbres até a modelagem de estruturas musicais e sistemas de performance interativa (MIRANDA, 2002).

### 3.1.1 Representação Digital do Som

O processo de digitalização ocorre em dois estágios:
1. **Amostragem** — mede a amplitude do sinal em instantes regulares
2. **Quantização** — mapeia as medições para valores discretos (ROSSING, 2002)

Essa sequência de valores numéricos serve de base para todas as operações subsequentes.

### 3.1.2 Síntese Sonora

Gera sons por meio de modelos computacionais, sem gravações acústicas:

| Tipo | Descrição |
|------|-----------|
| **Aditiva** | Ondas senoidais puras somadas para formar timbres |
| **Subtrativa** | Sinais ricos em harmônicos filtrados para moldar o espectro |
| **FM (Modulação de Frequência)** | Frequência de uma portadora alterada dinamicamente por uma moduladora — timbres expressivos |
| **Granular** | Pequenos fragmentos de áudio ("grãos") para criar texturas sonoras singulares (MANNING, 2013) |

### 3.1.3 Processamento de Áudio Digital

Manipula gravações pré-existentes via filtragem, equalização, compressão, reverberação e efeitos especiais (ZÖLZER, 2011). Amplamente empregado em DAWs.

### 3.1.4 Aplicações no Contexto do Aerochord

- Captação e interpretação de gestos corporais mapeados para mensagens MIDI coerentes
- A quantização assegura precisão na conversão de sinais de movimento
- A síntese e o processamento digital compõem o ambiente sonoro
- O hiperinstrumento torna o corpo do intérprete parte ativa da criação musical

---

## 3.2 O Protocolo MIDI (Musical Instrument Digital Interface)

### 3.2.1 Fundamentos e Limitações do MIDI 1.0

**O que é:** Sistema de transmissão de "instruções musicais" (não áudio) entre controladores, sintetizadores e sequenciadores, estabelecido como padrão desde 1983 (AVANZINI; FASCHI; LUDOVICO, 2023).

**Mensagens principais:** Note On/Off, Velocity, Control Change, Program Change, Pitch Bend, System Exclusive

**Estrutura de mensagem:** byte de status (tipo + canal) + 1 ou 2 bytes de dados (7 bits significativos cada)

**Taxa de transferência:** 31,25 kbps (serial DIN MIDI)

#### Limitações Críticas para o Aerochord

| Limitação | Impacto |
|-----------|---------|
| Resolução de 7 bits (128 passos) em Control Changes e Velocity | Saltos audíveis em variações contínuas |
| Pitch Bend afeta todas as notas do canal simultaneamente | Impossibilita modulações individuais em acordes polifônicos |
| Comunicação primordialmente unidirecional | Sem confirmação de recepção ou negociação de capacidades |
| Apenas 16 canais por porta | Limita fluxos de controle simultâneos |
| System Exclusive pouco padronizado | Dificulta trocas genéricas de parâmetros |
| Ausência de timestamps embutidos | Impossibilita compensação automática de jitter |

---

### 3.2.2 Arquitetura Técnica do MIDI 2.0

O MIDI 2.0 mantém **compatibilidade retroativa** com MIDI 1.0 e introduz comunicação **bidirecional**.

#### Universal MIDI Packet (UMP)

Formato central do MIDI 2.0:
- Pacotes de **32, 64 ou 128 bits**
- Campos: tipo de mensagem, grupo, canal/opcode e timestamp embutido (opcional)
- Suporta dados de alta resolução (16+ bits) e per-note controllers sem fragmentação

#### MIDI Capability Inquiry (MIDI-CI)

Mecanismo de **handshake inteligente**:
1. Dispositivo envia mensagem de Discovery
2. Receptor responde com suporte a MIDI 2.0, perfis e propriedades configuráveis
3. O Aerochord identifica automaticamente capacidades do receptor — dispensa configuração manual

#### Perfis (Profiles) e Property Exchange

- **Profiles:** conjuntos padronizados de regras para contextos específicos (ex: mapeamentos de alta resolução para filtros)
- **Property Exchange:** consulta e ajuste dinâmico de parâmetros do dispositivo remoto em tempo de execução

#### Flexibilidade de Transporte

O MIDI 2.0 suporta múltiplos transportes:
- USB-MIDI 2.0 nativo
- BLE MIDI
- RTP-MIDI em rede
- Web MIDI
- Fallback para MIDI 1.0 via SysEx em conexões legadas

#### Timestamps e Grupos

- Até **16 grupos independentes**, cada um com 16 canais
- Ex. no Aerochord: gestos da mão esquerda e direita alocados em grupos distintos — sem colisão de comandos

---

### 3.2.3 Novas Capacidades Expressivas do MIDI 2.0

#### Alta Resolução de Parâmetros

| Parâmetro | MIDI 1.0 | MIDI 2.0 |
|-----------|----------|----------|
| Velocity | 7 bits (128 passos) | 16 bits |
| Control Changes | 7 bits | 32 bits |
| Pitch Bend | Canal inteiro | Per-note, 32 bits |

Resultado: variações suaves e graduais sem artefatos de resolução limitada.

#### Per-Note Controllers

Permite enviar modulação, aftertouch ou pitch bend para **notas específicas dentro de um acorde**. O Aerochord pode aplicar vibrato ou detune em uma nota sustentada enquanto outras permanecem estáveis.

#### Articulação e Metadados de Expressão

Mensagens que descrevem detalhes de ataque, duração expressiva e variações de timbre. Permite comunicar transições entre staccato e legato e intensidades de ataque.

#### Transporte de Dados Estruturados

System Exclusive 8-bit, Flex Data e Mixed Data Sets permitem salvar e transferir:
- Configurações de sensibilidade gestual
- Curvas de resposta
- Layouts de mapeamento

#### Compatibilidade com MPE

O MIDI 2.0 incorpora nativamente o MPE (MIDI Polyphonic Expression), garantindo interoperabilidade com controladores MPE existentes.

---

### 3.2.4 Persistência de Performances e Arquivos

**MIDI Clip File (baseado em UMP):** gravação nativa de sequências com mensagens de alta resolução, per-note controllers e timestamps.

**Container File:** agrupa múltiplos clips, metadados e mídias associadas em um pacote coeso de sessão.

**No Aerochord:** ao iniciar uma performance, o sistema armazena eventos UMP + informações de mapeamento ativo + configurações calibradas via Property Exchange. O usuário pode exportar para DAWs compatíveis com MIDI 2.0.

---

### 3.2.5 Justificativa Final para a Arquitetura do Aerochord

O suporte a MIDI 2.0 **não é "nice-to-have"** — é elemento central. Exemplos diretos de aplicação:

- **Filtro contínuo:** distância entre mãos controla cutoff sem saltos audíveis (resolução estendida)
- **Vibrato por nota:** gestos oscilatórios em notas específicas via per-note controller
- **Calibração dinâmica:** curvas ajustadas em tempo real via Property Exchange
- **Autoconfiguração:** o Aerochord identifica capacidades do receptor ao conectar

---

## 3.3 Teoria Musical Aplicada

### 3.3.1 Escala Cromática e Oitavas

**Escala cromática:** divisão da oitava em 12 semitons iguais (sistema temperado igual). Cada semitom = aumento fixo de frequência.

**Representação MIDI:** cada nota = número inteiro de 0 a 127 (semitons sucessivos). C central = C4 = MIDI 60.

**Oitavas:** 12 semitons. Relação 2:1 de frequência (ex: A3 = 220 Hz → A4 = 440 Hz). Percebida como a mesma "identidade sonora" em registro mais agudo.

**No Aerochord:**
- Altura vertical da mão esquerda → seleciona a oitava ativa
- Dentro da oitava, a altura da mão direita → define qual nota será tocada
- Permite transposições rápidas sem perda da referência tonal

---

### 3.3.2 Articulações

#### Staccato

- Notas executadas de forma **breve e destacada**, com interrupções claras
- MIDI: curto intervalo entre Note On e Note Off
- Gesto: fechamento súbito da mão seguido de liberação imediata → notas curtas e rítmicas (DUCKWORTH, 2007)

#### Legato

- Execução **fluida e contínua**, notas ligadas sem interrupções
- MIDI: Note On da nova nota antes do Note Off da anterior → sobreposição suave
- Gesto: movimentos suaves, contínuos ou sustentados da mão (BENWARD; SAKER, 2009)

**Requisitos para o sistema:**
- Reconhecer velocidade do gesto, tempo entre ações e suavidade do movimento
- Thresholds e tolerâncias temporais ajustáveis para evitar staccato ininteligível ou legato com sobreposição indesejada

---

### 3.3.3 Efeitos de Expressividade

#### Vibrato

- Modulação periódica e contínua na altura da nota (frequência)
- **MIDI 2.0:** per-note controllers dedicados para taxa, profundidade e início do efeito — resolução de 32 bits, negociados via MIDI-CI
- **No Aerochord:** pequenos movimentos oscilatórios da mão → taxa e amplitude mapeadas em tempo real

#### Pitch Bend

- Alteração contínua da afinação de uma nota
- **MIDI 2.0:** Per-Note Pitch Bend — campo de 32 bits em UMP, sem conflitos entre notas simultâneas
- **No Aerochord:** deslocamentos suaves da mão → transições melódicas fluidas de alta sensibilidade

#### Sustain

- Manutenção do som após o término do gesto
- **MIDI 2.0:** Control Change de alta resolução (32 bits) em UMP — simula "meio-pedal" (half-pedaling)
- **No Aerochord:** mão aberta mantida ou gesto específico → aciona/desativa sustain

#### Dinâmica

- Intensidade sonora de uma nota ou frase (piano, forte, crescendo…)
- **MIDI 2.0:** velocity de alta resolução (16 ou 32 bits) na mensagem Note On em UMP; Control Changes de 32 bits para modulação contínua da intensidade, com timestamps embutidos
- **No Aerochord:** velocidade do movimento → velocity de alta precisão; abertura da mão → intensidade contínua

---

## 3.4 Considerações Sobre o Capítulo

Os fundamentos estabelecidos neste capítulo formam o alicerce do Aerochord:

- **MIDI 1.0** unificou o ecossistema, mas apresenta limitações críticas de resolução e expressividade para instrumentos gestuais
- **MIDI 2.0** reverte esses obstáculos via UMP, MIDI-CI e mensagens de alta resolução
- A capacidade de **endereçar 16 a 32 bits**, controlar cada nota individualmente e negociar perfis em tempo real viabiliza o mapeamento fluido de gestos em parâmetros musicais ricos
- Os conceitos de **teoria musical** (escala cromática, articulações, efeitos) fundamentam o design de mapeamento gesto-som descrito no Capítulo 5
