#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22); // SDA, SCL

  Serial.println("ESP32 ready");
}

void loop() {
  Serial.println("Sending...");

  Wire.beginTransmission(8);  // Leonardo address
  Wire.write(42);
  byte err = Wire.endTransmission();

  if (err == 0) {
    Serial.println("OK");
  } else {
    Serial.print("Error: ");
    Serial.println(err);
  }

  delay(1000);
}