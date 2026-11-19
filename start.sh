#!/usr/bin/env bash
# Flash the ESP32, then start the backend (USB serial reader) and the frontend.
#   ./start.sh
#   SERIAL_PORT=/dev/ttyACM0 ./start.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

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

# Pick the ESP32's USB serial port unless SERIAL_PORT is already set.
if [ -z "${SERIAL_PORT:-}" ]; then
  SERIAL_PORT="$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -n 1 || true)"
fi
if [ -z "$SERIAL_PORT" ]; then
  echo "No ESP32 serial port found. Plug it in or set SERIAL_PORT." >&2
  exit 1
fi
if [ ! -e "$SERIAL_PORT" ]; then
  echo "Serial port $SERIAL_PORT does not exist." >&2
  exit 1
fi

# The ESP32's /dev/ttyACM* is root:dialout by default, which makes esptool and
# the backend fail with "Permission denied" for a normal user. Ask for sudo once
# and install a udev rule so the board works after every replug and reset.
if [ ! -r "$SERIAL_PORT" ] || [ ! -w "$SERIAL_PORT" ]; then
  echo "==> $SERIAL_PORT is not accessible to $USER; installing a udev rule (sudo required once)"
  sudo tee /etc/udev/rules.d/99-omniflux-esp32.rules >/dev/null <<'EOF'
# OmniFlux: allow non-root access to ESP32 USB serial ports.
KERNEL=="ttyACM[0-9]*", SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"
KERNEL=="ttyUSB[0-9]*", SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", MODE="0666"
EOF
  sudo udevadm control --reload-rules
  sudo udevadm trigger --subsystem-match=tty
  sleep 1
  # Fallback for this session if udev did not pick the rule up.
  if [ ! -r "$SERIAL_PORT" ] || [ ! -w "$SERIAL_PORT" ]; then
    sudo chmod a+rw "$SERIAL_PORT"
  fi
fi

for dir in backend frontend; do
  if [ ! -d "$dir/node_modules" ]; then
    echo "==> Installing $dir dependencies"
    (cd "$dir" && npm install --no-audit --no-fund)
  fi
done

echo "==> Building firmware"
"$PIO" run -d Frimware -e esp32_s3_touch_lcd_2

echo "==> Uploading firmware to $SERIAL_PORT"
if ! upload_log="$("$PIO" run -d Frimware -e esp32_s3_touch_lcd_2 -t upload --upload-port "$SERIAL_PORT" 2>&1)"; then
  printf '%s\n' "$upload_log"
  if grep -qi "permission denied" <<<"$upload_log"; then
    echo "==> Port access denied; retrying the upload with sudo"
    sudo env HOME="$HOME" PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}" \
      "$PIO_ABS" run -d Frimware -e esp32_s3_touch_lcd_2 -t upload --upload-port "$SERIAL_PORT"
  else
    exit 1
  fi
else
  printf '%s\n' "$upload_log"
fi

# The board re-enumerates after the upload reset; make sure the port stays readable.
for _ in $(seq 1 20); do
  if [ -r "$SERIAL_PORT" ] && [ -w "$SERIAL_PORT" ]; then
    break
  fi
  sudo chmod a+rw "$SERIAL_PORT" 2>/dev/null || true
  sleep 0.5
done

echo "==> Starting backend on http://localhost:3001"
SERIAL_PORT="$SERIAL_PORT" node backend/server.js &
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
