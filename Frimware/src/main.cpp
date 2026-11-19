// OmniFlux — wrist extension / flexion recognition on the ESP32-S3.
// Runs continuously: every detected movement is classified and emitted as one
// JSON line on the USB serial port and over Bluetooth LE, e.g.
//   {"gesture":1,"name":"FLEXION","confidence":0.87}
// The gesture must be held through the 2 s capture window (that is how the
// training data was recorded); a quick swing back to neutral is ignored.
//
// The network runs in int8 (same maths as TFLite's reference kernel), exported by
// Train/export_int8.py from model_best_int8.tflite (the architecture-grid winner).
//
// The capture sketch (to record more dataset samples) is kept at the end, in `#if 0`.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdint.h>

#include <BLE2902.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "FastIMU.h"
#include "scaler.h"              // g_mean, g_scale, INPUT_DIM, NUM_CLASSES, GESTURE_CLASSES
#include "model_int8_weights.h"  // int8 weights, multipliers, quantisation params

static_assert(INPUT_DIM % MODEL_IN_DIM == 0, "model input does not divide the raw window");

// The grid-search model was trained on 4-frame average pooling (100 x 6 raw
// samples -> 25 x 6 model inputs).
constexpr int MODEL_POOL = INPUT_DIM / MODEL_IN_DIM;
static_assert(MODEL_POOL > 0, "model input is larger than the raw window");
static_assert(MODEL_IN_DIM % 6 == 0, "model input is not frame-major 6-axis data");
static_assert(NUM_CLASSES == MODEL_OUT_DIM, "model output size does not match classes");

// ---------- IMU (QMI8658, shares the I2C bus with the touch panel) ----------
#define IMU_ADDRESS 0x6B
#define PIN_IMU_SDA 48
#define PIN_IMU_SCL 47
QMI8658   IMU;
calData   calib = {0};
AccelData accelData;
GyroData  gyroData;

// ---------- Capture buffer: 100 samples x 6 channels = 600 features ----------
float sensor_data[INPUT_DIM];

// ================= Sensor capture =================
// 100 samples @ 50 Hz, order ax, ay, az, gx, gy, gz — matches the training CSVs.
void collectSensorData() {
  constexpr int kChannels = 6;

  for (int i = 0; i < INPUT_DIM / kChannels; i++) {
    IMU.update();
    IMU.getAccel(&accelData);
    IMU.getGyro(&gyroData);

    sensor_data[i * kChannels + 0] = accelData.accelX;
    sensor_data[i * kChannels + 1] = accelData.accelY;
    sensor_data[i * kChannels + 2] = accelData.accelZ;
    sensor_data[i * kChannels + 3] = gyroData.gyroX;
    sensor_data[i * kChannels + 4] = gyroData.gyroY;
    sensor_data[i * kChannels + 5] = gyroData.gyroZ;

    delay(20);   // 50 Hz — must match training
  }
}

