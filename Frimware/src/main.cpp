// OmniFlux — gesture recognition on ESP32-S3-Touch-LCD-2
// Captures 100 IMU samples (50 Hz), normalizes with the training StandardScaler,
// runs the embedded TFLite model, prints the result over serial and shows it on the LCD.
//
// Setup:
//   1. Generate model_data.h:  cd Train && python export_model.py
//   2. Build & flash:          pio run -e esp32_s3_touch_lcd_2 -t upload
//   3. Use: send 'b' over serial (or tap the screen) to classify one gesture.
//      'h' = help.

#include <Arduino.h>
#include <Wire.h>
#include "FastIMU.h"

#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "bsp_cst816.h"

#include <TensorFlowLite_ESP32.h>
#include "model_data.h"

// ---------- IMU ----------
#define IMU_ADDRESS          0x6B
#define PERFORM_CALIBRATION  // comment to skip startup accel/gyro calibration
QMI8658 IMU;
calData   calib = {0};
AccelData accelData;
GyroData  gyroData;

// ---------- LCD (ST7789, SPI) ----------
#define PIN_LCD_SCLK 39
#define PIN_LCD_MOSI 38
#define PIN_LCD_MISO 40
#define PIN_LCD_DC   42
#define PIN_LCD_RST  -1
#define PIN_LCD_CS   45
#define PIN_LCD_BL   1
#define PIN_TP_SDA   48
#define PIN_TP_SCL   47

#define LCD_ROTATION 0
#define LCD_H_RES    240
#define LCD_V_RES    320
#define LEDC_FREQ    5000
#define LEDC_TIMER_10_BIT 10

// ---------- Sampling (must match training: 100 samples @ 50 Hz) ----------
#define NUM_SAMPLES      100
#define SAMPLE_PERIOD_MS 20
#define N_CH             6
#define FEATURES         (NUM_SAMPLES * N_CH)   // 600
#define ACC_TRIGGER_G    0.20f

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, PIN_LCD_RST, LCD_ROTATION, true /*IPS*/, LCD_H_RES, LCD_V_RES);

// ---------- LVGL ----------
static lv_color_t       *disp_draw_buf;
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t      disp_drv;
static lv_obj_t *label_status  = nullptr;
static lv_obj_t *label_gesture = nullptr;

// ---------- TFLite Micro ----------
static tflite::MicroMutableOpResolver<8> resolver;
static const tflite::Model   *tfl_model   = nullptr;
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor *input  = nullptr;
static TfLiteTensor *output = nullptr;
static constexpr int   kTensorArenaSize = 512 * 1024;
static uint8_t        *tensor_arena = nullptr;

static float    g_raw[FEATURES];
static volatile bool g_touch_pressed = false;

// ================= LVGL glue =================
void my_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  (void)area; (void)color_p;
  lv_disp_flush_ready(drv);  // direct_mode: drawn straight into the framebuffer
}

void my_touchpad_read(lv_indev_drv_t *indev, lv_indev_data_t *data) {
  uint16_t x, y;
  bsp_touch_read();
  if (bsp_touch_get_coordinates(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PRESSED;
    g_touch_pressed = true;
  } else {
    data->state   = LV_INDEV_STATE_RELEASED;
    g_touch_pressed = false;
  }
}

void lvgl_tick() {
  lv_timer_handler();
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(0, 0, (uint16_t *)disp_draw_buf, gfx->width(), gfx->height());
#else
  gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)disp_draw_buf, gfx->width(), gfx->height());
#endif
}

