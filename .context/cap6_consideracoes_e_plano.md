# Capítulo 6 — Considerações Finais e Plano de Trabalho

## Visão Geral do Capítulo

Este capítulo consolida as contribuições do TCC 1 e apresenta o planejamento detalhado para o TCC 2:
- **6.1** — Considerações finais do TCC 1 (problema identificado, proposta, contribuições)
- **6.2** — Resultados esperados para o TCC 2 (3 artefatos principais)
- **6.3** — Plano de atividades para o TCC 2 (6 fases com duração em semanas)
- **6.4** — Cronograma de execução proposto (Tabela 3 — 4 meses / 16 semanas)

---

## 6.1 Considerações Finais do TCC 1

### Problema Identificado

Barreiras de acesso à expressão musical, especialmente em interfaces gestuais de baixo custo e alta expressividade. A análise revelou uma **lacuna** no estado da arte: nenhuma solução existente equilibra simultaneamente:
- Alta acessibilidade
- Baixa latência
- Alta expressividade

### Proposta: Aerochord

Hiperinstrumento digital orientado a gestos que utiliza:
- Câmera RGB comum (sem hardware especializado)
- Software C++/JUCE para captura, mapeamento e geração de eventos MIDI 2.0 de alta resolução

### Contribuições Entregues no TCC 1

| Artefato | Descrição |
|----------|-----------|
| Revisão do referencial teórico | Revisão extensiva de interfaces gestuais musicais, MIDI, computação musical e IHC |
| Análise do estado da arte | Identificação clara das limitações das abordagens existentes (Capítulo 4) |
| Requisitos | Definição de requisitos funcionais e não funcionais (Capítulo 5.1) |
| Arquitetura modular | Detalhamento da arquitetura para implementação futura (Capítulo 5.2) |
| Mapeamento gesto–som | Proposição de mapeamentos coerentes com necessidades de usabilidade (Tabela 2) |

Esses artefatos formam a **base sólida para o desenvolvimento e avaliação empiricamente orientada no TCC 2**.

---

## 6.2 Resultados Esperados para o TCC 2

### Artefato 1: Protótipo de Software

Protótipo funcional do Aerochord em C++/JUCE realizando o pipeline completo:
- Captura de vídeo
- Detecção de pose via MediaPipe
- Análise de gestos
- Mapeamento gesto–som
- Geração de mensagens MIDI 2.0 (com fallback para MIDI 1.0)

Conforme a arquitetura do Capítulo 5.2.

---

### Artefato 2: Validação Experimental

Protocolo formal de avaliação com usuários-alvo incluindo:

**Métricas Objetivas:**
- Latência end-to-end medida via timestamps em UMP
- Taxa de reconhecimento de gestos
- Estabilidade do rastreamento

**Métricas Subjetivas:**
- **SUS** (System Usability Scale) — usabilidade inicial
- **NASA-TLX** — carga cognitiva
- **Entrevistas semiestruturadas** — feedback qualitativo

Os resultados devem embasar refinamentos no mapeamento e ajustes de parâmetros sensíveis.

---

### Artefato 3: Monografia Final

Redação dos capítulos restantes da dissertação:
- Metodologia de Desenvolvimento
- Descrição de Implementação
- Resultados da Avaliação
- Discussão dos Resultados
- Conclusões
- Trabalhos Futuros

---

## 6.3 Plano de Atividades para o TCC 2

### Fase 1 — Desenvolvimento do Núcleo do Sistema (Semanas 1–5)

**Objetivo:** Implementar os módulos básicos e validar as interfaces internas

**Entregas:**
- Módulo de captura de vídeo (abstração multiplataforma)
- Integração do MediaPipe para extração de landmarks
- Módulo de geração de mensagens MIDI 2.0 em C++/JUCE
  - Configuração de handshake MIDI-CI
  - Fallback para MIDI 1.0
- Protótipos iniciais de envio de pacotes UMP

---

### Fase 2 — Implementação da Lógica de Interação (Semanas 4–7)

> Inicia na semana 4 (overlap com Fase 1)

**Objetivo:** Desenvolver e integrar a lógica de reconhecimento e mapeamento de gestos

**Entregas:**
- Módulo de análise de gestos (Máquina de Estados Finitos)
  - Reconhecimento de: pinça, movimentos de seleção de oitava, controles contínuos