// Compact per-channel min/max of the captured window. Printed before classifying
// so the live signal can be compared with the training CSVs.
void printWindowStats() {
  static const char *names[6] = {"ax", "ay", "az", "gx", "gy", "gz"};
  Serial.print("#WINDOW");
  for (int c = 0; c < 6; c++) {
    float lo = sensor_data[c], hi = sensor_data[c];
    for (int i = 1; i < INPUT_DIM / 6; i++) {
      const float v = sensor_data[i * 6 + c];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    Serial.printf(" %s[%.1f..%.1f]", names[c], lo, hi);
  }
  Serial.println();
}

// ================= Motion trigger =================
// The dataset windows start when the movement starts, so the classifier must do
// the same: the capture begins at the first acceleration change.
#define ACC_TRIGGER_G 0.20f
// No movement above ACC_TRIGGER_G for this long is reported as NO_MOVEMENT.
#define NO_MOVEMENT_MS 5000

bool waitForMotion(float *base_x, float *base_y, float *base_z, uint32_t timeout_ms = NO_MOVEMENT_MS) {
  float ax0 = 0, ay0 = 0, az0 = 0;
  int   n = 0;
  uint32_t t0 = millis();

  while (n < 20 && millis() - t0 < 1000) {
    IMU.update();
    IMU.getAccel(&accelData);
    ax0 += accelData.accelX;
    ay0 += accelData.accelY;
    az0 += accelData.accelZ;
    n++;
    delay(5);
  }
  if (n == 0) return false;
  ax0 /= n; ay0 /= n; az0 /= n;
  *base_x = ax0; *base_y = ay0; *base_z = az0;

  t0 = millis();
  while (millis() - t0 < timeout_ms) {
    IMU.update();
    IMU.getAccel(&accelData);
    float dx = fabsf(accelData.accelX - ax0)
             + fabsf(accelData.accelY - ay0)
             + fabsf(accelData.accelZ - az0);
    if (dx > ACC_TRIGGER_G) return true;
    delay(5);
  }
  return false;
}

// Idle after a report so the current pose is not captured twice; the next loop
// re-baselines and waits for a fresh movement before recording again.
#define REPORT_COOLDOWN_MS 500

// ================= Inference (int8) =================
// TFLite's reference fixed-point helpers.
static inline int32_t saturatingRoundingDoublingHighMul(int32_t a, int32_t b) {
  const bool overflow = (a == b) && (a == INT32_MIN);
  const int64_t ab = (int64_t)a * b;
  const int32_t nudge = ab >= 0 ? (1 << 30) : (1 - (1 << 30));
  const int32_t high = (int32_t)((ab + nudge) / (1LL << 31));
  return overflow ? INT32_MAX : high;
}

static inline int32_t roundingDivideByPOT(int32_t x, int exponent) {
  if (exponent == 0) return x;
  const int32_t mask = (1 << exponent) - 1;
  const int32_t remainder = x & mask;
  const int32_t threshold = (mask >> 1) + (x < 0 ? 1 : 0);
  return (x >> exponent) + (remainder > threshold ? 1 : 0);
}

static inline int32_t multiplyByQuantizedMultiplier(int32_t x, int32_t mult, int shift) {
  const int left = shift > 0 ? shift : 0;
  const int right = shift > 0 ? 0 : -shift;
  return roundingDivideByPOT(saturatingRoundingDoublingHighMul(x * (1 << left), mult), right);
}

static int8_t q_in[MODEL_IN_DIM];
static int8_t buf_a[MAX_UNITS];
static int8_t buf_b[MAX_UNITS];

// Fully connected int8 chain followed by softmax. Fills probs[].
static void predict(const float *x_norm, float *probs) {
  // Normalised 100 x 6 window -> average every MODEL_POOL frames per axis,
  // which is what the grid-search model was trained on.
  constexpr int kChannels = 6;
  for (int j = 0; j < MODEL_IN_DIM; j++) {
    const int frame = j / kChannels;
    const int channel = j % kChannels;
    float sum = 0;
    for (int k = 0; k < MODEL_POOL; k++)
      sum += x_norm[(frame * MODEL_POOL + k) * kChannels + channel];

    int q = (int)roundf(sum / (MODEL_POOL * IN_SCALE)) + IN_ZERO;
    q_in[j] = (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
  }

  const int8_t *in = q_in;
  int8_t *out = buf_a;

  for (int l = 0; l < NUM_DENSE; l++) {
    const int n_in = g_in_dim[l];
    const int n_out = g_out_dim[l];
    const int z_out = g_ozero[l];

    for (int o = 0; o < n_out; o++) {
      int32_t acc = g_b[l][o];
      const int8_t *w = &g_W[l][o * n_in];
      for (int i = 0; i < n_in; i++) acc += (int32_t)w[i] * in[i];

      int32_t v = multiplyByQuantizedMultiplier(acc, g_mult[l][o], g_shift[l][o]) + z_out;
      if (g_relu[l] && v < z_out) v = z_out;      // relu: 0 maps to the zero point
      out[o] = (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v));
    }

    in = out;
    out = (out == buf_a) ? buf_b : buf_a;
  }

  const int z_out = g_ozero[NUM_DENSE - 1];
  const float scale = g_oscale[NUM_DENSE - 1][0];
  float max_logit = -1e30f;
  for (int i = 0; i < MODEL_OUT_DIM; i++) {
    probs[i] = ((float)((int)in[i] - z_out)) * scale;
    if (probs[i] > max_logit) max_logit = probs[i];
  }
  float sum = 0;
  for (int i = 0; i < MODEL_OUT_DIM; i++) {
    probs[i] = expf(probs[i] - max_logit);
    sum += probs[i];
  }
  for (int i = 0; i < MODEL_OUT_DIM; i++) probs[i] /= sum;
}

// StandardScaler -> quantise -> network -> argmax. Returns the class.
int classify(float *confidence) {
  for (int i = 0; i < INPUT_DIM; i++)
    sensor_data[i] = (sensor_data[i] - g_mean[i]) / g_scale[i];

  float probs[NUM_CLASSES];
  predict(sensor_data, probs);

  int cls = 0;
  for (int i = 1; i < NUM_CLASSES; i++)
    if (probs[i] > probs[cls]) cls = i;
  *confidence = probs[cls];
  return cls;
}

// ================= Bluetooth (BLE) =================
// Same JSON lines as the USB serial port, notified to a BLE client (the backend
// connects with TRANSPORT=ble). Nordic UART service UUIDs, so any generic BLE
// UART app can read the stream too.
// Each board advertises a numbered name so the backend can tell the players
// apart; start.sh flashes player1/player2 with different PLAYER_ID values.
#ifndef PLAYER_ID
#define PLAYER_ID 1
#endif
#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)
#define BLE_NAME      "OmniFlux-" STRINGIFY(PLAYER_ID)
#define BLE_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

static BLECharacteristic *ble_tx = nullptr;
static volatile bool ble_connected = false;
static volatile bool ble_restart_advertising = false;

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override { ble_connected = true; }
  void onDisconnect(BLEServer *server) override {
    ble_connected = false;
    ble_restart_advertising = true;   // restarting from the loop is more reliable
  }
};

