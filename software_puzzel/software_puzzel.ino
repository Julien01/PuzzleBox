const int outPins[6] = {4, 13, 18, 23, 25, 26};
const int inPins[6]  = {5, 14, 19, 27, 32, 33};

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 6; i++) {
    pinMode(outPins[i], OUTPUT);
    pinMode(inPins[i], INPUT_PULLUP);
    digitalWrite(outPins[i], HIGH); // keep OFF state
  }
}

bool checkPair(int i) {
  // turn OFF all outputs first
  for (int j = 0; j < 6; j++) {
    digitalWrite(outPins[j], HIGH);
  }

  delay(5);

  // activate only ONE output
  digitalWrite(outPins[i], LOW);

  delay(5); // settle time

  int state = digitalRead(inPins[i]);

  // restore
  digitalWrite(outPins[i], HIGH);

  return (state == LOW);
}

void loop() {
  bool allComplete = true;

  for (int i = 0; i < 6; i++) {

    if (checkPair(i)) {
      Serial.print("Pair ");
      Serial.print(i);
      Serial.println(" OK");
    } else {
      Serial.print("Pair ");
      Serial.print(i);
      Serial.println(" WRONG");
      allComplete = false;
    }
  }

  if (allComplete) {
    Serial.println("VICTORY!");
  }

  Serial.println("------");
  delay(300);
}