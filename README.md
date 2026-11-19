# OmniFlux

ESP32-S3 wrist gesture recognition (extension / flexion), with a live web dashboard.

| Folder      | What it is                                                        |
| ----------- | ----------------------------------------------------------------- |
| `Frimware/` | PlatformIO firmware; classifies gestures and prints JSON over USB and BLE |
| `Train/`    | Model training and int8 export                                     |
| `dataset/`  | Gesture capture scripts and recorded CSVs                          |
| `backend/`  | Node server: reads the ESP32 (USB serial or Bluetooth) and streams state over SSE |
| `frontend/` | Nuxt dashboard: live confidence chart and statistics               |

## Web dashboard

### One command

With the ESP32 plugged in, from the repository root:

```bash
./start.sh
```

This installs missing dependencies, flashes **two** boards (player 1, then player 2;
they can be plugged in at the same time or swapped one at a time), and starts the
backend and frontend. Open <http://localhost:3000>; Ctrl-C stops both servers. Set
`SERIAL_PORT=/dev/ttyACM0` if auto-detection picks the wrong port. The script asks
whether the dashboard should receive data over USB serial or Bluetooth (`ble` is
the default; with USB, every flashed board is read on its own port).

If your user cannot access the serial port, the script asks for your sudo password once and installs `/etc/udev/rules.d/99-omniflux-esp32.rules`, which makes ESP32 serial ports accessible without sudo from then on. If the upload still fails with "Permission denied", it retries the upload with sudo.

If running `./start.sh` itself says "Permission denied", make it executable with `chmod +x start.sh` (or run `bash start.sh`).

The manual steps are below.

The firmware prints one JSON line per detected gesture on its USB serial port:

```json
{"player":1,"gesture":1,"name":"WRIST_FLEXION","confidence":0.87}
```

Move into the gesture and **hold it for about 2 seconds**. The model was trained on held poses, so a quick swing that returns to neutral is ignored (the model would otherwise call the rest pose flexion).

As soon as motion crosses the trigger threshold the firmware prints a recording
status line. The dashboard shows it as "Recording…" while the window is captured,
then replaces it with the classified gesture or an idle line:

```json
{"player":1,"gesture":-2,"name":"MOVING","confidence":0}
```

After reporting, the firmware pauses 500 ms and then waits for the next motion
above the threshold.

If no movement above the trigger threshold is seen for 5 s, it prints one idle
line instead. Idle events are shown on the dashboard but are not counted as
gestures:

```json
{"player":1,"gesture":-1,"name":"NO_MOVEMENT","confidence":0}
```

### Bluetooth (BLE)

Each board advertises as `OmniFlux-<n>` with a BLE service (Nordic UART UUIDs)
and notifies the same JSON lines it prints on USB serial. The backend can read them
instead of the serial port, so the browser does not need Web Bluetooth and any
browser works:

```bash
cd backend
TRANSPORT=ble npm start
```

On Linux this goes through BlueZ over DBus (`node-ble`); make sure the Bluetooth
service is running (`sudo systemctl enable --now bluetooth`). The backend scans
for every `OmniFlux-*` device, keeps one connection per player, tags each event
with its player number, reconnects every few seconds if a board reboots or moves
out of range, and keeps serving `/api/events` and `/api/state` unchanged.

`./start.sh` asks for the transport when it starts; picking `ble` flashes player
1, waits for you to connect player 2, flashes it, and then runs the dashboard
over Bluetooth. `TRANSPORT=ble ./start.sh` skips the prompt. Set `BLE_NAME` if
the advertised prefix changes.

### 1. Backend

```bash
cd backend
npm install
SERIAL_PORTS=/dev/ttyACM0,/dev/ttyACM1 npm start   # defaults to /dev/ttyACM1
```

Listens on port `3001`. Endpoints:

- `GET /api/events` — SSE stream (`snapshot` events with full history + stats).
- `GET /api/state` — current snapshot as JSON.

`PORT`, `TRANSPORT` (`serial` or `ble`), `SERIAL_PORTS` (comma-separated, one per
player), `SERIAL_PORT` (single-port shorthand), `SERIAL_BAUD` (default `115200`)
and `BLE_NAME` (default `OmniFlux`) are configurable via environment variables.
Each JSON line carries its player number, so the order of `SERIAL_PORTS` only
matters for older firmware. On Linux the serial device usually needs group access
(`sudo usermod -aG dialout $USER`, then log out and back in).

### 2. Frontend

```bash
cd frontend
npm install
npm run dev                             # http://localhost:3000
```

If the backend is not on `localhost:3001`, set `NUXT_PUBLIC_API_BASE` before starting, e.g. `NUXT_PUBLIC_API_BASE=http://192.168.1.20:3001 npm run dev`.

The dashboard shows connection status, total gestures, the last gesture and its confidence, average confidence, a confidence timeline (last 60 gestures) and the class distribution. History is kept in memory by the backend (last 120 gestures), so a backend restart clears it.

### Games

- **Wrist Flappy** — extension steers up, flexion steers down.
- **Ping Pong** — two players, one ESP32 each. The ball bounces between the
  paddles; when it reaches your side, flick from extension to flexion within
  one second. The firmware reports a gesture about 2 s after the motion (the
  capture window plus classification), so the ball waits at the paddle for the
  verdict instead of deciding on arrival. First to 7 wins.