void bleBegin() {
  BLEDevice::init(BLE_NAME);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new BleServerCallbacks());

  BLEService *service = server->createService(BLE_SERVICE_UUID);
  ble_tx = service->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  ble_tx->addDescriptor(new BLE2902());
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
  Serial.println("BLE: advertising as " BLE_NAME);
}

// Write one JSON line to USB serial and to any connected BLE client. The
// default 23-byte ATT MTU leaves 20 bytes of payload and Web Bluetooth cannot
// request a larger one, so split the line; the client reassembles on '\n'.
void emit(const char *line) {
  Serial.println(line);
  if (!ble_connected || ble_tx == nullptr) return;

  const String framed = String(line) + "\n";
  for (size_t i = 0; i < framed.length(); i += 20) {
    const size_t remaining = framed.length() - i;
    const size_t n = remaining < 20 ? remaining : 20;
    ble_tx->setValue((const uint8_t *)framed.c_str() + i, n);
    ble_tx->notify();
    delay(20);   // pace notifications so a slow client does not drop them
  }
}

// ================= Setup / loop =================
void setup() {
  Serial.begin(115200);
  // When the dashboard reads over BLE, nothing consumes USB serial. Without
  // this, a full CDC buffer blocks Serial writes for ~2 s and stalls the loop,
  // which delays (or appears to drop) every BLE notification. Drop debug
  // output instead.
  Serial.setTxTimeoutMs(0);
  bleBegin();

  Wire.begin(PIN_IMU_SDA, PIN_IMU_SCL);   // I2C for the QMI8658
  Wire.setClock(400000);

  if (IMU.init(calib, IMU_ADDRESS) != 0) {
    Serial.println("IMU init failed");
    while (1) {}
  }
  Serial.println("Keep IMU still for calibration...");
  delay(2000);
  IMU.calibrateAccelGyro(&calib);

  // calibrateAccelGyro() leaves the chip at +-2g / +-1024dps while the library
  // still scales with +-16g / +-2000dps, and it does not apply the biases.
  // Re-init to restore the ranges and load the calibration (same as the logger).
  if (IMU.init(calib, IMU_ADDRESS) != 0) {
    Serial.println("IMU re-init failed");
    while (1) {}
  }

  // Sanity check: at rest the accelerometer must read ~1 g.
  IMU.update();
  IMU.getAccel(&accelData);
  const float gravity = sqrtf(accelData.accelX * accelData.accelX +
                              accelData.accelY * accelData.accelY +
                              accelData.accelZ * accelData.accelZ);
  Serial.printf("accel at rest: ax=%.2f ay=%.2f az=%.2f | |a|=%.2f g (expect ~1.00)\n",
                accelData.accelX, accelData.accelY, accelData.accelZ, gravity);

  Serial.printf("Ready: %d classes, %d raw features -> %d model inputs (%d-frame avg), int8 %d layers. Move and hold the gesture ~2 s\n",
                NUM_CLASSES, INPUT_DIM, MODEL_IN_DIM, MODEL_POOL, NUM_DENSE);
  Serial.printf("classes: 0=%s, 1=%s\n", GESTURE_CLASSES[0], GESTURE_CLASSES[1]);
}

