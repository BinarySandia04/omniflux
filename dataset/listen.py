import serial
import time
import os
from datetime import datetime

# === User config ===
PORT = "/dev/ttyACM0"  # <-- change to your ESP32 port (check: ls /dev/ttyUSB* /dev/ttyACM*)
BAUD = 115200          # must match the firmware's Serial.begin(115200)
OUTDIR = "captures"    # directory for saved CSVs
ECHO_DATA = True       # True = print every CSV line; False = only show progress
LABEL = 1              # <-- gesture ID (0..11) captured this run; edit before each gesture

def make_filename(label):
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    subdir = os.path.join(OUTDIR, f"label_{label}")
    os.makedirs(subdir, exist_ok=True)
    return os.path.join(subdir, f"gesture_{ts}.csv")

def wait_and_save(ser, label):
    """Listen for one CSV block and save to file"""
    recording = False
    f = None
    filename = make_filename(label)

    print(f"[INFO] Waiting for CSV from ESP32 (label {label})...")

    current_rep = None
    line_count = 0

    while True:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode(errors="ignore").strip()
        if not line:
            continue

        if line.startswith("###CSV_BEGIN"):
            print(f"[INFO] Detected start of CSV for label {label}")
            f = open(filename, "w", encoding="utf-8")
            recording = True
            line_count = 0
            continue

        if line.startswith("###CSV_END"):
            if f:
                f.close()
                print(f"[SUCCESS] Saved file: {filename}")
                f = None
            recording = False
            break  # stop after one capture

        if recording and f:
            # stamp the gesture ID from LABEL into the CSV label column
            head, _, rest = line.partition(",")
            if head.isdigit():
                line = f"{label},{rest}"
            f.write(line + "\n")
            line_count += 1

            # Try to parse rep from CSV
            parts = line.split(",")
            if len(parts) >= 3:
                try:
                    rep_val = int(parts[1])
                    if rep_val != current_rep:
                        current_rep = rep_val
                        print(f"  [REP {rep_val}]")
                except ValueError:
                    pass  # skip header line

            # Echo data line if enabled
            if ECHO_DATA:
                print("   " + line)

def main():
    print("=== ESP32 Gesture Data Capture (Verbose) ===")
    print(f"Opening port {PORT} at {BAUD} baud...")
    ser = serial.Serial(PORT, BAUD, timeout=1)
    time.sleep(2)  # give board time to reset
    ser.reset_input_buffer()

    os.makedirs(OUTDIR, exist_ok=True)

    print(f"\n[CAPTURE] Label = {LABEL} (1 rep)...")
    ser.write(b"b")
    wait_and_save(ser, label=LABEL)

    print("\n=== Capture complete! Files are saved in:", OUTDIR, "===\n")

if __name__ == "__main__":
    main()
