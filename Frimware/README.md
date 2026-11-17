# Omniflux Firmware

PlatformIO firmware for the ESP32-S3 Touch LCD 2 (Arduino framework).

## Running PlatformIO

Run commands from the repository root:

```bash
pio run -e esp32_s3_touch_lcd_2                 # compile
pio run -e esp32_s3_touch_lcd_2 -t upload       # flash a connected board
pio device monitor -b 115200                    # serial console
pio test -e esp32_s3_touch_lcd_2                # run tests
pio run -t clean                                # clean build outputs
```

If `pio` is not on your PATH, install PlatformIO Core (`pipx install platformio` or `pip install platformio`) or run the bundled installer:

```bash
python get-platformio.py
```

## Layout

- `src/`: application firmware (`main.cpp` with `setup()` and `loop()`).
- `include/`: shared project headers.
- `lib/`: private libraries.
- `test/`: automated tests.
- `platformio.ini`: board, framework, and dependencies.