void ui_init() {
  lv_obj_t *scr = lv_scr_act();

  label_status = lv_label_create(scr);
  lv_label_set_text(label_status, "OmniFlux - Listo");
  lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 20);

  label_gesture = lv_label_create(scr);
  lv_label_set_text(label_gesture, "Pulsa 'b' o\ntoca la pantalla");
  lv_obj_align(label_gesture, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_width(label_gesture, LCD_H_RES - 20);
  lv_label_set_long_mode(label_gesture, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(label_gesture, LV_TEXT_ALIGN_CENTER, 0);
}

// ================= IMU capture =================
bool wait_for_motion(uint32_t timeout_ms = 10000) {
  float ax0 = 0, ay0 = 0, az0 = 0;
  int   n = 0;
  uint32_t t0 = millis();
  while (n < 20 && millis() - t0 < 1000) {
    IMU.update(); IMU.getAccel(&accelData);
    ax0 += accelData.accelX; ay0 += accelData.accelY; az0 += accelData.accelZ;
    n++;
    delay(5);
  }
  if (n == 0) return false;
  ax0 /= n; ay0 /= n; az0 /= n;

  t0 = millis();
  while (millis() - t0 < timeout_ms) {
    IMU.update(); IMU.getAccel(&accelData);
    float dx = fabsf(accelData.accelX - ax0)
             + fabsf(accelData.accelY - ay0)
             + fabsf(accelData.accelZ - az0);
    if (dx > ACC_TRIGGER_G) return true;
    delay(5);
  }
  return false;
}

// Fills raw[600] in the same order the notebook used (per sample: ax,ay,az,gx,gy,gz).
bool capture(float *raw) {
  if (!wait_for_motion(10000)) return false;

  uint32_t t0 = millis();
  for (int i = 0; i < NUM_SAMPLES; i++) {
    IMU.update();
    IMU.getAccel(&accelData);
    IMU.getGyro(&gyroData);
    int k = i * N_CH;
    raw[k + 0] = accelData.accelX;
    raw[k + 1] = accelData.accelY;
    raw[k + 2] = accelData.accelZ;
    raw[k + 3] = gyroData.gyroX;
    raw[k + 4] = gyroData.gyroY;
    raw[k + 5] = gyroData.gyroZ;

    uint32_t t_next = t0 + (i + 1) * SAMPLE_PERIOD_MS;
    int32_t  dt     = (int32_t)t_next - (int32_t)millis();
    if (dt > 0) delay(dt);
  }
  return true;
}

// ================= Inference =================
int classify(float *raw, float *confidence) {
  for (int i = 0; i < FEATURES; i++)
    input->data.f[i] = (raw[i] - g_mean[i]) / g_scale[i];  // StandardScaler on-device

  if (interpreter->Invoke() != kTfLiteOk) return -1;

  int best = 0;
  for (int c = 1; c < NUM_CLASSES; c++)
    if (output->data.f[c] > output->data.f[best]) best = c;
  *confidence = output->data.f[best];
  return best;
}

// ================= Setup / loop =================
void setup() {
  Serial.begin(115200);

  // Display
  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  gfx->fillScreen(BLACK);
  ledcAttach(PIN_LCD_BL, LEDC_FREQ, LEDC_TIMER_10_BIT);
  ledcWrite(PIN_LCD_BL, (1 << LEDC_TIMER_10_BIT) / 100 * 80);

  // I2C bus shared by touch (0x15) and IMU (0x6B)
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL);
  Wire.setClock(400000);
  bsp_touch_init(&Wire, gfx->getRotation(), gfx->width(), gfx->height());

  // IMU
  int err = IMU.init(calib, IMU_ADDRESS);
  if (err != 0) { Serial.printf("IMU init error %d\n", err); while (1) {} }
#ifdef PERFORM_CALIBRATION
  Serial.println("Keep IMU still for calibration...");
  delay(3000);
  IMU.calibrateAccelGyro(&calib);
  delay(100);
  IMU.init(calib, IMU_ADDRESS);
  Serial.println("Calibration done");
#endif

  // LVGL
  lv_init();
  uint32_t buf_px = (uint32_t)gfx->width() * gfx->height();
  disp_draw_buf = (lv_color_t *)heap_caps_malloc(buf_px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!disp_draw_buf)
    disp_draw_buf = (lv_color_t *)heap_caps_malloc(buf_px * 2, MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, buf_px);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res   = gfx->width();
  disp_drv.ver_res   = gfx->height();
  disp_drv.flush_cb  = my_disp_flush;
  disp_drv.draw_buf  = &draw_buf;
  disp_drv.direct_mode = true;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  ui_init();

  // TFLite Micro (arena in PSRAM: float32 model ~350 KB doesn't fit internal SRAM)
  tensor_arena = (uint8_t *)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM);
  if (!tensor_arena)
    tensor_arena = (uint8_t *)malloc(kTensorArenaSize);

  resolver.AddFullyConnected();
  resolver.AddRelu();
  resolver.AddSoftmax();
  resolver.AddReshape();

  tfl_model = tflite::GetModel(g_model);
  if (tfl_model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Model schema version mismatch!"); while (1) {}
  }
  interpreter = new tflite::MicroInterpreter(tfl_model, resolver, tensor_arena, kTensorArenaSize);
  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("AllocateTensors failed!"); while (1) {}
  }
  input  = interpreter->input(0);
  output = interpreter->output(0);

  Serial.printf("Ready: %d classes, input tensor %u bytes\n", NUM_CLASSES, (unsigned)input->bytes);
  Serial.println("b = clasificar gesto | toque = clasificar | h = ayuda");
  lvgl_tick();
}

void loop() {
  char c = 0;
  if (Serial.available()) c = Serial.read();

  // Rising edge on touch = tap
  static bool prev_pressed = false;
  bool tap = g_touch_pressed && !prev_pressed;
  prev_pressed = g_touch_pressed;

  if (c == 'b' || tap) {
    lv_label_set_text(label_gesture, "Capturando...");
    lvgl_tick();

    if (capture(g_raw)) {
      float conf = 0;
      int cls = classify(g_raw, &conf);
      if (cls < 0) {
        Serial.println("#RESULT error");
        lv_label_set_text(label_gesture, "Error");
      } else {
        Serial.printf("#RESULT %d %s %.2f\n", cls, GESTURE_CLASSES[cls], conf * 100.0f);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s\n%.0f%%", GESTURE_CLASSES[cls], conf * 100.0f);
        lv_label_set_text(label_gesture, buf);
      }
    } else {
      Serial.println("#RESULT none (no motion)");
      lv_label_set_text(label_gesture, "Sin movimiento");
    }
    lvgl_tick();
  } else if (c == 'h' || c == '?') {
    Serial.println("b = clasificar gesto | toque = clasificar | h = ayuda");
  }

  lvgl_tick();
  delay(5);
}
