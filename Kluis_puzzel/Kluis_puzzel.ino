#include <Wire.h>
#include <TM1637Display.h>

// -------------------------
// I2C CONFIG
// -------------------------
#define SLAVE_ADDR 11
#define START_CODE 60

#define SDA_PIN 26
#define SCL_PIN 27

volatile int lastReceived = 0;
volatile bool puzzleStarted = false;
volatile bool victory = false;

// -------------------------
// PINNEN
// -------------------------

// Keypad kolommen = INPUTS
// Volgens jouw schema:
// Kolom_1 = GPIO34
// Kolom_2 = GPIO35
// Kolom_3 = GPIO32
const byte colPins[3] = {34, 35, 32};

// Keypad rijen = OUTPUTS
// Volgens jouw schema:
// Rij_1 = GPIO25
// Rij_2 = GPIO21
// Rij_3 = GPIO22
// Rij_4 = GPIO23
const byte rowPins[4] = {25, 21, 22, 23};

const byte TM_CLK = 18;
const byte TM_DIO = 19;

// Volgens jouw schema: Kluis_slot = GPIO33
const byte SOLENOID_PIN = 33;

// Solenoid maximaal 2 seconden open
const unsigned long SOLENOID_OPEN_TIME = 2000;

bool solenoidActive = false;
unsigned long solenoidStartTime = 0;

TM1637Display display(TM_CLK, TM_DIO);

// -------------------------
// KEYPAD
// -------------------------
char keyMap[4][3] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

// -------------------------
// 9 DIAGRAMMEN
// -------------------------
struct Diagram {
  char cols[3];
  char rows[4];
};

Diagram diagrams[9] = {
  {{'A','C','E'}, {'1','2','3','4'}},
  {{'F','H','L'}, {'8','3','9','4'}},
  {{'P','U','A'}, {'2','7','0','8'}},
  {{'C','H','U'}, {'0','4','2','3'}},
  {{'E','L','A'}, {'9','0','8','6'}},
  {{'A','F','P'}, {'1','3','2','6'}},
  {{'P','H','E'}, {'5','3','4','0'}},
  {{'U','C','A'}, {'1','2','6','7'}},
  {{'H','P','L'}, {'5','3','8','9'}}
};

// -------------------------
// SPELSTATUS
// -------------------------
const byte MAX_LEVEL = 5;
byte level = 1;
bool gameFinished = false;

byte currentDiagram = 0;
byte currentRow = 0;
byte currentCol = 0;
char expectedKey = '\0';

bool startMessagePending = false;

// -------------------------
// SOLENOID FUNCTIES
// -------------------------
void stopSolenoid() {
  digitalWrite(SOLENOID_PIN, LOW);
  solenoidActive = false;
}

void startSolenoid() {
  if (solenoidActive) {
    return;
  }

  digitalWrite(SOLENOID_PIN, HIGH);
  solenoidActive = true;
  solenoidStartTime = millis();

  Serial.println("Solenoid AAN. Kluis open voor maximaal 2 seconden.");
}

void updateSolenoid() {
  if (!solenoidActive) {
    return;
  }

  if (millis() - solenoidStartTime >= SOLENOID_OPEN_TIME) {
    stopSolenoid();
    Serial.println("Solenoid UIT. Kluis weer dicht.");
  }
}

// -------------------------
// DISPLAY HULPFUNCTIES
// -------------------------
uint8_t encodeChar(char c) {
  switch (c) {
    case '0': return display.encodeDigit(0);
    case '1': return display.encodeDigit(1);
    case '2': return display.encodeDigit(2);
    case '3': return display.encodeDigit(3);
    case '4': return display.encodeDigit(4);
    case '5': return display.encodeDigit(5);
    case '6': return display.encodeDigit(6);
    case '7': return display.encodeDigit(7);
    case '8': return display.encodeDigit(8);
    case '9': return display.encodeDigit(9);

    case 'A': return 0x77;
    case 'C': return 0x39;
    case 'E': return 0x79;
    case 'F': return 0x71;
    case 'H': return 0x76;
    case 'L': return 0x38;
    case 'P': return 0x73;
    case 'U': return 0x3E;

    case '-': return 0x40;
    case ' ': return 0x00;
    default:  return 0x00;
  }
}

void show4Chars(char a, char b, char c, char d) {
  uint8_t segs[4] = {
    encodeChar(a),
    encodeChar(b),
    encodeChar(c),
    encodeChar(d)
  };

  display.setSegments(segs);
}

void showCurrentCode() {
  char diagChar = '1' + currentDiagram;
  char letter = diagrams[currentDiagram].cols[currentCol];
  char number = diagrams[currentDiagram].rows[currentRow];

  show4Chars(diagChar, ' ', letter, number);
}

void showGood() {
  display.showNumberDecEx(8888, 0, true);
}

void showWrong() {
  show4Chars('-', '-', '-', '-');
}

void showOpen() {
  uint8_t segs[4] = {
    display.encodeDigit(0), // O lijkt op 0
    0x73,                  // P
    0x79,                  // E
    0x54                   // n
  };

  display.setSegments(segs);
}

void showWaiting() {
  show4Chars('-', '-', '-', '-');
}

