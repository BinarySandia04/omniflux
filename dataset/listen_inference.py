import json
import serial

# === User config ===
PORT = "/dev/ttyACM1"  # <-- change to your ESP32 port
BAUD = 115200          # must match Serial.begin(115200) in the firmware

ser = serial.Serial(PORT, BAUD, timeout=1)

# The firmware streams one JSON line per detected gesture; other lines are debug.
for raw in ser:
    line = raw.decode(errors="ignore").strip()
    if line.startswith("{"):
        print(json.loads(line), flush=True)
