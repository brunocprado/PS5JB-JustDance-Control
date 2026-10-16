# PS5JustDance

Jogue **Just Dance** (2014 a 2022, versões de PS4 no PS5) em um **PS5 com jailbreak** usando **Joy-Cons do Nintendo Switch** ou **qualquer Celular**, sem precisar de servidores da Ubisoft, PSN ou PS Camera.

Baseado na mesma arquitetura de injeção de stub freestanding do `PS5Instrumentalizer`.

## Como funciona

Todas as versões de PS4 do Just Dance (`Just Dance 2014` até `Just Dance 2022`) possuem suporte nativo e **100% offline** para até 4 controles **PlayStation Move** (`libSceMove.sprx`), pontuando exclusivamente pelo acelerômetro e giroscópio (sem exigir a PlayStation Camera).

O `PS5JustDance.elf` roda em segundo plano no PS5:
1. Detecta automaticamente qualquer edição do Just Dance (`CUSA*`) ao iniciar.
2. Injeta um stub independente (`build/stub.bin`) no processo do jogo e redireciona as chamadas da `libSceMove` (`sceMoveInit`, `sceMoveOpen`, `sceMoveGetDeviceInfo`, `sceMoveReadStateLatest`, `sceMoveReadStateRecent`, `sceMoveSetLightSphere`, `sceMoveSetVibration`).
3. Emula até **4 controles PS Move** em tempo real a partir de **3 fontes de entrada simultâneas**:
   - **Joy-Con via BetterJoy / DSU (Cemuhook)**: compatível nativamente com o protocolo `DSUS`/`DSUC` (Windows/Linux/Android).
   - **Joy-Con via Script Python (`tools/joycon_bridge.py`)**: lê de 1 a 4 Joy-Cons (Esquerdo ou Direito) via Bluetooth HID, converte os eixos do IMU para o referencial do PS Move e transmite pacotes binários `JDM1` via UDP (porta `9052`).
   - **Celular via Navegador Web (`http://IP_DO_PS5:8080`)**: servidor HTTP embutido no próprio plugin do PS5 (ou envio via apps DSU no Android/iOS).

## Como usar no PS5

1. Ative o jailbreak e o **etaHEN** no PS5.
2. Envie o plugin para o console:
   - Copie `PS5JustDance.elf` para `/data/etaHEN/plugins/` e carregue pelo etaHEN Toolbox, **ou**
   - Envie diretamente para o ELF loader:
     ```sh
     nc -q0 IP_DO_PS5 9021 < PS5JustDance.elf
     ```
3. Abra qualquer **Just Dance** (versão de PS4 rodando no PS5). Uma notificação na tela confirmará: **"PS5JustDance: Joy-Con / Phone Move support active!"**.

---

## Conectando seus Controles

### Opção 1: Joy-Con do Switch com `joycon_bridge.py` (PC / Linux / Mac)

1. Pareie seu(s) Joy-Con(s) no Bluetooth do computador.
2. Instale a biblioteca `hidapi` e execute a ponte apontando para o IP do PS5:
   ```sh
   pip install hidapi
   python3 tools/joycon_bridge.py --ps5-ip 192.168.1.100
   ```
3. Na tela de seleção de controle do Just Dance, escolha **PS Move** e aperte o botão **ZR** (Joy-Con Direito) ou **ZL** (Joy-Con Esquerdo), que corresponde ao gatilho **T / Move**!

| Botão Joy-Con (Direito / Esquerdo) | Botão PS Move Emulado | Função no Just Dance |
| --- | --- | --- |
| `ZR` / `ZL` | Gatilho `T` | Entrar como jogador / Confirmar PS Move |
| `R` / `L` / `Home` / `Capture` | Botão `Move` | Ação / Especial |
| `A` / `Baixo` | `X` (Cruz) | Confirmar nos menus |
| `B` / `Esquerda` | `O` (Círculo) | Voltar nos menus |
| `X` / `Cima` | `Triângulo` | Alternar opção |
| `Y` / `Direita` | `Quadrado` | Favoritar / Busca |
| `+` / `-` | `START` | Pausar música |

### Opção 2: Joy-Con ou Celular via BetterJoy / DSU (Cemuhook)

- **No PC (BetterJoy)**: No arquivo `/data/PS5JustDance/config.ini` no PS5, defina `dsu_server_ip = IP_DO_SEU_PC`. O PS5 se inscreverá automaticamente na porta `26760` do BetterJoy e receberá até 4 Joy-Cons!
- **No Celular (DSUController / MotionSource)**: Aponte o app ou um relay UDP para `IP_DO_PS5` na porta `9052`.

### Opção 3: Celular via Interface Web Embutida

Abra `http://IP_DO_PS5:8080` no navegador do celular, selecione `Jogador 1..4` e toque em **Ativar Sensores**.
*(Nota: navegadores modernos como iOS Safari exigem HTTPS para liberar `DeviceMotionEvent` fora de `localhost`; no Android Chrome você pode habilitar `chrome://flags/#unsafely-treat-insecure-origin-as-secure` adicionando `http://IP_DO_PS5:8080`, ou usar um app DSU no celular).*

---

## Compilando e Testando

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk   # ou usa automaticamente ../.deps/sdk/ps5-payload-sdk
make        # gera build/stub.bin e PS5JustDance.elf
make test   # executa os testes de unidade no host (protocolos, stub freestanding e config/SFO)
make clean
```