// -------------------------
// KEYPAD LEZEN
// -------------------------
char readKeypadOnce() {
  // Zet alle rijen LOW
  for (byte r = 0; r < 4; r++) {
    digitalWrite(rowPins[r], LOW);
  }

  // Scan rij voor rij
  for (byte r = 0; r < 4; r++) {
    digitalWrite(rowPins[r], HIGH);
    delayMicroseconds(200);

    for (byte c = 0; c < 3; c++) {
      if (digitalRead(colPins[c]) == HIGH) {
        digitalWrite(rowPins[r], LOW);
        return keyMap[r][c];
      }
    }

    digitalWrite(rowPins[r], LOW);
  }

  return '\0';
}

char waitForStableKeypress() {
  char k1 = readKeypadOnce();
  if (k1 == '\0') return '\0';

  delay(25);

  char k2 = readKeypadOnce();

  if (k1 == k2) return k1;
  return '\0';
}

void waitUntilReleased() {
  while (readKeypadOnce() != '\0') {
    updateSolenoid();
    delay(10);
  }
}

// -------------------------
// NIEUWE OPDRACHT GENEREREN
// -------------------------
void generateNewChallenge() {
  currentDiagram = random(0, 9);
  currentRow = random(0, 4);
  currentCol = random(0, 3);

  expectedKey = keyMap[currentRow][currentCol];

  showCurrentCode();

  Serial.print("Level ");
  Serial.print(level);
  Serial.print(" | diagram ");
  Serial.print(currentDiagram + 1);
  Serial.print(" | code ");
  Serial.print(diagrams[currentDiagram].cols[currentCol]);
  Serial.print(diagrams[currentDiagram].rows[currentRow]);
  Serial.print(" | juiste knop ");
  Serial.println(expectedKey);
}

// -------------------------
// SPELLOGICA
// -------------------------
void resetToLevel1() {
  level = 1;

  showWrong();
  delay(700);

  generateNewChallenge();
}

void handleCorrect() {
  showGood();
  delay(500);

  if (level >= MAX_LEVEL) {
    gameFinished = true;
    victory = true;

    stopSolenoid();
    showOpen();

    Serial.println("Laatste level gehaald.");
    Serial.println("Display toont OPEN.");
    Serial.println("Druk op knop 1 om de solenoid maximaal 2 seconden te openen.");
  } else {
    level++;
    generateNewChallenge();
  }
}

void handleKey(char pressedKey) {
  if (gameFinished) return;

  Serial.print("Gedrukt: ");
  Serial.print(pressedKey);
  Serial.print(" | verwacht: ");
  Serial.println(expectedKey);

  if (pressedKey == expectedKey) {
    handleCorrect();
  } else {
    Serial.println("Fout. Terug naar level 1.");
    resetToLevel1();
  }
}

// -------------------------
// EINDSTATUS
// -------------------------
void handleFinishedKeypad() {
  char key = waitForStableKeypress();

  if (key != '\0') {
    Serial.print("Eindstatus toets gedrukt: ");
    Serial.println(key);

    // AANGEPAST:
    // Eerst was dit key == '0'.
    // Nu opent knop 1 de solenoid.
    if (key == '1') {
      startSolenoid();
    }

    waitUntilReleased();
    delay(50);
  }
}

// -------------------------
// I2C CALLBACKS
// -------------------------
void receiveEvent(int bytes) {
  while (Wire.available()) {
    lastReceived = Wire.read();

    if (lastReceived == START_CODE) {
      puzzleStarted = true;
      victory = false;
      gameFinished = false;
      level = 1;

      stopSolenoid();

      startMessagePending = true;
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

// -------------------------
// SETUP
// -------------------------
void setup() {
  Serial.begin(115200);

  display.setBrightness(7, true);

  // Kolommen zijn inputs.
  // GPIO34 en GPIO35 hebben geen interne pullup/pulldown.
  // In jouw schema zitten externe 10k pulldowns, dus INPUT is goed.
  for (byte c = 0; c < 3; c++) {
    pinMode(colPins[c], INPUT);
  }

  // Rijen zijn outputs.
  // Normaal LOW.
  for (byte r = 0; r < 4; r++) {
    pinMode(rowPins[r], OUTPUT);
    digitalWrite(rowPins[r], LOW);
  }

  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW);

  showWaiting();

  Wire.begin((uint8_t)SLAVE_ADDR, SDA_PIN, SCL_PIN, 100000);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  randomSeed(micros());

  Serial.println("SLAVE READY");
  Serial.println("Wacht op startcode...");
}

// -------------------------
// LOOP
// -------------------------
void loop() {
  updateSolenoid();

  if (startMessagePending) {
    startMessagePending = false;

    Serial.println("START RECEIVED");
    Serial.println("Start spel.");

    stopSolenoid();
    generateNewChallenge();
  }

  if (!puzzleStarted) {
    delay(100);
    return;
  }

  if (gameFinished) {
    showOpen();
    handleFinishedKeypad();
    delay(20);
    return;
  }

  char key = waitForStableKeypress();

  if (key != '\0') {
    handleKey(key);
    waitUntilReleased();
    delay(50);
  }
}