void loop() {
  if (ble_restart_advertising) {
    ble_restart_advertising = false;
    BLEDevice::startAdvertising();
  }

  // Continuous: wait for movement, classify the window, emit one JSON line.
  // The idle event is printed once per idle period, not every timeout.
  static bool idleReported = false;
  float base_x = 0, base_y = 0, base_z = 0;
  if (!waitForMotion(&base_x, &base_y, &base_z)) {
    if (!idleReported) {
      emit("{\"player\":" STRINGIFY(PLAYER_ID) ",\"gesture\":-1,\"name\":\"NO_MOVEMENT\",\"confidence\":0}");
      idleReported = true;
    }
    return;
  }
  idleReported = false;

  // Alert the frontend before the 2 s window so it can show "recording".
  emit("{\"player\":" STRINGIFY(PLAYER_ID) ",\"gesture\":-2,\"name\":\"MOVING\",\"confidence\":0}");

  collectSensorData();

  // The model was trained on the gesture held for the whole 2 s window, and it
  // classifies rest as flexion. So ignore windows that end back at rest (a quick
  // swing), and only report gestures that are still held when capture finishes.
  constexpr int kSamples = INPUT_DIM / 6;
  float end_x = 0, end_y = 0, end_z = 0;
  for (int i = kSamples - 10; i < kSamples; i++) {
    end_x += sensor_data[i * 6 + 0];
    end_y += sensor_data[i * 6 + 1];
    end_z += sensor_data[i * 6 + 2];
  }
  end_x /= 10; end_y /= 10; end_z /= 10;

  const float held = fabsf(end_x - base_x) + fabsf(end_y - base_y) + fabsf(end_z - base_z);
  if (held < 0.5f) {
    // Quick swing back to neutral: no gesture, but still close the recording state.
    emit("{\"player\":" STRINGIFY(PLAYER_ID) ",\"gesture\":-1,\"name\":\"NO_MOVEMENT\",\"confidence\":0}");
    idleReported = true;
    delay(REPORT_COOLDOWN_MS);
    return;
  }

  printWindowStats();

  float conf = 0;
  const int cls = classify(&conf);
  char line[112];
  snprintf(line, sizeof(line), "{\"player\":%d,\"gesture\":%d,\"name\":\"%s\",\"confidence\":%.2f}",
           PLAYER_ID, cls, GESTURE_CLASSES[cls], conf);
  emit(line);

  delay(REPORT_COOLDOWN_MS);
}

