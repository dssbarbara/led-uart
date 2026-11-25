# Controle de LED RGB pela UART

Receptor UART para a placa **FRDM-KL25Z** que controla o LED RGB a partir de um protocolo fixo de 6 bytes. Feito com **Zephyr 4.2** e **PlatformIO**.

**Integrantes**: Barbara e Ingrid

## Funcionalidades

- Recebe pacotes de 6 bytes pela UART (115200 8N1).
- Valida o **ID** e o **checksum (XOR)** de cada pacote.
- Executa as ações: ligar, desligar, toggle e piscar (1s ou 2s).
- Descarta pacotes inválidos ou truncados sem travar.
- Recepção por interrupção + fila, e pisca não bloqueante (`k_work_delayable`).

> **Não implementado ainda (bônus):** CRC-8 no lugar do XOR e controle de intensidade com PWM. Hoje cada cor só liga (valor > 0) ou desliga.

## Hardware

| Item | Detalhe |
|---|---|
| Placa | FRDM-KL25Z |
| LED vermelho | PTB18 (ativo em nível baixo) |
| LED verde | PTB19 (ativo em nível baixo) |
| LED azul | PTD1 (ativo em nível baixo) |
| UART | `uart0` (ponte USB do OpenSDA, aparece como `/dev/ttyACM0` no Linux) |

A inversão do nível baixo já está descrita no devicetree da placa (`GPIO_ACTIVE_LOW`), então no código `gpio_pin_set_dt(..., 1)` significa "LED aceso".

## Estrutura do projeto

```
led_uart/
├── platformio.ini        # ambiente PlatformIO (placa frdm_kl25z, framework zephyr)
├── src/main.c            # lógica do receptor
├── zephyr/
│   ├── CMakeLists.txt    # build do Zephyr
│   ├── prj.conf          # configuração do Kconfig
│   └── app.overlay       # devicetree: apelidos dos LEDs e baud rate da UART
├── teste.py              # script de teste (pyserial)
└── README.md
```

## Protocolo

Cada pacote tem 6 bytes:

```
Byte 0   Byte 1   Byte 2   Byte 3   Byte 4   Byte 5
  ID       R        G        B      AÇÃO     CHECK
```

| Campo | Descrição |
|---|---|
| `ID` | Identificador fixo do dispositivo: `0xA5` (constante `PKT_ID` em `main.c`) |
| `R`, `G`, `B` | Cor desejada, 0 a 255 |
| `AÇÃO` | Código da ação (tabela abaixo) |
| `CHECK` | `ID ^ R ^ G ^ B ^ AÇÃO` (XOR dos 5 primeiros bytes) |

### Ações

| Código | Ação | Comportamento |
|---|---|---|
| `0x01` | LIGAR | Acende com a cor R, G, B recebida |
| `0x02` | DESLIGAR | Apaga o LED |
| `0x03` | TOGGLE | Inverte o estado atual (usando a cor do pacote) |
| `0x04` | PISCA 1s | Alterna aceso/apagado a cada 1 segundo |
| `0x05` | PISCA 2s | Alterna aceso/apagado a cada 2 segundos |

Qualquer comando válido cancela o pisca anterior antes de ser aplicado.

### Exemplo

Ligar o vermelho:

```
ID=A5  R=FF  G=00  B=00  AÇÃO=01
CHECK = A5 ^ FF ^ 00 ^ 00 ^ 01 = 5B

Pacote: A5 FF 00 00 01 5B
```

## Como o receptor funciona

1. A **ISR da UART** (`uart_cb`) só lê os bytes que chegaram e os coloca numa fila (`k_msgq`).
2. O loop do `main()` retira os bytes da fila e monta o pacote com uma pequena máquina de estados:
   - Sem nenhum byte do pacote ainda, espera indefinidamente.
   - Com o pacote em andamento, espera no máximo **100 ms** (`RX_TIMEOUT_MS`) pelo próximo byte. Se estourar, descarta o pacote truncado e recomeça.
   - Se o primeiro byte não for o ID, ele é ignorado. É assim que o receptor se ressincroniza depois de lixo na linha.
3. Com 6 bytes completos, `process_packet()` valida o ID, o checksum e o código da ação. Se algo falhar, o pacote é ignorado e o receptor continua ouvindo.
4. O pisca usa `k_work_delayable`, que reagenda a si mesmo e não bloqueia a recepção. Um mutex protege o estado do LED.

**Console e log estão desligados** (`prj.conf`), porque o `uart0` é a mesma UART do console padrão e as mensagens de debug se misturariam com os bytes do protocolo.

## Compilar e gravar

```bash
pio run                # compila
pio run -t upload      # grava na placa
```

## Testar

Instale o `pyserial` e dê permissão de acesso à serial (só na primeira vez; depois faça logout e login):

```bash
pip install pyserial
sudo usermod -aG dialout $USER
```

Rode o script de teste:

```bash
python3 teste.py /dev/ttyACM0
```

O script envia comandos válidos (liga, toggle, pisca, desliga) e depois casos inválidos. Nos casos inválidos o LED **não deve mudar**:

| Caso | Resultado esperado |
|---|---|
| ID errado | Ignorado |
| CHECK errado | Ignorado |
| Ação inválida (`0x09`) | Ignorado |
| Pacote truncado (3 bytes) | Descartado por timeout |
| Pacote válido logo depois | Executado normalmente |


## Decisões de projeto

- **"Pisca 1s / 2s"** foi interpretado como trocar de estado a cada 1s ou 2s. Para um ciclo completo de 1s ou 2s, basta dividir o período por 2.


##Teste do Código

https://github.com/user-attachments/assets/d9e998a9-fb87-4a6c-ac0a-1f779c5e7869


