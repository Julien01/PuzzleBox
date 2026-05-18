#include <Wire.h>



#define SLAVE_ADDR 9

volatile bool startAck = false;

const int outPins[6] = {4, 13, 18, 23, 25, 26};
const int inPins[6]  = {5, 14, 19, 27, 32, 33};

volatile int lastReceived = 0;

volatile bool puzzleStarted = false;
volatile bool victory = false;

void receiveEvent(int bytes) {
  while (Wire.available()) {
    lastReceived = Wire.read();

    Serial.print("Received: ");
    Serial.println(lastReceived);

    if (lastReceived == 48) {
      puzzleStarted = true;   // start game
    }
  }
}

void requestEvent() {
  if (!puzzleStarted) {
    Wire.write(0);   // not started
  }
  else if (victory) {
    Wire.write(2);   // victory
  }
  else {
    Wire.write(1);   // running
  }
}

void setup() {
  Serial.begin(115200);

  Wire.begin(SLAVE_ADDR);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  for (int i = 0; i < 6; i++) {
    pinMode(outPins[i], OUTPUT);
    pinMode(inPins[i], INPUT_PULLUP);
    digitalWrite(outPins[i], HIGH);
  }

  Serial.println("SLAVE ready");
}

bool checkPair(int i) {
  for (int j = 0; j < 6; j++) {
    digitalWrite(outPins[j], HIGH);
  }

  delay(5);

  digitalWrite(outPins[i], LOW);
  delay(5);

  int state = digitalRead(inPins[i]);

  digitalWrite(outPins[i], HIGH);

  return (state == LOW);
}

void loop() {
  if (!puzzleStarted || victory) {
    delay(200);
    return;
  }

  bool allComplete = true;

  for (int i = 0; i < 6; i++) {
    if (!checkPair(i)) {
      allComplete = false;
    }
  }

  if (allComplete) {
    Serial.println("VICTORY!");
    victory = true;
  }

  delay(200);
}