// =====================================================================
// MODO CAPTURA (desactivado: cambia #if 0 por #if 1 y comenta el
// clasificador de arriba para volver a grabar muestras del dataset)
// =====================================================================
#if 0
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "FastIMU.h"

#define IMU_ADDRESS 0x6B    // Change to the address of the IMU
#define PERFORM_CALIBRATION // Comment to disable startup calibration

QMI8658 IMU;                // Change to the name of any supported IMU!

calData   calib = {0};      // Calibration data
AccelData accelData;        // Sensor data
GyroData  gyroData;
MagData   magData;

// ===== Gesture logging config =====
static const uint16_t NUM_SAMPLES        = 100;   // ~2 s @ 50 Hz
static const uint16_t SAMPLE_PERIOD_MS   = 20;    // 50 Hz
static const float    ACC_TRIGGER_G      = 0.20f; // motion start threshold
static const uint16_t NUM_REPS_PER_CLASS = 30;

static volatile uint8_t current_label     = 1;    // 1 or 2
static volatile bool    request_one_burst = false;
static volatile bool    request_30_reps   = false;
static bool             is_capturing      = false;

// --- tiny helpers ---
void print_help() {
  Serial.println(F("\n=== Gesture Logger (Serial) ==="));
  Serial.println(F("1 -> set label to 1"));
  Serial.println(F("2 -> set label to 2"));
  Serial.println(F("b -> record ONE burst"));
  Serial.println(F("r -> record 30 bursts"));
  Serial.println(F("h -> help"));
}

// Wait until acceleration deviates from a short baseline by ACC_TRIGGER_G
bool wait_for_motion(uint32_t timeout_ms = 10000) {
  // Baseline
  float ax0 = 0, ay0 = 0, az0 = 0;
  int   n   = 0;
  uint32_t t0 = millis();

  while (n < 20 && millis() - t0 < 1000) {
    IMU.update();
    IMU.getAccel(&accelData);
    ax0 += accelData.accelX;
    ay0 += accelData.accelY;
    az0 += accelData.accelZ;
    n++;
    delay(5);
  }
  if (n == 0) return false;
  ax0 /= n; ay0 /= n; az0 /= n;

  // Wait for deviation
  t0 = millis();
  while (millis() - t0 < timeout_ms) {
    IMU.update();
    IMU.getAccel(&accelData);
    float dx = fabsf(accelData.accelX - ax0)
             + fabsf(accelData.accelY - ay0)
             + fabsf(accelData.accelZ - az0);
    if (dx > ACC_TRIGGER_G) return true;
    delay(5);
  }
  return false;
}

// Capture one window and print CSV lines
bool capture_burst_stream(uint8_t label, uint16_t rep) {
  if (!wait_for_motion(10000)) {
    Serial.println("# Timeout waiting motion");
    return false;
  }

  uint32_t t_start = millis();
  for (uint16_t i = 0; i < NUM_SAMPLES; i++) {
    IMU.update();
    IMU.getAccel(&accelData);
    IMU.getGyro(&gyroData);

    uint32_t t_ms = millis() - t_start;

    // CSV: label,rep,idx,t_ms,ax,ay,az,gx,gy,gz
    Serial.printf("%u,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                  label, rep, i, t_ms,
                  accelData.accelX, accelData.accelY, accelData.accelZ,
                  gyroData.gyroX,   gyroData.gyroY,   gyroData.gyroZ);

    // Pace to 50 Hz
    uint32_t t_next = t_start + (i + 1) * SAMPLE_PERIOD_MS;
    int32_t  dt     = (int32_t)t_next - (int32_t)millis();
    if (dt > 0) delay(dt);
  }
  return true;
}