- Módulo de mapeamento gesto–som (conforme Tabela 2 e Capítulo 5.2.1)
- Mecanismos de minimização de inconsistências
- Filtros de suavização nos landmarks para mitigar falsos positivos

---

### Fase 3 — Integração e Testes Iniciais (Semanas 8–10)

**Objetivo:** Integrar todos os módulos e verificar o pipeline completo

**Atividades:**
- Integração de todos os módulos com comunicação por filas lock-free
- Testes funcionais do pipeline completo (sem travamentos ou atrasos perceptíveis)
- Ajustes iniciais de parâmetros com base em medições de profiling:
  - Tamanhos de buffers
  - Prioridades de threads
  - Thresholds de confiança

---

### Fase 4 — Execução da Avaliação com Usuários (Semanas 11–12)

**Objetivo:** Coletar dados empíricos com usuários representativos

**Participantes:** Músicos sem treinamento formal + pessoas com mobilidade reduzida

**Protocolo:** Conforme metodologia do Capítulo 4.3

**Dados coletados:**
- Objetivos: latência, precisão de gestos (logs de eventos gestuais e mensagens MIDI)
- Subjetivos: SUS, NASA-TLX, entrevistas semiestruturadas

---

### Fase 5 — Análise dos Dados e Escrita dos Resultados (Semanas 13–14)

**Objetivo:** Analisar dados e redigir capítulo de resultados

**Atividades:**
- Análise estatística e qualitativa dos dados coletados
- Comparação de métricas com metas estabelecidas (ex: latência < 30 ms)
- Discussão em relação ao referencial teórico e expectativas de expressividade/usabilidade
- Redação do capítulo de Resultados e Discussão
- Destaque de aprendizados e ajustes de mapeamento derivados da avaliação

---

### Fase 6 — Redação Final e Preparação para Defesa (Semanas 15–16)

**Objetivo:** Finalizar a monografia e preparar a defesa

**Atividades:**
- Revisão completa da monografia
- Incorporação de feedback do orientador
- Finalização de diagramas e tabelas
- Elaboração de apresentação para a banca
- Verificação de formatação e consistência de citações
- Planejamento da defesa

---

## 6.4 Cronograma de Execução Proposto

### Tabela 3 — Cronograma Simplificado de Atividades do TCC 2

| Atividade / Fase | Mês 1 | Mês 2 | Mês 3 | Mês 4 |
|-----------------|:-----:|:-----:|:-----:|:-----:|
| Fase 1: Desenvolvimento do Núcleo do Sistema | ● | ● | | |
| Fase 2: Implementação da Lógica de Interação | | ● | ● | |
| Fase 3: Integração e Testes Iniciais | | | ● | |
| Fase 4: Execução da Avaliação com Usuários | | | ● | ● |
| Fase 5: Análise dos Dados e Escrita dos Resultados | | | | ● |
| Fase 6: Redação Final e Preparação para Defesa | | | | ● |

**Sobreposições planejadas:**
- Fases 1 e 2 se sobrepõem (semanas 4–5) — desenvolvimento paralelo
- Fases 3 e 4 se sobrepõem no Mês 3 — integração e avaliação em sequência rápida
- Fases 5 e 6 concentradas no Mês 4 — escrita e preparação para defesa

O cronograma demonstra compatibilidade com o semestre disponível e inclui margens para ajustes e imprevistos.

---

## Síntese das Contribuições e Próximos Passos

### O que o TCC 1 entregou

Uma proposta de engenharia **completa e bem fundamentada** — não apenas a ideia do Aerochord, mas toda a base técnica, teórica e metodológica para sua construção.

### O que o TCC 2 deve produzir

| Entregável | Evidência de Sucesso |
|-----------|---------------------|
| Protótipo funcional | Pipeline completo operando sem erros |
| Validação experimental | Latência < 30 ms medida empiricamente; SUS e NASA-TLX dentro de metas |
| Monografia final | Documento coeso cobrindo toda a jornada do projeto |

### Missão do Aerochord

Democratizar a musicalidade mediante controle gestual de alta precisão — permitindo que qualquer pessoa, independentemente de proficiência em instrumento ou equipamento prévio, explore gestos finos com resposta sonora refinada.
