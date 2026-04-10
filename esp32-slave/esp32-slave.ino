#include <Wire.h>

#define SLAVE_ADDR 8
#define SDA_PIN 21
#define SCL_PIN 22

void receiveEvent(int bytes) {
  Serial.print("Received: ");

  while (Wire.available()) {
    int value = Wire.read();
    Serial.print(value);
    Serial.print(" ");
  }

  Serial.println();
}

void setup() {
  Serial.begin(115200);

  // Explicitly define pins + slave address
Wire.begin(SLAVE_ADDR);  // ONLY address, no pins

  Wire.onReceive(receiveEvent);

  Serial.println("ESP32 Slave ready");
}

void loop() {
  delay(1000);
}