# Repository Guidelines

## Project Structure & Module Organization

This repository is a PlatformIO scaffold targeting `esp32-s3-devkitc-1` with the Arduino framework.

- `platformio.ini`: board, platform, framework, and future dependency configuration.
- `src/`: application firmware; currently empty. Add the Arduino entry point as `src/main.cpp`, with `setup()` and `loop()`.
- `include/`: shared project headers.
- `lib/`: private libraries, each in its own directory, such as `lib/sensor/src/`.
- `test/`: automated tests; currently contains only guidance.
- `.pio/`: generated build artifacts and dependencies.
- `get-platformio.py`: bundled PlatformIO installer; avoid unrelated changes to it.

## Build, Test, and Development Commands

Run commands from the repository root with PlatformIO Core installed:

- `pio run -e esp32-s3-devkitc-1`: compile firmware.
- `pio run -e esp32-s3-devkitc-1 -t upload`: build and flash a connected board.
- `pio device monitor -b 115200`: open the serial console; match the baud rate to the firmware's `Serial.begin(...)` setting.
- `pio test -e esp32-s3-devkitc-1`: run tests once suites exist; board execution requires connected hardware.
- `pio run -t clean`: remove build outputs.

The empty scaffold needs application code before a firmware build can succeed.

## Coding Style & Naming Conventions

No formatter, linter, or established application style is configured. For new C++ code, use four-space indentation, descriptive `snake_case` filenames, and consistent naming within each module. Use `.h` for headers and `.cpp` for implementations. Keep hardware initialization in `setup()` and recurring work in `loop()`; extract reusable logic into private libraries.

## Testing Guidelines

No test suites, explicit test framework, or coverage threshold are configured. Add PlatformIO suites under `test/test_<feature>/`, using descriptive test names. Cover logic changes and regressions; document board-dependent checks with wiring, expected serial output, and observed results.

## Commit & Pull Request Guidelines

Git history is unavailable in this workspace, so existing commit conventions cannot be verified. Use concise, imperative subjects such as `Add sensor initialization`. Keep commits focused.

Pull requests should explain the change, link relevant issues, and report build results and hardware checks. Include wiring changes or serial logs when relevant. Keep credentials out of source files and generated `.pio/` artifacts out of commits.
