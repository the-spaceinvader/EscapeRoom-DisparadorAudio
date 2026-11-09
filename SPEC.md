# Dispositivo de áudio para corredor de escape room

## Objetivo
Tocar um áudio em loop enquanto há pessoas a atravessar um corredor de 8 m em cotovelo, e terminá-lo com fade-out depois de saírem.

## Componentes

| Dispositivo | Local | Componentes | Função |
|---|---|---|---|
| **Mestre** | Entrada | AtomS3 Lite, ABC Port, Unit Mini ToF-90 (U196), Unit AudioPlayer | Deteta passagens na entrada, controla o áudio, gere o estado do jogo e disponibiliza a webapp |
| **Remoto** | Saída | AtomS3 Lite, Unit Mini ToF-90 (U196) | Deteta passagens na saída e comunica-as ao Mestre |

Os dispositivos comunicam sem fios por ESP-NOW. Cada ToF funciona como uma barreira de um lado ao outro do corredor. Ambos usam os dois cores do S3, separando a comunicação da lógica do jogo.

## Funcionamento
1. **Pronto:** o sistema arranca em silêncio e aguarda.
2. **A tocar:** uma passagem na entrada inicia o áudio em loop.
3. **A aguardar:** uma passagem na saída inicia a contagem da janela de silêncio.
4. **Fade-out:** terminada a janela sem novas passagens, o volume desce gradualmente até 0 e o áudio para.
5. **Rearme:** o sistema volta sozinho a Pronto, com o volume configurado reposto.

## Regras
- Uma passagem em qualquer barreira, durante a espera, reinicia a janela de silêncio.
- Uma passagem na entrada, durante o fade-out, cancela-o e o volume sobe em fade-in até ao valor configurado. O fade-in tem duração fixa, a menor que o intervalo entre degraus de volume permitir, e não é ajustável.
- Uma passagem na saída, com o sistema em Pronto, é ignorada.
- Se as passagens não pararem, o fade-out começa ao fim de uma espera máxima, contada desde a primeira deteção na saída.
- Uma pessoa parada em frente à barreira conta como uma única passagem.
- Uma ordem enviada pelo Remoto não pode perder-se nem ser executada em duplicado.
- O jogo continua a funcionar normalmente enquanto o AP está ligado.

## Parâmetros ajustáveis
- Volume do áudio
- Duração do fade-out
- Janela de silêncio
- Espera máxima
- Largura do corredor em cada barreira, que define o limiar de deteção (80 cm por defeito)

Os valores mantêm-se depois de desligar o AP ou a alimentação.

## Modos de operação
- **Normal:** o AP e a webapp ficam desligados, e o jogo funciona de forma autónoma. É o modo em que o Mestre arranca sempre.
- **Configuração:** um toque longo no botão do Mestre liga ou desliga o AP, com palavra-passe fixa, e o estado é indicado no LED. O AP desliga-se sozinho após 10 minutos sem ninguém ligado à webapp.

## Webapp
- **Ajustes:** todos os parâmetros dos dois dispositivos, a partir do Mestre.
- **Calibração:** distância medida em direto em cada barreira.
- **Debugging:** iniciar e parar o áudio, e ver o estado do jogo, a ligação ao Remoto, os erros (incluindo a falta do cartão de áudio) e o registo de eventos.

## Atualização de firmware
Por USB, nos dois dispositivos.
