# Disparador Áudio — dispositivo de áudio para corredor de escape room

Toca um áudio em loop enquanto há pessoas a atravessar um corredor de 8 m em cotovelo e termina-o com fade-out depois de saírem. São dois nós ESP32-S3 (AtomS3 Lite) que comunicam sem fios por ESP-NOW, com uma página de configuração no telemóvel.

- **Mestre** (entrada): deteta passagens, controla o áudio, gere o estado do jogo e serve a página de configuração.
- **Remoto** (saída): deteta passagens e comunica-as ao Mestre.

Cada sensor de distância funciona como uma barreira de um lado ao outro do corredor.

## Documentação

| Ficheiro | Conteúdo |
|---|---|
| [`SPEC.md`](SPEC.md) | Especificação funcional (a referência) |
| [`DECISIONS.md`](DECISIONS.md) | Decisões de projeto, hardware verificado e resultados dos testes de cada fase |
| [`Manual_Utilizacao.md`](Manual_Utilizacao.md) | Manual de utilização |

O `DECISIONS.md` cita sketches das Fases 1 a 4 (por exemplo `Fase3_Logica/Fase3_Logica.ino`). São etapas intermédias de desenvolvimento e não fazem parte deste repositório. O firmware final está em `Fase5_Mestre/` e `Fase5_Remoto/`.

## Hardware

| Nó | Componentes |
|---|---|
| Mestre | [AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite), [Atomic Port ABC Base](https://docs.m5stack.com/en/atom/AtomPortABC), [Unit Mini ToF-90 (U196)](https://docs.m5stack.com/en/unit/Unit_Mini_ToF-90), [Unit AudioPlayer (U197)](https://docs.m5stack.com/en/unit/Unit_AudioPlayer) |
| Remoto | AtomS3 Lite, Unit Mini ToF-90 (U196) |

Ligações do Mestre pela ABC Port: ToF na P.A (I2C, G38/G39) e AudioPlayer na P.C (UART, G5/G6). No Remoto, o ToF liga à porta Grove da própria placa (G2/G1).

## Firmware

| Pasta | Nó |
|---|---|
| `Fase5_Mestre/` | Mestre (`Fase5_Mestre.ino`, `WebPage.h`, `Protocol.h`) |
| `Fase5_Remoto/` | Remoto (`Fase5_Remoto.ino`, `Protocol.h`) |

Os dois `Protocol.h` têm de ser **idênticos**. Para verificar:

```bash
diff Fase5_Mestre/Protocol.h Fase5_Remoto/Protocol.h
```

### Compilar e carregar (Arduino IDE)

- Core ESP32 da Espressif (testado com 3.3.11), placa **M5AtomS3**.
- Bibliotecas: `M5Unified`, `M5GFX`, `M5Unit-TOF` (com `M5UnitUnified`, `M5Utility` e `M5HAL`) e `ArduinoJson` 7.x (só o Mestre).
- *Tools → USB CDC On Boot → Enabled* para usar o Serial Monitor a 115200 baud.
- Se o sketch do Mestre não couber, *Tools → Partition Scheme → Huge APP*.
- A atualização de firmware faz-se por USB, nos dois dispositivos.

### Antes de usar com as tuas placas

O `Protocol.h` tem os endereços MAC dos dois nós fixos no firmware. Substitui-os pelos das tuas placas, em **ambas** as cópias, antes de compilar.

O canal ESP-NOW e do ponto de acesso é a constante `ESPNOW_CHANNEL` no `Protocol.h`. A palavra-passe do ponto de acesso está em `AP_PASSWORD` no `Fase5_Mestre.ino`.

## Estado

Todas as fases do plano foram concluídas e validadas num corredor real (ver `DECISIONS.md`).

## Licença

GPL-3.0. Ver o ficheiro `LICENSE`.
