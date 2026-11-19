#!/usr/bin/env bash
# Flash the ESP32, then start the backend (USB serial or Bluetooth reader) and
# the frontend.
#   ./start.sh
#   SERIAL_PORT=/dev/ttyACM0 ./start.sh
#   TRANSPORT=ble ./start.sh        # skip the transport prompt
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# The firmware sends over USB serial and BLE at the same time; this only picks
# how the backend receives the data.
if [ -z "${TRANSPORT:-}" ]; then
  IFS= read -r -p "Transport for the dashboard [serial/ble] (ble): " TRANSPORT || true
  TRANSPORT="${TRANSPORT:-ble}"
fi
case "$TRANSPORT" in
  serial|ble) ;;
  *) echo "Unknown transport '$TRANSPORT' (use serial or ble)." >&2; exit 1 ;;
esac
export TRANSPORT

# Find PlatformIO.
PIO="${PIO:-}"
if [ -z "$PIO" ]; then
  if command -v pio >/dev/null 2>&1; then
    PIO=pio
  elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then
    PIO="$HOME/.platformio/penv/bin/pio"
  else
    echo "PlatformIO not found. Install it or set PIO=/path/to/pio." >&2
    exit 1
  fi
fi
PIO_ABS="$(command -v "$PIO" || true)"
PIO_ABS="${PIO_ABS:-$PIO}"

# One board per ping-pong player; the firmware has an env per player.
if [ -z "${PLAYERS:-}" ]; then
  IFS= read -r -p "How many players to flash [1/2] (2): " PLAYERS || true
  PLAYERS="${PLAYERS:-2}"
fi
case "$PLAYERS" in
  1|2) ;;
  *) echo "Invalid player count '$PLAYERS' (use 1 or 2)." >&2; exit 1 ;;
esac
export PLAYERS
REQUESTED_SERIAL_PORT="${SERIAL_PORT:-}"
USED_PORTS=""
FLASHED_PORTS=()

find_serial_port() {
  if [ -n "$REQUESTED_SERIAL_PORT" ]; then
    printf '%s' "$REQUESTED_SERIAL_PORT"
    return
  fi
  local port
  for port in /dev/ttyACM* /dev/ttyUSB*; do
    [ -e "$port" ] || continue
    case " $USED_PORTS " in *" $port "*) continue ;; esac
    printf '%s' "$port"
    return
  done
}

# The ESP32's /dev/ttyACM* is root:dialout by default, which makes esptool and
# the backend fail with "Permission denied" for a normal user. Ask for sudo once
# and install a udev rule so the board works after every replug and reset.
ensure_port_access() {
  local port="$1"
  if [ -r "$port" ] && [ -w "$port" ]; then
    return
  fi
  echo "==> $port is not accessible to $USER; installing a udev rule (sudo required once)"
  sudo tee /etc/udev/rules.d/99-omniflux-esp32.rules >/dev/null <<'EOF'
# OmniFlux: allow non-root access to ESP32 USB serial ports.
KERNEL=="ttyACM[0-9]*", SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"
KERNEL=="ttyUSB[0-9]*", SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", MODE="0666"
EOF
  sudo udevadm control --reload-rules
  sudo udevadm trigger --subsystem-match=tty
  sleep 1
  # Fallback for this session if udev did not pick the rule up.
  if [ ! -r "$port" ] || [ ! -w "$port" ]; then
    sudo chmod a+rw "$port"
  fi
}

upload_player() {
  local player="$1" port="$2" upload_log
  if ! upload_log="$("$PIO" run -d Frimware -e "player$player" -t upload --upload-port "$port" 2>&1)"; then
    printf '%s\n' "$upload_log"
    if grep -qi "permission denied" <<<"$upload_log"; then
      echo "==> Port access denied; retrying the upload with sudo"
      sudo env HOME="$HOME" PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}" \
        "$PIO_ABS" run -d Frimware -e "player$player" -t upload --upload-port "$port"
    else
      exit 1
    fi
  else
    printf '%s\n' "$upload_log"
  fi
}

for dir in backend frontend; do
  if [ ! -d "$dir/node_modules" ]; then
    echo "==> Installing $dir dependencies"
    (cd "$dir" && npm install --no-audit --no-fund)
  fi
done

if [ "$TRANSPORT" = serial ] && [ "$PLAYERS" -gt 1 ]; then
  echo "==> Serial mode: the backend will read each flashed board's port." >&2
fi

for ((player = 1; player <= PLAYERS; player++)); do
  SERIAL_PORT="$(find_serial_port)"
  if [ -z "$SERIAL_PORT" ] || [ ! -e "$SERIAL_PORT" ]; then
    echo "No ESP32 serial port found for player $player. Plug it in or set SERIAL_PORT." >&2
    exit 1
  fi
  ensure_port_access "$SERIAL_PORT"

  echo "==> Flashing player $player/$PLAYERS (env player$player) on $SERIAL_PORT"
  upload_player "$player" "$SERIAL_PORT"
  FLASHED_PORTS+=("$SERIAL_PORT")
  USED_PORTS="$USED_PORTS $SERIAL_PORT"

  # The board re-enumerates after the upload reset; keep the port readable.
  for _ in $(seq 1 20); do
    if [ -r "$SERIAL_PORT" ] && [ -w "$SERIAL_PORT" ]; then
      break
    fi
    sudo chmod a+rw "$SERIAL_PORT" 2>/dev/null || true
    sleep 0.5
  done

  if (( player < PLAYERS )); then
    # Wait for a swap only when no unused board is already plugged in.
    if [ -z "$(find_serial_port)" ]; then
      echo
      IFS= read -r -p "Unplug player $player, connect the next ESP32, then press Enter... " _ || true
      USED_PORTS=""   # the previous board is gone; its port can be reused
    fi
  fi
done

SERIAL_PORTS="$(IFS=,; echo "${FLASHED_PORTS[*]}")"

echo "==> Starting backend on http://localhost:3001 ($TRANSPORT)"
TRANSPORT="$TRANSPORT" SERIAL_PORTS="$SERIAL_PORTS" node backend/server.js &
BACKEND=$!

echo "==> Starting frontend on http://localhost:3000"
node frontend/node_modules/nuxt/bin/nuxt.mjs dev frontend &
FRONTEND=$!

cleanup() {
  kill "$BACKEND" "$FRONTEND" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo
echo "Dashboard: http://localhost:3000   (Ctrl-C stops everything)"
wait -n "$BACKEND" "$FRONTEND"
