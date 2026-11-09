# Decisões e verificações de hardware

Complementa o `SPEC.md` (que é a referência funcional). Cada sessão de fase lê este ficheiro e atualiza-o ao terminar. Estado de cada ponto: **Decidido** (pelo utilizador), **Verificado** (na documentação) ou **Por confirmar** (testar em hardware ou perguntar).

## Ferramentas — Decidido
- Arduino IDE com core ESP32, placa `M5AtomS3`.
- Bibliotecas: `M5Unified` (+ `M5GFX`), `M5Unit-TOF` ([repo](https://github.com/m5stack/M5Unit-TOF), depende de `M5UnitUnified`, `M5Utility` e `M5HAL`; substitui a `M5Unit-ToF4M`) e `M5Unit-AudioPlayer` ([repo](https://github.com/m5stack/M5Unit-AudioPlayer); **não usada desde a Fase 2**, ver abaixo).
- Comentários no código em inglês.

## Pinos — Verificado (imagem do pinout da ABC Port para AtomS3)
| Porta | Função | Fio amarelo | Fio branco | Ligado a |
|---|---|---|---|---|
| P.A | I2C | G38 (SDA) | G39 (SCL) | ToF Mini ToF-90 (Mestre) |
| P.B | GPIO | G7 | G8 | livre |
| P.C | UART | G5 | G6 | AudioPlayer (Mestre) |

- O Remoto liga o ToF à porta Grove da própria placa (G2 e G1), sem ABC Port.
- Botão do AtomS3 Lite: GPIO41. LED RGB: GPIO35. Vêm de fontes secundárias ([ESPHome](https://devices.esphome.io/devices/M5Stack-AtomS3-Lite/)): **Por confirmar**. A `M5Unified` abstrai ambos.

## Sensor ToF — Decidido: Unit Mini ToF-90 (U196, VL53L0X), nos dois nós — Verificado ([docs](https://docs.m5stack.com/en/unit/Unit_Mini_ToF-90))
- Substitui o U172 (Unit ToF4M, VL53L1CX), por escolha do utilizador.
- I2C 0x29, alcance 3 a 200 cm. **Em condições normais o máximo é 120 cm**; 200 cm só em Long Range, no escuro e com pouco infravermelho. Precisão ±3 %, campo de visão 25°, período 23 ms (mínimo 8 ms).
- Fios do Grove: amarelo = SDA, branco = SCL (Mestre G38/G39; Remoto G2/G1).
- Sem modos curto/médio/longo nem ROI: usa-se o perfil por defeito da biblioteca.
- Sem `range_status` no código da Fase 1; leituras negativas contam como "sem alvo".
- Exemplo oficial usa `M5.Ex_I2C`/helper `wiring::addI2C`; o sketch usa `Wire.begin(SDA, SCL)` com pinos explícitos e `Units.add(unit, Wire)`.

## Limiar de deteção — Decidido
- limiar = largura do corredor − margem.
- Margem: **30 cm** (valor de teste, parâmetro de firmware, a rever no corredor real).
- Largura por defeito: 80 cm (SPEC), limitado a 120 cm (alcance normal do VL53L0X) no firmware.
- Passagem = transição "livre → ocupado" (distância abaixo do limiar), com histerese e tempo de rearme para que uma pessoa parada conte como uma só.

## AudioPlayer (U197, N9301) — Verificado ([docs](https://docs.m5stack.com/en/unit/Unit_AudioPlayer), [protocolo UART](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1155/Unit-AudioPlayer-Protocol-EN.pdf))
- UART 9600 8N1. Esperar 5 s após ligar. Mais de 500 ms entre comandos (recomendação do documento).
- Volume 0 a 30 (por defeito o máximo). Existe comando de stop (`04 FB 01 14`) e modo single loop (`0B F4 02 01 01 03`).
- microSD FAT16/FAT32, até 32 GB, MP3/WAV. O cartão tem de estar inserido antes de ligar. O índice da faixa segue a ordem de gravação no cartão.
- Erros devolvidos por UART (drive não encontrado, erro de ficheiro).
- **Verificado em hardware (Fase 2):** G6 = RX e G5 = TX do ESP32 (só se testou esta ordem, funcionou); comandos de volume a 250 ms e a 100 ms funcionam (ver Fase 2); a biblioteca expõe `stopAudio()` mas envia `04 FB 01 03` (não o `14`), por isso usam-se os bytes diretos.

## Fase 1 — Parâmetros — Decidido (valores a validar em bancada)
- Perfil por defeito do sensor (sem Long Range), leitura contínua da biblioteca.
- Histerese 10 cm; ocupado após 2 leituras consecutivas abaixo do limiar; rearme após 500 ms contínuos de "livre".
- Limiar = 80 − 30 = 50 cm. Constante `NODE_MASTER`/`NODE_REMOTE` no sketch escolhe os pinos I2C (autorizado).
- Sketch: `Fase1_Sensor/Fase1_Sensor.ino` (compila para `esp32:esp32:m5stack_atoms3`; **não testado em hardware**).
- **Verificado em hardware (Mestre, bancada, mão à frente do ToF):** `unit.range()` está em mm; sem alvo devolve sempre `-1` (nunca um valor grande); cadência ~30 ms; mão parada ~14 a 22 s conta uma só passagem; passagens a ~1 s de intervalo contam todas; mínimo lido 73 mm.
- **Sequência guiada (`#define TEST_SEQUENCE`, desligado por defeito), Mestre em bancada:**
  - Alvo à volta de 50 cm: sem oscilação, a histerese segura o estado `OCCUPIED` até >60 cm (o teste começou com a mão já presente, por isso 0 passagens novas).
  - Roupa escura a ~30 cm: leituras válidas (181 a 313 mm) durante ~13 s, uma só passagem; 38 % das leituras `-1` no teste todo.
  - Luz forte: 68 % das leituras `-1`; 6 passagens contadas para 3 passagens da mão, por isso há **contagem a mais** (o `-1` a meio de uma passagem durante >500 ms faz `OCCUPIED -> FREE` e a seguinte leitura conta de novo). Não se distingue a mão fora de alcance de falha do sensor, porque no teste só se imprime uma leitura a cada 500 ms.
  - Passagens rápidas (~0,3 s): 7 passagens para 3 repetições de 2.
- **Decidido:** a luz ambiente do corredor será fraca; o resultado com luz forte não bloqueia a Fase 1 e os valores (histerese 10 cm, 2 leituras, rearme 500 ms) ficam como estão.
- **Por confirmar (Fase 6, no corredor real):** efeito da luz do corredor nas leituras `-1`, ordem SDA/SCL no Remoto (G2/G1).

## Fase 2 — Áudio
- **Decidido:** bytes diretos do protocolo na `Serial1` a 9600 (sem a biblioteca `M5Unit-AudioPlayer`, que bloqueia 100 ms em `setVolume`, usa o stop `03` e tem debug ligado). `M5Unit-AudioPlayer` deixa de ser necessária neste projeto. Comandos Serial de teste (`p s v o i fo fi st q x`) autorizados só para a Fase 2.
- Sketch: `Fase2_Audio/Fase2_Audio.ino` (compila para `esp32:esp32:m5stack_atoms3`). Pinos: RX=G6, TX=G5. Espera 5 s, depois envia volume, single loop `0B F4 02 01 01 03` e consulta de dispositivo `04 FB 01 08 08`; o cartão em falta é detetado por resposta sem SD (código diferente de 2/3/6), erro `AA 55 02 FF 02/03` ou timeout de 1 s.
- Valores de teste (não são valores finais): volume 20; fade-out e fade-in 5000 ms e depois 2000 ms; intervalo mínimo entre degraus 500, 250 e 100 ms.
- **Verificado em hardware (Mestre, bancada, jack):**
  - Arranque com cartão: `AUDIO READY`. Faixa 1 toca em loop (a unit envia `04 FB 01 03 03` e `04 FB 03 0E 00 01` a cada repetição).
  - Fade-out e fade-in a 250 ms (20 degraus de 1 nível, 5000 ms) e a 100 ms (20 degraus, 2000 ms): sem erros nem comandos perdidos no registo; o utilizador diz que funcionam bem. O limite de 500 ms do documento não se aplica ao comando de volume.
  - No fim do fade-out: stop `04 FB 01 14 14` e volume reposto; o `p` seguinte toca do início. O stop `03` (comando `x`) **não foi testado**.
  - No arranque a unit envia `BB 44 01 02 02` (cartão inserido) e `AA BB 65` (trama fora do protocolo, sem efeito).
  - Arranque sem cartão (testado em hardware): a unit responde a cada comando com `AA 55 02 FF 03 03` (sem drive disponível) e o sketch indica `CARD ERROR: drive not found`. Com o cartão reposto e a placa religada, volta `AUDIO READY`. A recusa do `p` com `ERR audio not ready` em estado de erro **não foi vista** no registo.
  - Stop `03` (`04 FB 01 03 03`, comando `x`): a unit responde `04 FB 02 00 00 01` (estado 0, parado) e o `p` seguinte volta a tocar a faixa 1 do início. O stop `14` não devolve resposta. Ambos funcionam; o projeto usa o `14` por defeito (sem resposta, não é preciso tratá-la), e o `03` fica como alternativa que confirma o estado.
- Os valores de teste acima foram substituídos pelas decisões da secção "Máquina de estados e parâmetros" (fade-out ajustável com 3 s por defeito, fade-in fixo e mínimo).
- **Por confirmar:** se o fade 100 ms é audivelmente mais suave que 250 ms (o utilizador achou ambos bons); menor intervalo entre degraus de volume sem comandos perdidos abaixo de 100 ms (Fase 3).

## Máquina de estados e parâmetros — Decidido (para a Fase 3)
- Em "A tocar", uma passagem na entrada não faz nada: o áudio continua a tocar.
- Durante o fade-out, uma passagem na saída não faz nada: o fade-out continua. Só uma passagem na entrada cancela o fade-out.
- Depois de cancelar o fade-out (fade-in concluído), o sistema volta a "A tocar" e espera uma nova passagem na saída para recomeçar a janela de silêncio.
- **Fade-in:** fixo (não ajustável) e o mais curto que o intervalo entre degraus de volume permitir. Parte do volume atual até ao volume configurado, com um tempo fixo por nível (duração proporcional ao salto, no máximo 30 degraus). A Fase 3 mede o menor intervalo sem comandos perdidos, testando 100, 50, 33 e 20 ms (só 100 ms foi testado na Fase 2), e fixa esse valor no firmware. O `SPEC.md` foi atualizado em conformidade.
- **Espera máxima:** reinicia sempre que o sistema sai de "A aguardar" (para Pronto ou para "A tocar" depois de um fade-out cancelado). A primeira deteção na saída de cada ciclo volta a abri-la. Razão: contá-la desde a primeira saída de sempre faria o fade-out arrancar de imediato à saída seguinte, sem respeitar a janela de silêncio.
- **Parâmetros ajustáveis na webapp** (guardados em NVS):

| Parâmetro | Defeito | Limites |
|---|---|---|
| Volume | 20 | 1 a 30 |
| Fade-out | 3 s | 0,5 a 10 s |
| Janela de silêncio | 10 s | 1 a 120 s |
| Espera máxima | 60 s | ≥ janela de silêncio, até 600 s |
| Largura do corredor | 80 cm | máximo 120 cm; mínimo por definir (tem de ser > 30 cm, a margem) |

## Fase 3 — Lógica — Decidido e testado em hardware (Mestre, bancada)
- **Verificado em hardware** (`sw 3`, `mw 10`, `fo 2000`, volume 20): entrada toca sem esperar (só o comando de play); entrada em "A tocar" ignorada; saída em Pronto, durante o fade-out e durante o Rearme ignoradas; saída abre a janela (3,000 s exatos); passagens em "A aguardar" reiniciam a janela; espera máxima aos 10,000 s da primeira saída, mesmo com passagens a cada ~2 s; fade-out de 20 degraus em 2000 ms, stop `14`, volume reposto, Rearme ~1,2 s; entrada durante o fade-out cancela e faz fade-in (visto a 1 e a 8 de volume; a 8→20: 12 degraus de 50 ms, 600 ms) e a espera máxima reinicia (o fade-out seguinte só arranca 3 s depois da saída); entrada durante o Rearme fica na fila (stop, volume, play pela ordem); mão parada ~30 s conta uma só passagem; parâmetros persistem após desligar e os valores fora dos limites são recusados; arranque sem cartão recusa a entrada com erro e mantém Pronto.
- **Não testado:** efeito do `cw` na deteção real (só se viu o limiar passar a 40 cm no `?`); `ens` a 33 e 20 ms.
- Sketch: `Fase3_Logica/Fase3_Logica.ino` (compila para `esp32:esp32:m5stack_atoms3`).
- Saída do Remoto simulada por clique no botão ou comando `x`.
- Fade-in em curso: a lógica já está em "A tocar"; uma saída abre a janela e o fade-in termina na mesma; uma entrada é ignorada.
- Entrada durante o Rearme: aceite, o play fica na fila atrás do stop e da reposição de volume. Entrada nos 5 s de arranque da unit: recusada com erro no registo. Saída durante o Rearme: ignorada.
- O play já não envia o volume antes (o volume é enviado no arranque, após cada stop e quando muda). `CMD_GAP_MS` mantém-se em 600 ms.
- Limites: largura mínima 40 cm; `sw` maior que `mw` é recusado com erro, sem alterar o outro valor.
- Comandos Serial autorizados (só testes, a migrar para a webapp na Fase 5): `e x v fo sw mw cw d ? ens N [v]`.
- **Fade-in fixo: 50 ms por nível** (`FADE_IN_STEP_MS`; 20 níveis = 1 s). Decidido com o utilizador depois do ensaio `ens` em hardware: 100 e 50 ms sem erros da unit e com volume final correto, na rampa simples e na variante com verificação (os 60 degraus aparecem em sequência, sem nenhum salto). **33 e 20 ms não foram testados**, ficam como margem. O mesmo valor é o intervalo mínimo entre degraus do fade-out (`FADE_OUT_STEP_MS`).
- Ensaio `ens N v`: as respostas da unit vêm desfasadas de uma posição (a resposta k indica o nível do degrau k+1; 58 respostas em 60 consultas, igual a 100 e 50 ms), por isso o resultado sai `ANOMALIES` mesmo sem comandos perdidos. Critério usado: lista contínua sem saltos de nível. Causa do desfasamento **não explicada**.
- Troca de cartão a quente (Mestre ligado): a unit envia `BB 44 01 03 03` (removido) e `BB 44 01 02 02` (inserido) e continua a tocar com o novo cartão; mas, se arrancou sem cartão, repor o cartão **não** recupera o áudio sem reiniciar (o cartão só é verificado no arranque). Não pedido no SPEC, fica para decidir na Fase 5 ou 6.
- **Failsafe do Remoto offline (dúvida 6): autorizado** como "arrancar o fade-out após um tempo" quando o Remoto está offline. Implementa-se na Fase 4 (precisa de deteção de ligação). **Por definir na Fase 4:** que tempo (sugestão: a espera máxima, contada desde que o Remoto ficou offline) e se vale também em "A aguardar".
- `SPEC.md`: referência "ToF U172" atualizada para "Unit Mini ToF-90 (U196)".

## Fase 4 — Remoto — Decidido e testado em hardware (Mestre + Remoto, bancada)
- Sketches: `Fase4_Mestre/Fase4_Mestre.ino` (Fase 3 + ESP-NOW) e `Fase4_Remoto/Fase4_Remoto.ino`, cada um com cópia **idêntica** de `Protocol.h` (verificar com `diff`). Compilam para `esp32:esp32:m5stack_atoms3` (core 3.3.11).
- **Protocolo** ([ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_now.html): `SEND_SUCCESS` só confirma o nível MAC): unicast, stop-and-wait. `EXIT_EVENT` (bootId u32 aleatório por arranque, seq u16, ageMs u16), `EVENT_ACK`, `HEARTBEAT` (1 s, flag ToF sem dados). Reenvios do mesmo seq a 40/80/160/320 ms e depois de 500 em 500 ms, sem limite; fila do Remoto com 16 eventos (cheia: descarta o mais antigo e conta). Mestre: bootId novo aceita; seq novo executa; seq repetido só reenvia o ACK; o ACK só sai depois de o evento entrar na fila da lógica. Evento perdido se o Remoto perder a alimentação com um evento por entregar (só em RAM).
- **Hora do evento:** o Mestre usa `receção − ageMs`; em "A aguardar" a janela nunca recua.
- **Emparelhamento (dúvida 15):** MAC fixo (STA) em `Protocol.h`; cada nó ignora outros emissores. Mestre `AC:A7:04:00:35:4C`, Remoto `AC:A7:04:00:42:00` (lidos com `esptool`; já em `Protocol.h`).
- **Canal:** `ESPNOW_CHANNEL = 1` (constante única; há redes fortes no local, **por rever** com os canais delas). Poupança de energia Wi-Fi desligada. Os dois nós em modo STA: com `WIFI_AP_STA` sem `softAP()` o AP arrancava com configuração por defeito, contra o SPEC (AP desligado em modo Normal). Na Fase 5 o Mestre passa a `WIFI_AP_STA` ao ligar o AP, com o mesmo canal.
- **Cores:** comunicação numa tarefa FreeRTOS no core 0 (prioridade 3), lógica no `loop()` (core 1), ligadas por filas.
- **Ligação:** Remoto offline se não chegar nenhum pacote em 3 s. **Failsafe (dúvida 6):** só em "A tocar"; se o Remoto estiver offline durante a espera máxima (contada desde o mais tardio entre "ficou offline" e "entrou em A tocar"), arranca o fade-out. Em "A aguardar" não é preciso (a janela e a espera máxima já terminam o ciclo). Em Pronto a entrada toca na mesma.
- **Comandos Serial autorizados (só testes, a remover na Fase 5):** Remoto `? x burst N` (1 a 64); Mestre os da Fase 3 mais `drop N` e `noack N`.
- Reservados para a Fase 5, não implementados: mensagem de configuração Mestre→Remoto (largura do corredor; no Remoto fixa em 80 cm) e leitura em direto para a calibração.
- **Stop no arranque do Mestre (autorizado, acrescentado na Fase 4):** a unit de áudio é alimentada pelos 5 V do USB e continua a tocar quando o ESP reinicia (carregamento por USB, `esptool`, watchdog), por isso o Mestre ficava em Pronto com o áudio preso. `serviceBoot` envia agora o stop (`04 FB 01 14 14`) antes do volume e do loop. Verificado em hardware: arranque com a unit a tocar passa a silêncio.
- **Falha corrigida durante os testes:** o failsafe testava a ligação com `remoteOnline(now)` e podia ver "offline" um passo antes de `remoteOfflineSinceMs` ser atualizado, arrancando o fade-out de imediato. Passou a usar `remoteWasOnline`. Verificado no T5.
- **Verificado em hardware (Mestre `/dev/cu.usbmodem1101`, Remoto `/dev/cu.usbmodem101`; os números das portas trocam-se quando se volta a ligar os cabos):**
  - T1 passagem real no ToF do Remoto: ACK em 6 a 11 ms numa só transmissão; abre a janela de silêncio (2996 a 2998 ms), passagens seguidas reiniciam-na, fade-out de 2000 ms, stop, `READY`; mão parada conta uma só passagem.
  - T2 `burst 16`: 16 eventos, `seq` consecutivos, ordenados, sem duplicados nem buracos, ~100 ms no total. `burst 20`: 4 descartes na fila do Remoto, os 16 restantes entregues sem buracos (os descartados nunca recebem `seq`).
  - T3 `noack 3`: 4 transmissões (0, 40, 120 e 280 ms), executado **uma só vez**, 3 duplicados reconhecidos no 4.º.
  - T4 `drop 5`: retransmissões até chegar (`age` 122 ms), executado uma vez, hora real do evento usada na janela.
  - T5 Remoto offline em "A tocar": offline em 3 s, fade-out por failsafe exatamente 10 s depois (com `mw 10`), quer o Remoto se desligue durante o jogo quer já estivesse desligado antes da entrada (então conta desde a entrada). O Remoto que volta a tempo cancela-o (margem de só 36 ms nesse ensaio).
  - T6 reinício do Remoto (`bootId` novo, `seq` 1 executado, não tomado por duplicado) e reinício do Mestre com o Remoto ligado (volta online em ~2 s após corte de alimentação; ~5 s após carregamento por USB, não explicado).
  - O ToF do Remoto em G2/G1 funciona sem aviso de SDA/SCL trocados (ordem confirmada).
  - Passagem do Remoto durante o fade-out ignorada (regra da Fase 3).
- **Não testado / fica para a Fase 6 (corredor real, cotovelo, AP ligado):** perda de ligação por distância e entrega de eventos pendentes depois (T7: em bancada o Remoto nunca perdeu o sinal; o `offline` visto foi perda de alimentação); revisão do canal 1 face às redes fortes do local; T5d (offline em "A aguardar") dispensado por análise (o failsafe só existe em "A tocar" e os temporizadores normais fecham o ciclo).
- **Observações:** o ToF do Mestre devolveu `range 0 mm` uma vez (provavelmente mão encostada). O `?` do Mestre mostra agora o estado do Remoto (online, `bootId`, contadores).
- Remoto alimentado por adaptador USB (dúvida 12); a alimentação do Mestre continua por responder.

## Fase 5 — Webapp — Decidido e testado em bancada (Mestre + Remoto reais, telemóvel como cliente)
- Sketches: `Fase5_Mestre/` (`Fase5_Mestre.ino`, `WebPage.h`, `Protocol.h`) e `Fase5_Remoto/` (`Fase5_Remoto.ino`, `Protocol.h`). Os dois `Protocol.h` são idênticos (verificado com `diff`). Compilam para `esp32:esp32:m5stack_atoms3` (core 3.3.11): Mestre 92 % do programa (1208627 de 1310720 bytes, partição por defeito), Remoto 84 %. Precisa da biblioteca `ArduinoJson` 7.x (instalada em `~/Arduino/libraries`); a Fase 4 fica intacta em `Fase4_*`.
- **Dúvida 7 — decidido:** "Iniciar" só em Pronto e equivale a uma passagem de entrada (passa a "A tocar" e termina pelas regras normais, incluindo o failsafe). "Parar" faz stop imediato (`14`) com o volume reposto e passa por Rearme até Pronto; não existe em Pronto nem em Rearme.
- **Dúvida 14 — decidido:** SSID `EscapeRoom-Audio` (visível), WPA2, IP `192.168.4.1`, canal 1 (`ESPNOW_CHANNEL`), máximo de 4 clientes. A palavra-passe definitiva (escolhida pelo utilizador) está em `AP_PASSWORD` no `Fase5_Mestre.ino`.
- **Dúvida 16 — decidido (um só LED RGB, brilho baixo `LED_LEVEL` = 24/255; erros têm prioridade sobre o estado do jogo):** Pronto verde; A tocar azul; A aguardar ciano; Fade-out amarelo a piscar lento; Rearme amarelo; cartão em falta ou removido vermelho fixo; ToF da entrada sem dados vermelho a piscar rápido (também no erro de arranque, que pára o Mestre); Remoto offline laranja a piscar lento; ToF do Remoto com falha magenta a piscar lento. Com o AP ligado, por cima da cor atual: 1 pulso branco de 1 s em cada 3 s sem clientes, 2 pulsos de 250 ms com um cliente associado. Toque longo: 3 piscas brancos rápidos. Sem aviso no último minuto do AP. "Pulsar" implementado como ligar/desligar, sem rampa.
- **Dúvida 17 — decidido:** 200 entradas × 96 caracteres em RAM, **sem persistência** (perde-se ao reiniciar). Primeira entrada do arranque: motivo do reinício (`esp_reset_reason`). Horas em tempo desde o arranque (não há RTC).
- **Dúvida 18 — decidido:** polling, sem WebSocket. Estado a cada 1 s (mantém também o AP ativo); calibração a 4 Hz (250 ms) só com o separador visível, com mínimo e máximo desde o pedido anterior; registo incremental (`since=N`) a cada 2 s com o separador de debugging visível. Sem botão "usar a medição como largura".
- **Toque longo:** 2 s (`LONG_PRESS_MS`, `BtnA.wasHold()`); o clique curto não faz nada (deixa de simular a saída). Liga ou desliga o AP em qualquer estado do jogo.
- **Corte do AP: 10 min** (o SPEC dizia 15, foi atualizado) **sem pedidos HTTP** (o temporizador arranca quando o AP liga). Um telemóvel associado mas sem a página aberta não conta.
- **Arquitetura (core 0):** tarefa `comm` (ESP-NOW, prioridade 3) e tarefa `web` (AP e `WebServer`, prioridade 2, pilha 10 KB); core 1: `loop()`. A tarefa web nunca escreve nas variáveis do jogo: lê um instantâneo publicado pelo `loop()` a cada 100 ms e envia comandos (guardar parâmetros, iniciar e parar áudio) por uma fila. O AP e o `WiFi.mode` são geridos só na tarefa web. Ao ligar: `WIFI_AP_STA` + `softAP(..., canal 1)`; ao desligar: `WIFI_STA`; nas duas transições repõe-se o canal e verifica-se que o peer ESP-NOW continua registado (`ensurePeer`, `enforceChannel`). **Por confirmar em hardware: que o ESP-NOW sobrevive a estas trocas de modo.**
- **Página:** um só HTML em PROGMEM (`WebPage.h`), CSS e JS inline, sem recursos externos, em pt-PT, com 3 separadores (Ajustes, Calibração, Debugging). API JSON: `GET /api/status`, `/api/calib`, `/api/log?since=N`; `POST /api/params`, `/api/audio`.
- **Ajustes:** um só "Guardar"; o firmware valida o conjunto inteiro (restrição `janela ≤ espera máxima` independente da ordem de edição) e só guarda em NVS o que mudou. Seis parâmetros: volume, fade-out, janela, espera máxima, **largura da entrada (Mestre)** e **largura da saída (Remoto)**, ambas 40 a 120 cm (limites fixos no firmware, `WIDTH_MIN_CM`/`WIDTH_MAX_CM` em `Protocol.h`). Chave NVS nova: `cwr` (largura do Remoto).
- **Protocolo ESP-NOW (tipos novos, autorizados):** `4 CONFIG` (Mestre→Remoto, largura; `header.bootId` = `bootId` do Remoto), `6 CONFIG_ACK` (Remoto→Mestre, ecoa a largura em uso), `5 RANGE` (Remoto→Mestre, última/mínima/máxima leitura em mm e estado, a cada 200 ms), `7 STREAM_LEASE` (Mestre→Remoto, "envia leituras nos próximos 2,5 s"; renovada a cada 1 s enquanto o separador de calibração está a ser lido). O Mestre reenvia o `CONFIG` a cada 500 ms até o Remoto confirmar a largura **com o `bootId` atual**, por isso reenvia sozinho depois de um reinício do Remoto. O Remoto guarda também a largura na NVS (`cw`) para arrancar já com o limiar certo. O `RANGE` não tem ACK (uma perda é inofensiva).
- **Erros mostrados (webapp e LED):** cartão em falta (resposta sem SD ou erro da unit), cartão removido (evento `BB 44 01 03`; novo na Fase 5, só indicação, **não recupera o áudio**), ToF da entrada sem dados, Remoto offline, ToF do Remoto com falha, ESP-NOW não iniciado e (só na webapp) largura do Remoto por confirmar.
- **Comandos Serial:** removidos todos os de teste. Mestre fica só com `?`; Remoto fica só com `?`. O botão do Remoto não é usado.
- **Alterações de comportamento face à Fase 4 (só o que a Fase 5 exige):** a unit só é inicializada como antes; o `decodeFrame` deixou de registar respostas de volume/estado (já não há consultas); `failStop` do Mestre passou a piscar o LED a vermelho.
- **Não implementado (não pedido):** repor valores por defeito pela webapp (o `d` da Fase 3), recuperar o áudio ao repor o cartão, reiniciar pela webapp, RSSI da ligação, captive portal/mDNS, suspender o jogo durante a calibração (o jogo continua ativo: uma passagem na calibração ainda toca o áudio).
- **Verificado em hardware (passos guiados, Serial dos dois nós lido pelo assistente; LED, página e áudio confirmados pelo utilizador):**
  - Arranque: `Remote confirmed width 80 cm` aos 5 s, `AUDIO READY` aos 7 s, LED verde fixo (o `M5.Led` do AtomS3 Lite funciona; `LED_LEVEL` = 24 foi aceite).
  - Toque longo de 2 s: `AP ON` (SSID `EscapeRoom-Audio`, `192.168.4.1`, canal 1) e `AP OFF`; 3 piscas brancos e pulsos brancos do AP vistos.
  - **ESP-NOW com o AP ligado e depois de o desligar:** passagens do Remoto entregues com ACK em 5 a 7 ms numa só transmissão, janela de silêncio de 2996 ms, sem `REMOTE OFFLINE` nas trocas `STA -> AP_STA -> STA`. Num jogo de uso livre com o AP ligado: 22 entradas e 11 saídas (seq 1 a 11) sem `WARN`, `ERR`, duplicados nem lacunas; fade-out cancelado e fade-in funcionam.
  - Webapp no telemóvel: página abre, "ligado", erros, estado do jogo, ligação ao Remoto e registo de eventos corretos; calibração em direto nos dois cartões (distância, ocupada/livre, limiar 50 cm).
  - Ajustes: largura do Remoto 80 -> 70 -> 80 cm guardada e confirmada pelo Remoto em ~4 a 10 ms (`CONFIG width ...` no Remoto, `Remote confirmed width ...` no Mestre).
  - Iniciar e parar áudio (duas vezes): `PLAYING`, stop imediato, `REARM -> READY` em ~0,6 s; botões ativados e desativados conforme o estado.
  - Erros: cartão removido e reposto (`CARD removed`/`CARD inserted`, aviso e LED vermelho vistos e depois limpos); Remoto desligado e religado (`REMOTE OFFLINE`, `REMOTE ONLINE` e reenvio automático da largura ao novo `bootId`).
  - Corte do AP: `AP idle for 10 min: switching off` e `AP OFF` aos 616 s depois de ligar (os 16 s a mais vêm provavelmente de um pedido HTTP pouco depois de ligar, p. ex. o telemóvel a religar-se).
- **Não testado / fica para a Fase 6:** AP com vários clientes e em redor do cotovelo (alcance), arranque do Remoto sem Mestre ligado (cópia da largura na NVS do Remoto), fade com muita atividade da página aberta durante um jogo real, revisão do canal 1 face às redes do local.

## Dúvidas do plano por responder
6. ~~Failsafe para o Remoto offline~~ — respondida: autorizado, ver Fase 3 (implementação na Fase 4).
7. ~~"Iniciar e parar o áudio" na webapp altera o estado do jogo?~~ — respondida: sim, ver Fase 5.
10. Altura e posição dos ToF, e ficheiro de áudio no cartão (um só, MP3 ou WAV).
11. Amplificação e colunas na saída do jack.
12. Alimentação do Mestre e do Remoto. (Remoto: adaptador USB, decidido na Fase 4; Mestre por responder.)
14. ~~SSID, palavra-passe e IP do AP~~ — respondida, ver Fase 5.
15. ~~Emparelhamento do Remoto~~ — respondida: MAC fixo no firmware (ver Fase 4).
16. ~~Indicações do LED além do estado do AP~~ — respondida, ver Fase 5.
17. ~~Tamanho do registo de eventos e persistência~~ — respondida, ver Fase 5.
18. ~~Calibração em direto por polling ou WebSocket~~ — respondida: polling, ver Fase 5.

## Estado das fases
- [x] 1. Sensor (testado em bancada no Mestre; contagem a mais só com luz forte, não aplicável: luz ambiente fraca no corredor)
- [x] 2. Áudio (testado em bancada no Mestre; arranque sem cartão e os dois comandos de stop verificados)
- [x] 3. Lógica (testado em bancada no Mestre; saída do Remoto simulada por botão/Serial)
- [x] 4. Remoto (testado em bancada com Mestre e Remoto reais; alcance e cotovelo ficam para a Fase 6)
- [x] 5. Webapp (testada em bancada com Mestre e Remoto reais; palavra-passe do AP definida)
- [x] 6. Validação (testada com sucesso no corredor real, segundo o utilizador; sem medições registadas aqui; sem extras pedidos; instalação e alimentação tratadas pelo utilizador)
