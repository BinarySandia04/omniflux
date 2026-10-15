#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32-S3 ready");
}

void loop() {
    Serial.println("Hello, nigga!");

    delay(1000);
}
