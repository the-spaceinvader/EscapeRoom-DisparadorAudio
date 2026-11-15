# Manual de instalação — Dispositivo de áudio do corredor

Este manual é para quem monta, programa e põe o sistema em funcionamento: ligações, cartão de áudio, firmware e primeira configuração. O uso no dia a dia está no [manual de utilização](Manual_Utilizacao.md). A especificação funcional está no [`SPEC.md`](SPEC.md).

Não cobre a fixação mecânica nem a instalação elétrica no local.

## 1. Material

| Nó | Componentes |
|---|---|
| **Mestre** (entrada) | [AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite), [Atomic Port ABC Base](https://docs.m5stack.com/en/atom/AtomPortABC), [Unit Mini ToF-90 (U196)](https://docs.m5stack.com/en/unit/Unit_Mini_ToF-90), [Unit AudioPlayer (U197)](https://docs.m5stack.com/en/unit/Unit_AudioPlayer), cartão microSD |
| **Remoto** (saída) | AtomS3 Lite, Unit Mini ToF-90 (U196) |

Também precisa de:
- cabos Grove HY2.0-4P (um por unidade);
- alimentação USB-C de 5 V para cada nó, com um cabo USB-C **de dados** para programar;
- um sistema de amplificação ligado ao jack de 3,5 mm da unidade de áudio. A documentação da unidade não indica o nível nem a potência dessa saída;
- um computador com o Arduino IDE (ou o `arduino-cli`) e um telemóvel para a configuração.

## 2. Ligações

Os cabos Grove têm quatro fios: preto (GND), vermelho (5 V), amarelo e branco.

### 2.1 Mestre

Encaixe o AtomS3 Lite na ABC Port e ligue as unidades:

| Unidade | Porta da ABC Port | Fio amarelo | Fio branco |
|---|---|---|---|
| Mini ToF-90 | **P.A** (I2C) | G38 (SDA) | G39 (SCL) |
| AudioPlayer | **P.C** (UART) | G5 | G6 |
| — | P.B (GPIO) | G7 | G8 (livre) |

Na UART do AudioPlayer, o RX do ESP32 é o **G6** e o TX é o **G5**. Esta é a ordem verificada em hardware.

### 2.2 Remoto

Ligue o Mini ToF-90 à **porta Grove da própria placa**, sem ABC Port: fio amarelo em G2 (SDA) e fio branco em G1 (SCL).

## 3. Cartão de áudio

| Requisito | Valor |
|---|---|
| Tipo | microSD, **até 32 GB** |
| Sistema de ficheiros | **FAT16 ou FAT32** |
| Formato do áudio | **MP3 ou WAV** |

- O sistema toca a **primeira faixa** do cartão, em loop. A unidade ordena as faixas pela **ordem em que os ficheiros foram gravados**, e não pelo nome. Deixe só um ficheiro de áudio no cartão.
- O cartão tem de estar inserido **antes** de ligar o Mestre. Para o trocar, desligue o Mestre.
- Se arrancar sem cartão, o Mestre mostra o erro de cartão (LED vermelho fixo). Repor o cartão depois não recupera o áudio: é preciso desligar e voltar a ligar.

## 4. Sensores e largura do corredor

Cada sensor funciona como uma barreira: mede a distância em linha reta e, com o corredor vazio, lê a distância até ao lado oposto. Quando alguém passa, a distância diminui.

| Característica do Mini ToF-90 | Valor |
|---|---|
| Alcance | 3 a 200 cm. Em condições normais, o máximo é **120 cm**. |
| Campo de visão | 25° |
| Precisão | ±3 % |
| Endereço I2C | 0x29 |

**Deteção:**
- **Limiar:** largura do corredor menos 30 cm. Com 80 cm, o limiar é 50 cm.
- **Passagem:** a barreira fica ocupada quando duas leituras seguidas ficam abaixo do limiar, e livre quando a leitura passa os 10 cm acima do limiar ou deixa de haver alvo. Só volta a poder contar uma nova passagem depois de 500 ms de barreira livre. Por isso, uma pessoa parada conta uma só vez.
- **Largura:** 40 a 120 cm (de fábrica, 80 cm), definida separadamente para o Mestre e para o Remoto.

**Cuidados:**
- Meça a largura com uma fita métrica, introduza-a na webapp e confirme no separador **Calibração** que, com o corredor vazio, a distância medida é a largura.
- Evite luz forte (por exemplo solar) sobre o sensor. Nos ensaios com luz forte, o sensor deixou de dar leitura em grande parte das medições e contou passagens a mais. Com luz fraca mediu bem, mesmo em roupa escura.

## 5. Firmware

O firmware está em `Fase5_Mestre/` e em `Fase5_Remoto/`. A atualização faz-se por USB, nos dois dispositivos.

### 5.1 Software

- **Arduino IDE** com o core ESP32 da Espressif (testado com a versão **3.3.11**). Se ainda não tiver o core, acrescente este endereço em *Preferências → URLs adicionais para gestor de placas* e instale o pacote `esp32`:
  `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
- **Placa:** *M5AtomS3*.
- **USB CDC On Boot:** *Enabled*, para usar o Serial Monitor.
- **Partição:** a por defeito chega (o Mestre ocupa 92 % e o Remoto 84 % do programa). Se, noutra versão do core, o Mestre não couber, escolha *Huge APP*.

### 5.2 Bibliotecas

Instale pelo Gestor de Bibliotecas:

| Biblioteca | Versão testada | Nó |
|---|---|---|
| M5Unified | 0.2.25 | ambos |
| M5GFX | 0.2.32 | ambos |
| M5Unit-TOF | 0.3.0 | ambos |
| M5UnitUnified | 0.5.8 | ambos (dependência da M5Unit-TOF) |
| M5Utility | 0.3.1 | ambos (dependência) |
| M5HAL | 0.1.4 | ambos (dependência) |
| ArduinoJson | 7.4.3 | só o Mestre |

A biblioteca `M5Unit-AudioPlayer` **não é necessária**: o áudio é controlado enviando diretamente os comandos do [protocolo UART](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1155/Unit-AudioPlayer-Protocol-EN.pdf).

Se o compilador não encontrar `M5UnitUnified.h`, ou se a ligação falhar com símbolos duplicados, verifique que há **uma só cópia de cada biblioteca** nas pastas de bibliotecas do Arduino e que não há ficheiros duplicados do tipo `x 2.cpp`.

### 5.3 Endereços MAC, palavra-passe e canal (`Protocol.h` e firmware do Mestre)

**Endereços MAC.** O `Protocol.h` tem os MAC dos dois nós fixos. Os do repositório são os das placas originais, por isso **troque-os pelos das suas placas**:

1. Carregue qualquer um dos sketches em cada placa. Ambos escrevem o MAC no arranque, e o `?` no Serial Monitor (115200 baud) repete-o.
2. Escreva os valores em `MASTER_MAC` e `REMOTE_MAC` nas **duas** cópias de `Protocol.h`.
3. Confirme que as cópias são idênticas:

```bash
diff Fase5_Mestre/Protocol.h Fase5_Remoto/Protocol.h
```

4. Volte a compilar e carregar os dois nós.

Se o `REMOTE_MAC` estiver a zeros, o Mestre desativa o ESP-NOW e regista `REMOTE_MAC not set in Protocol.h`.

**Rede de configuração.** Estas constantes estão no `Fase5_Mestre.ino`:

| Constante | Valor |
|---|---|
| `AP_SSID` | `EscapeRoom-Audio` |
| `AP_PASSWORD` | `mission2escape` (8 a 63 caracteres) |
| IP do ponto de acesso | `192.168.4.1` |
| Máximo de clientes | 4 |

**Canal.** `ESPNOW_CHANNEL`, no `Protocol.h`, é o canal Wi-Fi do ESP-NOW **e** do ponto de acesso (de fábrica, 1). Se mudar, altere nas duas cópias de `Protocol.h` e volte a carregar os dois nós.

### 5.4 Compilar e carregar

**Arduino IDE:** abra `Fase5_Mestre/Fase5_Mestre.ino` (ou o do Remoto), escolha a placa e as opções da secção 5.1, escolha a porta da placa em *Ferramentas → Porta* e carregue.

**`arduino-cli`:** para saber a porta de cada placa ligada:

```bash
arduino-cli board list
```

Compilar (testado com os dois nós; no computador onde foi testado, foi preciso acrescentar `--libraries $HOME/Arduino/libraries`, ver abaixo):

```bash
arduino-cli compile --fqbn esp32:esp32:m5stack_atoms3:CDCOnBoot=cdc Fase5_Mestre
```

```bash
arduino-cli compile --fqbn esp32:esp32:m5stack_atoms3:CDCOnBoot=cdc Fase5_Remoto
```

Se as bibliotecas estiverem noutra pasta que não a de bibliotecas por defeito do `arduino-cli`, acrescente `--libraries <pasta>` ao comando. Sem isso, o compilador pode falhar a procurar `M5UnitUnified.h`.

Carregar, com a porta que o `board list` indicou:

```bash
arduino-cli upload -p <porta> --fqbn esp32:esp32:m5stack_atoms3:CDCOnBoot=cdc Fase5_Mestre
```

*Os comandos de compilação foram executados ao escrever este manual. O de carregamento não, porque as placas já estavam programadas.*

**Números das portas:** mudam quando se religam os cabos. Antes de carregar, confirme qual placa é qual pelo MAC que ela escreve no Serial Monitor.

### 5.5 Modo de descarga

Se a placa não aparece como porta, ou o carregamento falha, ponha-a em modo de descarga: carregue no botão de reset da placa durante cerca de **2 segundos**, até acender o LED verde interno, e largue. O LED verde apaga-se e a placa fica pronta a receber o firmware. Depois, carregue normalmente. Ver a [documentação da placa](https://docs.m5stack.com/en/arduino/m5atoms3/program).

### 5.6 Verificar depois de carregar

No Serial Monitor (115200 baud), com os dois nós ligados:

- O Mestre regista o seu MAC e o canal ESP-NOW.
- Passados cerca de 5 segundos, o Mestre regista `Remote confirmed width 80 cm` (ou a largura definida).
- Passados cerca de 7 segundos, o áudio fica pronto (`AUDIO READY`) e o LED fica **verde fixo**.
- O comando `?` mostra o estado, os parâmetros e os tempos restantes, em cada nó. É o único comando Serial que o firmware aceita.

### 5.7 Atualizar o firmware

1. Altere o código e, se mexer no `Protocol.h`, mantenha as duas cópias idênticas.
2. Carregue o Mestre e o Remoto, um de cada vez, como na secção 5.4.
3. Confirme na webapp que o Remoto está online e que as definições estão corretas. Não foi testado se as definições guardadas se mantêm depois de uma atualização de firmware.

## 6. Primeira colocação em funcionamento

1. Insira o cartão de áudio (secção 3) e ligue os dois nós. O LED do Mestre fica **verde fixo** em cerca de 10 segundos.
2. Faça um toque longo de 2 segundos no botão do Mestre para ligar a rede de configuração, e abra a página em `http://192.168.4.1` (ver o manual de utilização).
3. No separador **Ajustes**, introduza a largura de cada barreira e guarde.
4. No separador **Calibração**, com o corredor vazio, confirme que cada sensor mede a largura definida.
5. Ajuste os restantes parâmetros, se necessário. Os valores de fábrica são:

| Parâmetro | Valor de fábrica | Limites |
|---|---|---|
| Volume | 20 | 1 a 30 |
| Duração do fade-out | 3 s | 0,5 a 10 s |
| Janela de silêncio | 10 s | 1 a 120 s |
| Espera máxima | 60 s | da janela até 600 s |
| Largura na entrada (Mestre) | 80 cm | 40 a 120 cm |
| Largura na saída (Remoto) | 80 cm | 40 a 120 cm |

### Lista de verificação

- [ ] Estado **Pronto**: LED verde fixo e **Sem erros** no separador Debugging.
- [ ] Cartão **pronto** e Remoto **online**.
- [ ] Passagem na entrada: o áudio começa (LED azul).
- [ ] Passagem na saída: LED ciano, janela de silêncio, fade-out, Rearme e regresso a Pronto.
- [ ] Passagem na entrada **durante o fade-out**: o fade-out é cancelado, o volume sobe e o sistema volta a "A tocar".
- [ ] Mão parada à frente de cada sensor: conta **uma só** passagem.
- [ ] Botões **Iniciar áudio** e **Parar áudio** do Debugging.
- [ ] Remoto desligado com o áudio a tocar: LED laranja a piscar em cerca de 3 segundos, e o fade-out arranca ao fim da espera máxima.
- [ ] Cartão retirado e reposto: aviso na página e LED vermelho, que desaparecem ao repor o cartão.
- [ ] Corte de energia: as definições mantêm-se e a rede de configuração arranca desligada.
- [ ] A rede de configuração desliga-se sozinha após 10 minutos sem uso da página.
- [ ] Teste no corredor real, com a rede de configuração ligada: o Remoto mantém-se online.

## 7. Resolução de problemas

| Problema | Causa provável | O que fazer |
|---|---|---|
| A placa não aparece como porta, ou o carregamento falha | Cabo só de carga, ou placa bloqueada | Use um cabo de dados e ponha a placa em modo de descarga (secção 5.5) |
| Carreguei no nó errado | Os números das portas trocam ao religar cabos | Confirme pelo MAC no Serial Monitor antes de carregar |
| `M5UnitUnified.h: No such file` | Biblioteca em falta ou noutra pasta | Instale a biblioteca (secção 5.2). Com `arduino-cli`, use `--libraries` |
| Erro de ligação com símbolos duplicados | Cópias duplicadas de uma biblioteca | Deixe uma só cópia de cada biblioteca, sem ficheiros `x 2.cpp` |
| `REMOTE_MAC not set in Protocol.h` | MAC por preencher | Preencha o MAC (secção 5.3) |
| Remoto sempre offline | MAC trocados ou errados, `Protocol.h` diferentes nos dois nós, canal diferente, ou sem alimentação | Compare os dois `Protocol.h` com `diff` e recarregue os dois nós |
| LED vermelho a piscar depressa desde o arranque | Sensor da entrada sem dados | Verifique o cabo na **P.A** (fios nos G38/G39) e reinicie. O Mestre fica parado até ser reiniciado |
| LED magenta a piscar devagar | Sensor do Remoto sem dados | Verifique o cabo na porta Grove do Remoto (G2/G1) |
| LED vermelho fixo (erro de cartão) | Cartão em falta, formato não suportado ou unidade sem resposta | Cartão inserido antes de ligar (secção 3), unidade de áudio na **P.C**, e fios G5/G6 |
| O áudio continuava a tocar depois de reiniciar o Mestre | A unidade de áudio é alimentada pelo USB e não reinicia com o ESP | Já tratado: o Mestre envia um comando de paragem no arranque |
| A rede `EscapeRoom-Audio` não aparece | Rede desligada, ou falha ao arrancar | Toque longo de 2 s no botão do Mestre. Se o registo mostrar `AP failed to start`, confirme que `AP_PASSWORD` tem 8 a 63 caracteres |
| Falhas de ligação sem fios no local | Interferência no canal 1 | Mude `ESPNOW_CHANNEL` nas duas cópias de `Protocol.h` e recarregue os dois nós |
| Passagens contadas a mais, ou leituras em falta | Luz forte sobre o sensor | Proteja o sensor da luz (secção 4) |

## 8. Anexo: valores de referência do firmware

| Constante | Valor | Onde |
|---|---|---|
| Margem do limiar | 30 cm | `THRESHOLD_MARGIN_CM` (Mestre) |
| Histerese | 10 cm | `HYSTERESIS_CM` (Mestre) |
| Rearme da barreira | 500 ms | `REARM_MS` (Mestre) |
| Fade-in | 50 ms por nível de volume | `FADE_IN_STEP_MS` (Mestre) |
| Toque longo no botão | 2 s | `LONG_PRESS_MS` (Mestre) |
| Corte da rede de configuração | 10 min sem pedidos da página | `AP_IDLE_TIMEOUT_MS` (Mestre) |
| Brilho do LED | 24 de 255 | `LED_LEVEL` (Mestre) |
| Heartbeat do Remoto | 1 s | `HEARTBEAT_PERIOD_MS` (`Protocol.h`) |
| Remoto offline | 3 s sem pacotes | `REMOTE_OFFLINE_MS` (`Protocol.h`) |
| Canal Wi-Fi | 1 | `ESPNOW_CHANNEL` (`Protocol.h`) |

O MAC das placas originais (`Protocol.h`): Mestre `AC:A7:04:00:35:4C`, Remoto `AC:A7:04:00:42:00`.
