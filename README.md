# PS5JustDance

Play **Just Dance** (2014 to 2022, PS4 versions running on PS5) on a **jailbroken PS5** using **Nintendo Switch Joy-Cons** or **any Smartphone**, without needing Ubisoft servers, PSN, or a PlayStation Camera.

Built on the same freestanding stub injection architecture as `PS5Instrumentalizer`.

## How It Works

All PS4 editions of Just Dance (`Just Dance 2014` through `Just Dance 2022`) include native, **100% offline** support for up to 4 **PlayStation Move** controllers (`libSceMove.sprx`), scoring dance moves purely from the 3-axis accelerometer and 3-axis gyroscope (no PlayStation Camera required).

`PS5JustDance.elf` runs in the background on the PS5:
1. Automatically detects any Just Dance edition (`CUSA*`) when launched.
2. Injects a freestanding, position-independent stub (`build/stub.bin`) into the game process and hooks both `libSceMove` (`sceMoveInit`, `sceMoveOpen`, `sceMoveGetDeviceInfo`, `sceMoveReadStateLatest`, `sceMoveReadStateRecent`, `sceMoveSetLightSphere`, `sceMoveSetVibration`) and `libScePad` (`scePadReadState`, `scePadRead`) for full in-game UI menu navigation.
3. Emulates up to **4 PlayStation Move controllers** in real time from **3 simultaneous input sources**:
   - **Switch Joy-Con via Python Bridge (`tools/joycon_bridge.py`)**: reads 1 to 4 Joy-Cons (Left or Right) over Bluetooth HID, converts IMU axes to the PS Move coordinate frame, maps the analog stick to D-Pad navigation, and streams 36-byte `JDM1` UDP packets (port `9052`).
   - **Switch Joy-Con or Smartphone via BetterJoy / DSU (Cemuhook)**: natively compatible with the `DSUS`/`DSUC` v1001 protocol (Windows/Linux/Android/iOS).
   - **Smartphone via Built-in Web UI (`http://PS5_IP:8080`)**: embedded HTTP server inside the PS5 payload featuring a bilingual (**EN / PT-BR**) mobile controller UI with live accelerometer readout, sensitivity control, dance simulation pad, and full D-Pad/PlayStation menu navigation buttons.

---

## How to Use on PS5

1. Enable your PS5 jailbreak and **etaHEN**.
2. Send the payload to the console:
   - Copy `PS5JustDance.elf` to `/data/etaHEN/plugins/` and load it via etaHEN Toolbox, **or**
   - Send it directly to the ELF loader:
     ```sh
     nc -q0 PS5_IP 9021 < PS5JustDance.elf
     ```
3. Launch any **Just Dance** title (PS4 version running on PS5). An on-screen notification will confirm: **"PS5JustDance: Joy-Con / Phone Move support active!"**.

---

## Connecting Your Controllers

### Option 1: Nintendo Switch Joy-Con with `joycon_bridge.py` (Windows / Linux / macOS)

1. Pair your Joy-Con(s) to your computer via Bluetooth.
2. Install `hidapi` and run the bridge pointing to your PS5 IP:
   ```sh
   pip install hidapi
   python3 tools/joycon_bridge.py --ps5-ip 192.168.1.100
   ```
3. On the Just Dance controller selection screen, choose **PS Move** and press **ZR** (Right Joy-Con) or **ZL** (Left Joy-Con) to join!

| Joy-Con Button (Right / Left) | Emulated PS Move / Pad Button | Function in Just Dance |
| --- | --- | --- |
| `ZR` / `ZL` | `T` Trigger + `Move` | Join lobby / Confirm PS Move |
| `R` / `L` / `Home` / `Capture` | `Move` Button | Action / Special |
| `Analog Stick` | `D-Pad Up / Down / Left / Right` | Navigate song list & UI menus |
| `A` / `Down` | `Cross (X)` + `Move` | Confirm in menus |
| `B` / `Left` | `Circle (O)` | Back in menus |
| `X` / `Up` | `Triangle` | Toggle option / Sort |
| `Y` / `Right` | `Square` | Favorite / Search |
| `+` / `-` | `START / OPTIONS` | Pause song |

### Option 2: Joy-Con or Smartphone via BetterJoy / DSU (Cemuhook)

- **On PC (BetterJoy / joycond-cemuhook)**: In `/data/PS5JustDance/config.ini` on the PS5, set `dsu_server_ip = YOUR_PC_IP`. The PS5 will automatically subscribe to port `26760` and map up to 4 Joy-Cons.
- **On Smartphone (MotionSource / DSUController)**: Point the app's UDP output to `PS5_IP` on port `9052`.

### Option 3: Smartphone via Built-in Web UI (`http://PS5_IP:8080`)

1. Open `http://PS5_IP:8080` in your phone's browser.
2. Use the top-right **PT-BR / EN** button to switch the UI language at any time.
3. Select **Player 1..4** and tap **ENABLE SENSORS**.
4. **Unlocking Accelerometer over HTTP**:
   - Mobile browsers block `DeviceMotionEvent` on plain `http://` origins by default.
   - **On Android (Chrome)**: Open `chrome://flags/#unsafely-treat-insecure-origin-as-secure`, add `http://PS5_IP:8080`, set it to **Enabled**, and relaunch Chrome.

---

## Building and Testing

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk   # or automatically uses ../.deps/sdk/ps5-payload-sdk
make        # builds build/stub.bin and PS5JustDance.elf
make test   # runs host unit tests (protocols, freestanding stub, and config/SFO parser)
make clean
```

## License

Licensed under the GNU Lesser General Public License v2.1 (see `LICENSE.txt`).