void setup() {
  Wire.begin(48, 47);
  Wire.setClock(400000); // 400 kHz clock
  Serial.begin(115200);
  while (!Serial) { ; }

  int err = IMU.init(calib, IMU_ADDRESS);
  if (err != 0) {
    Serial.print("Error initializing IMU: ");
    Serial.println(err);
    while (true) { ; }
  }

#ifdef PERFORM_CALIBRATION
  Serial.println("FastIMU calibration & data example");
  if (IMU.hasMagnetometer()) {
    delay(1000);
    Serial.println("Move IMU in figure 8 pattern until done.");
    delay(3000);
    IMU.calibrateMag(&calib);
    Serial.println("Magnetic calibration done!");
  }
  else {
    delay(5000);
  }

  delay(5000);
  Serial.println("Keep IMU level.");
  delay(5000);
  IMU.calibrateAccelGyro(&calib);
  Serial.println("Calibration done!");
  Serial.println("Accel biases X/Y/Z: ");
  Serial.print(calib.accelBias[0]); Serial.print(", ");
  Serial.print(calib.accelBias[1]); Serial.print(", ");
  Serial.println(calib.accelBias[2]);
  Serial.println("Gyro biases X/Y/Z: ");
  Serial.print(calib.gyroBias[0]); Serial.print(", ");
  Serial.print(calib.gyroBias[1]); Serial.print(", ");
  Serial.println(calib.gyroBias[2]);
  if (IMU.hasMagnetometer()) {
    Serial.println("Mag biases X/Y/Z: ");
    Serial.print(calib.magBias[0]); Serial.print(", ");
    Serial.print(calib.magBias[1]); Serial.print(", ");
    Serial.println(calib.magBias[2]);
    Serial.println("Mag Scale X/Y/Z: ");
    Serial.print(calib.magScale[0]); Serial.print(", ");
    Serial.print(calib.magScale[1]); Serial.print(", ");
    Serial.println(calib.magScale[2]);
  }
  delay(5000);
  IMU.init(calib, IMU_ADDRESS);
#endif

  // err = IMU.setGyroRange(500);
  // err = IMU.setAccelRange(2);
  if (err != 0) {
    Serial.print("Error Setting range: ");
    Serial.println(err);
    while (true) { ; }
  }

  print_help();
}

void loop() {
  // --- Minimal serial UI for gesture logging ---
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '1') { current_label = 1; Serial.println("# Label=1"); }
    else if (c == '2') { current_label = 2; Serial.println("# Label=2"); }
    else if (c == 'b') { request_one_burst = true; }
    else if (c == 'r') { request_30_reps   = true; }
    else if (c == 'h' || c == '?') { print_help(); }
  }

  // --- Record ONE burst (CSV) ---
  if (request_one_burst && !is_capturing) {
    is_capturing = true;
    request_one_burst = false;

    Serial.println("###CSV_BEGIN");
    Serial.println("label,rep,idx,t_ms,ax,ay,az,gx,gy,gz");
    bool ok = capture_burst_stream(current_label, 0);
    Serial.println("###CSV_END");
    if (!ok) Serial.println("# No motion / failed");

    is_capturing = false;
  }

  // --- Record 30 bursts (CSV) ---
  if (request_30_reps && !is_capturing) {
    is_capturing = true;
    request_30_reps = false;

    Serial.println("###CSV_BEGIN");
    Serial.println("label,rep,idx,t_ms,ax,ay,az,gx,gy,gz");

    for (uint16_t rep = 1; rep <= NUM_REPS_PER_CLASS; rep++) {
      if (!capture_burst_stream(current_label, rep)) {
        Serial.println("# Timeout. Re-run 'r' when ready.");
        break;
      }
      delay(300);
    }

    Serial.println("###CSV_END");
    is_capturing = false;
  }
}
#endif
