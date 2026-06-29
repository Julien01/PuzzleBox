#include <Wire.h>
#include <TM1637Display.h>

// =====================
// I2C
// =====================

#define SLAVE_ADDR 10

// Pas deze aan als jouw PCB andere I2C-pinnen gebruikt
#define I2C_SDA 26
#define I2C_SCL 27

#define CMD_START_HARDWARE_PUZZLE 54
#define CMD_RESET_HARDWARE_PUZZLE 101

#define STATE_IDLE     0
#define STATE_RUNNING  1
#define STATE_VICTORY  2

volatile uint8_t responseValue = STATE_IDLE;

volatile bool slaveDataReceived = false;
volatile int slaveBytesCount = 0;
volatile uint8_t slaveRxBuffer[32];

volatile bool startPuzzleRequested = false;
volatile bool resetPuzzleRequested = false;

volatile int lastReceived = 0;

// =====================
// Function prototypes
// =====================

void handleSerialInput();

void resetPuzzle();
void startPuzzle();

bool boolPuzzleCorrect();

void resetPlayerCode();
void updatePlayerCodeFromPots();
void showPotDigitsOnDisplay();
void updatePotDisplayConstantly();

void updateMorse();
void startMorseCode();
void stopMorseCode();

void checkVictory();
void startVictoryBlink();
void handleVictoryBlink();

int correctCodeAsNumber();
bool potentiometerCodeCorrect();

// =====================
// Pin setup
// =====================

// Switch pins
const int boolPins[8] = {
  33, 32, 35, 34, 25, 23, 22, 21
};

// GPIO36 = waarde kiezen van 0 t/m 9
// GPIO37 = actief cijfer kiezen van 1 t/m 4
const int potValuePin  = 36;
const int potSelectPin = 37;

// Morse LED
const int morseLedPin = 5;

// TM1637 seven-segment display
const int displayDataPin = 19;
const int displayClkPin  = 18;

TM1637Display display(displayClkPin, displayDataPin);

// =====================
// Fixed switch combination
// =====================
//
// true  = verwachte input is HIGH
// false = verwachte input is LOW
//
// Deze array verandert alleen wat de verwachte switchstand is.
// De pinMode blijft altijd INPUT.
//

bool correctBoolState[8] = {
  true,   // GPIO 33
  false,   // GPIO 32
  false,  // GPIO 35
  false,  // GPIO 34
  true,   // GPIO 25
  false,  // GPIO 23
  false,  // GPIO 22
  true    // GPIO 21
};

// =====================
// Fixed potmeter / Morse code
// =====================

// Code is altijd 5213
int correctCode[4] = {
  5, 2, 1, 3
};

// Door speler ingestelde code
int playerCode[4] = {
  0, 0, 0, 0
};

// Huidige potmeterstatus voor display/debug
int activeDigitIndex = 0;       // 0 t/m 3
int selectedDigitValue = 0;     // 0 t/m 9
int lastRawValuePot = 0;
int lastRawSelectPot = 0;

// =====================
// Puzzle state
// =====================

bool puzzleStarted = false;
bool victory = false;
bool puzzleFinished = false;

// =====================
// Victory blink state
// =====================

bool victoryBlinkActive = false;
unsigned long victoryBlinkStartTime = 0;
unsigned long lastVictoryBlink = 0;
bool victoryDisplayState = false;

const unsigned long victoryBlinkDuration = 3000;
const unsigned long victoryBlinkInterval = 150;

// =====================
// Morse settings
// =====================

const unsigned long dotTime = 200;
const unsigned long dashTime = dotTime * 3;
const unsigned long symbolGap = dotTime;
const unsigned long letterGap = dotTime * 3;
const unsigned long repeatGap = dotTime * 7;

// Non-blocking Morse state
bool morseRunning = false;
bool morseLedOn = false;

int morseDigitIndex = 0;
int morseSymbolIndex = 0;

unsigned long morseNextChangeTime = 0;
unsigned long morseCurrentDuration = 0;

enum MorsePhase {
  MORSE_START_SYMBOL,
  MORSE_SYMBOL_ON,
  MORSE_SYMBOL_GAP,
  MORSE_LETTER_GAP,
  MORSE_REPEAT_GAP
};

MorsePhase morsePhase = MORSE_START_SYMBOL;

// =====================
// Display update settings
// =====================

unsigned long lastDisplayUpdate = 0;
const unsigned long displayUpdateInterval = 30;

// =====================
// Debug settings
// =====================

unsigned long lastDebugPrint = 0;
const unsigned long debugInterval = 1000;

bool previousSwitchesCorrect = false;

// =====================
// I2C events
// =====================

void receiveEvent(int bytes) {
  slaveBytesCount = 0;

  while (Wire.available() && slaveBytesCount < 32) {
    slaveRxBuffer[slaveBytesCount] = Wire.read();
    slaveBytesCount++;
  }

  if (slaveBytesCount > 0) {
    uint8_t cmd = slaveRxBuffer[0];
    lastReceived = cmd;

    if (cmd == CMD_START_HARDWARE_PUZZLE) {
      startPuzzleRequested = true;

      // Direct ACK naar main-controller
      responseValue = STATE_RUNNING;
    }

    if (cmd == CMD_RESET_HARDWARE_PUZZLE) {
      resetPuzzleRequested = true;

      // Direct terug naar idle
      responseValue = STATE_IDLE;
    }
  }

  slaveDataReceived = true;
}

void requestEvent() {
  // Main leest 1 byte:
  // 0 = idle
  // 1 = running
  // 2 = victory
  Wire.write(responseValue);
}

// =====================
// Puzzle reset/start
// =====================

void resetPuzzle() {
  puzzleStarted = false;
  victory = false;
  puzzleFinished = false;
  victoryBlinkActive = false;

  responseValue = STATE_IDLE;

  stopMorseCode();
  resetPlayerCode();

  digitalWrite(morseLedPin, LOW);
  display.clear();

  previousSwitchesCorrect = false;

  Serial.println("[HARDWARE PUZZLE] Reset naar IDLE");
  Serial.println("[HARDWARE PUZZLE] Wacht op I2C startcode 54...");
}

void startPuzzle() {
  puzzleStarted = true;
  victory = false;
  puzzleFinished = false;
  victoryBlinkActive = false;

  responseValue = STATE_RUNNING;

  stopMorseCode();
  resetPlayerCode();

  digitalWrite(morseLedPin, LOW);
  display.clear();

  previousSwitchesCorrect = false;

  Serial.println("[HARDWARE PUZZLE] Puzzle gestart door I2C startcode 54");
}

// =====================
// Puzzle setup
// =====================

void printPuzzleSetup() {
  Serial.println();
  Serial.println("========== HARDWARE PUZZLE SETUP ==========");

  Serial.println("[SETUP] Correct switch states:");
  Serial.println("[SETUP] true = expected HIGH, false = expected LOW");

  for (int i = 0; i < 8; i++) {
    Serial.print("  boolPins[");
    Serial.print(i);
    Serial.print("] GPIO ");
    Serial.print(boolPins[i]);
    Serial.print(" expected ");

    if (correctBoolState[i]) {
      Serial.println("HIGH / TRUE");
    } else {
      Serial.println("LOW / FALSE");
    }
  }

  Serial.println();

  Serial.print("[SETUP] Correct pot/Morse code: ");
  for (int i = 0; i < 4; i++) {
    Serial.print(correctCode[i]);
  }
  Serial.println();

  Serial.println("[SETUP] Potmeter input:");
  Serial.println("  GPIO36 = waarde 0-9");
  Serial.println("  GPIO37 = actief cijfer 1-4");

  Serial.print("[SETUP] Morse LED GPIO: ");
  Serial.println(morseLedPin);

  Serial.print("[SETUP] Display CLK GPIO: ");
  Serial.println(displayClkPin);

  Serial.print("[SETUP] Display DIO GPIO: ");
  Serial.println(displayDataPin);

  Serial.print("[SETUP] I2C address: ");
  Serial.println(SLAVE_ADDR);

  Serial.print("[SETUP] I2C SDA GPIO: ");
  Serial.println(I2C_SDA);

  Serial.print("[SETUP] I2C SCL GPIO: ");
  Serial.println(I2C_SCL);

  Serial.print("[SETUP] Startcode: ");
  Serial.println(CMD_START_HARDWARE_PUZZLE);

  Serial.println("===========================================");
  Serial.println();
}

// =====================
// Setup
// =====================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Hardware puzzle PCB gestart");
  Serial.println("----------------------------");

  // I2C slave
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  bool ok = Wire.begin(
    (uint8_t)SLAVE_ADDR,
    I2C_SDA,
    I2C_SCL,
    100000
  );

  if (!ok) {
    Serial.println("FOUT: I2C slave kon niet gestart worden");

    while (true) {
      delay(1000);
    }
  }

  pinMode(morseLedPin, OUTPUT);
  digitalWrite(morseLedPin, LOW);

  // Alle switches zijn gewone inputs.
  // Geen INPUT_PULLUP of INPUT_PULLDOWN.
  for (int i = 0; i < 8; i++) {
    pinMode(boolPins[i], INPUT);

    Serial.print("[PINMODE] GPIO ");
    Serial.print(boolPins[i]);
    Serial.println(" set as INPUT");
  }

  pinMode(potValuePin, INPUT);
  pinMode(potSelectPin, INPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(potValuePin, ADC_11db);
  analogSetPinAttenuation(potSelectPin, ADC_11db);

  display.setBrightness(7);
  display.clear();

  resetPlayerCode();

  // Belangrijk:
  // Puzzel start NIET automatisch.
  puzzleStarted = false;
  victory = false;
  puzzleFinished = false;
  victoryBlinkActive = false;
  responseValue = STATE_IDLE;

  printPuzzleSetup();

  Serial.println("================================");
  Serial.println("SLAVE ready");
  Serial.print("I2C address: ");
  Serial.println(SLAVE_ADDR);
  Serial.println("Puzzle staat IDLE.");
  Serial.println("Wacht op I2C startcode 54.");
  Serial.println();
  Serial.println("I2C status:");
  Serial.println("  0 = idle / not started");
  Serial.println("  1 = running");
  Serial.println("  2 = victory");
  Serial.println();
  Serial.println("Serial commands:");
  Serial.println("  RESET     -> reset naar idle");
  Serial.println("  START     -> lokaal starten zonder I2C, alleen voor test");
  Serial.println("  LEDTEST   -> test Morse LED");
  Serial.println("  MORSETEST -> test Morse zonder switches");
  Serial.println("================================");
}

// =====================
// Switch input reading
// =====================

bool readBoolPin(int index) {
  int state = digitalRead(boolPins[index]);

  // Normale input-logica:
  // HIGH = true
  // LOW  = false
  return state == HIGH;
}

bool boolPuzzleCorrect() {
  for (int i = 0; i < 8; i++) {
    bool currentState = readBoolPin(i);

    if (currentState != correctBoolState[i]) {
      return false;
    }
  }

  return true;
}

// =====================
// Potentiometer reading
// =====================

void resetPlayerCode() {
  for (int i = 0; i < 4; i++) {
    playerCode[i] = 0;
  }

  activeDigitIndex = 0;
  selectedDigitValue = 0;
  lastRawValuePot = 0;
  lastRawSelectPot = 0;
}

int readPotRawPin(int pin) {
  const int samples = 12;
  long total = 0;

  for (int i = 0; i < samples; i++) {
    total += analogRead(pin);
    delayMicroseconds(150);
  }

  return total / samples;
}

int rawToDigit(int rawValue) {
  // Verdeel 0 t/m 4095 in 10 zones.
  int digit = (rawValue * 10L) / 4096L;

  if (digit < 0) {
    digit = 0;
  }

  if (digit > 9) {
    digit = 9;
  }

  return digit;
}

int rawToActiveDigitIndex(int rawValue) {
  // Verdeel 0 t/m 4095 in 4 zones.
  int index = (rawValue * 4L) / 4096L;

  if (index < 0) {
    index = 0;
  }

  if (index > 3) {
    index = 3;
  }

  return index;
}

void updatePlayerCodeFromPots() {
  lastRawValuePot = readPotRawPin(potValuePin);
  lastRawSelectPot = readPotRawPin(potSelectPin);

  selectedDigitValue = rawToDigit(lastRawValuePot);
  activeDigitIndex = rawToActiveDigitIndex(lastRawSelectPot);

  // Alleen het actieve cijfer wordt aangepast.
  // De andere cijfers blijven opgeslagen.
  playerCode[activeDigitIndex] = selectedDigitValue;
}

bool potentiometerCodeCorrect() {
  for (int i = 0; i < 4; i++) {
    if (playerCode[i] != correctCode[i]) {
      return false;
    }
  }

  return true;
}

void showPotDigitsOnDisplay() {
  uint8_t segments[4];

  // Actieve cijfer knippert.
  bool activeVisible = ((millis() / 350) % 2) == 0;

  for (int i = 0; i < 4; i++) {
    if (i == activeDigitIndex && !activeVisible) {
      segments[i] = 0x00;
    } else {
      segments[i] = display.encodeDigit(playerCode[i]);
    }
  }

  display.setSegments(segments);
}

void updatePotDisplayConstantly() {
  if (millis() - lastDisplayUpdate < displayUpdateInterval) {
    return;
  }

  lastDisplayUpdate = millis();

  if (!puzzleStarted) {
    display.clear();
    return;
  }

  if (victory || puzzleFinished || victoryBlinkActive) {
    return;
  }

  // Display alleen tonen in fase 2
  if (boolPuzzleCorrect()) {
    updatePlayerCodeFromPots();
    showPotDigitsOnDisplay();
  } else {
    display.clear();
  }
}

int correctCodeAsNumber() {
  return correctCode[0] * 1000 +
         correctCode[1] * 100 +
         correctCode[2] * 10 +
         correctCode[3];
}

// =====================
// Serial input
// =====================

void handleSerialInput() {
  if (!Serial.available()) {
    return;
  }

  String input = Serial.readStringUntil('\n');
  input.trim();
  input.toUpperCase();

  if (input == "RESET") {
    resetPuzzle();
    Serial.println("[SERIAL] Puzzle reset naar idle.");
    return;
  }

  if (input == "START") {
    startPuzzle();
    Serial.println("[SERIAL] Puzzle lokaal gestart.");
    return;
  }

  if (input == "LEDTEST") {
    Serial.println("[LEDTEST] Blinking LED on GPIO 5.");

    stopMorseCode();

    for (int i = 0; i < 5; i++) {
      digitalWrite(morseLedPin, HIGH);
      delay(300);
      digitalWrite(morseLedPin, LOW);
      delay(300);
    }

    Serial.println("[LEDTEST] Done.");
    return;
  }

  if (input == "MORSETEST") {
    Serial.println("[MORSETEST] Starting Morse 5213 without switch check.");

    victory = false;
    puzzleFinished = false;
    victoryBlinkActive = false;
    puzzleStarted = true;
    responseValue = STATE_RUNNING;

    stopMorseCode();
    startMorseCode();
    return;
  }

  Serial.println("[SERIAL] Unknown command.");
  Serial.println("[SERIAL] Commands:");
  Serial.println("  RESET");
  Serial.println("  START");
  Serial.println("  LEDTEST");
  Serial.println("  MORSETEST");
}

// =====================
// Debug output
// =====================

void printBoolDebug() {
  Serial.println("[SWITCH INPUTS]");

  for (int i = 0; i < 8; i++) {
    bool currentState = readBoolPin(i);
    int rawState = digitalRead(boolPins[i]);

    Serial.print("  boolPins[");
    Serial.print(i);
    Serial.print("] GPIO ");
    Serial.print(boolPins[i]);

    Serial.print(" raw=");
    Serial.print(rawState == HIGH ? "HIGH" : "LOW");

    Serial.print(" logical=");
    Serial.print(currentState ? "TRUE " : "FALSE");

    Serial.print(" expected=");
    Serial.print(correctBoolState[i] ? "TRUE " : "FALSE");

    Serial.print(" expectedRaw=");
    Serial.print(correctBoolState[i] ? "HIGH" : "LOW");

    Serial.println(currentState == correctBoolState[i] ? " | OK" : " | WRONG");
  }
}

void printPotDebug() {
  Serial.println("[POTENTIOMETERS]");

  Serial.print("  GPIO36 raw=");
  Serial.print(lastRawValuePot);
  Serial.print(" -> selected value=");
  Serial.println(selectedDigitValue);

  Serial.print("  GPIO37 raw=");
  Serial.print(lastRawSelectPot);
  Serial.print(" -> active digit=");
  Serial.println(activeDigitIndex + 1);

  Serial.print("  Player code: ");

  for (int i = 0; i < 4; i++) {
    if (i == activeDigitIndex) {
      Serial.print("[");
      Serial.print(playerCode[i]);
      Serial.print("]");
    } else {
      Serial.print(" ");
      Serial.print(playerCode[i]);
      Serial.print(" ");
    }
  }

  Serial.println();

  Serial.print("  Expected code: ");
  for (int i = 0; i < 4; i++) {
    Serial.print(correctCode[i]);
  }
  Serial.println();

  Serial.print("  Pot code correct: ");
  Serial.println(potentiometerCodeCorrect() ? "YES" : "NO");
}

void printCorrectCodeDebug() {
  Serial.print("[SOLUTION] Switch states: ");

  for (int i = 0; i < 8; i++) {
    Serial.print(correctBoolState[i] ? "1" : "0");
  }

  Serial.print(" | Pot/Morse code: ");

  for (int i = 0; i < 4; i++) {
    Serial.print(correctCode[i]);
  }

  Serial.println();
}

void printGeneralDebug() {
  if (millis() - lastDebugPrint < debugInterval) {
    return;
  }

  lastDebugPrint = millis();

  bool switchesCorrect = boolPuzzleCorrect();

  if (switchesCorrect && !previousSwitchesCorrect) {
    Serial.println();
    Serial.println("[PHASE] Switches correct. Entering phase 2: two-potmeter input + Morse LED.");
  }

  previousSwitchesCorrect = switchesCorrect;

  Serial.println();
  Serial.println("========== HARDWARE PUZZLE DEBUG ==========");

  Serial.print("Puzzle started: ");
  Serial.println(puzzleStarted ? "YES" : "NO");

  Serial.print("Victory: ");
  Serial.println(victory ? "YES" : "NO");

  Serial.print("Puzzle finished: ");
  Serial.println(puzzleFinished ? "YES" : "NO");

  Serial.print("Victory blink active: ");
  Serial.println(victoryBlinkActive ? "YES" : "NO");

  Serial.print("I2C response value: ");
  Serial.println(responseValue);

  Serial.print("Last I2C received: ");
  Serial.println(lastReceived);

  Serial.print("Morse running: ");
  Serial.println(morseRunning ? "YES" : "NO");

  Serial.print("Morse LED state: ");
  Serial.println(morseLedOn ? "ON" : "OFF");

  printCorrectCodeDebug();

  Serial.print("Phase: ");
  if (!puzzleStarted) {
    Serial.println("0 - idle / waiting for startcode 54");
  } else if (!switchesCorrect) {
    Serial.println("1 - switches");
  } else {
    Serial.println("2 - two-potmeter code input + Morse");
  }

  Serial.print("Switch puzzle correct: ");
  Serial.println(switchesCorrect ? "YES" : "NO");

  if (puzzleStarted) {
    printBoolDebug();

    if (switchesCorrect) {
      printPotDebug();
    } else {
      Serial.println("[POTENTIOMETERS] Not active yet. Complete switches first.");
    }
  }

  if (!puzzleStarted) {
    Serial.println("[STATUS] IDLE. Waiting for I2C startcode 54.");
  }
  else if (victory || puzzleFinished) {
    Serial.println("[STATUS] Puzzle completed.");
  }
  else if (victoryBlinkActive) {
    Serial.println("[STATUS] Victory blink running. I2C still returns RUNNING.");
  }
  else if (!switchesCorrect) {
    Serial.println("[STATUS] Waiting for correct switch combination.");
  }
  else if (!potentiometerCodeCorrect()) {
    Serial.println("[STATUS] Switches correct. Set player code to 5213.");
  }
  else {
    Serial.println("[STATUS] Puzzle should now complete.");
  }

  Serial.println("==========================================");
}

// =====================
// Morse code
// =====================

const char* morseForDigit(int digit) {
  switch (digit) {
    case 0: return "-----";
    case 1: return ".----";
    case 2: return "..---";
    case 3: return "...--";
    case 4: return "....-";
    case 5: return ".....";
    case 6: return "-....";
    case 7: return "--...";
    case 8: return "---..";
    case 9: return "----.";
    default: return "?";
  }
}

void printMorseCodeDebug() {
  Serial.println();
  Serial.print("[MORSE] Full code: ");

  for (int i = 0; i < 4; i++) {
    Serial.print(correctCode[i]);
  }

  Serial.println();

  for (int i = 0; i < 4; i++) {
    Serial.print("[MORSE] ");
    Serial.print(correctCode[i]);
    Serial.print(" = ");
    Serial.println(morseForDigit(correctCode[i]));
  }
}

void startMorseCode() {
  if (morseRunning) {
    return;
  }

  morseRunning = true;
  morseLedOn = false;

  morseDigitIndex = 0;
  morseSymbolIndex = 0;
  morsePhase = MORSE_START_SYMBOL;
  morseNextChangeTime = millis();

  printMorseCodeDebug();

  Serial.println("[MORSE] Non-blocking Morse started.");
}

void stopMorseCode() {
  morseRunning = false;
  morseLedOn = false;
  digitalWrite(morseLedPin, LOW);
}

void updateMorse() {
  if (!morseRunning) {
    return;
  }

  unsigned long now = millis();

  if (now < morseNextChangeTime) {
    return;
  }

  int currentDigit = correctCode[morseDigitIndex];
  const char* morse = morseForDigit(currentDigit);
  char currentSymbol = morse[morseSymbolIndex];

  switch (morsePhase) {
    case MORSE_START_SYMBOL:
      Serial.print("[MORSE] digit ");
      Serial.print(currentDigit);
      Serial.print(" symbol ");
      Serial.print(morseSymbolIndex);
      Serial.print(" = ");
      Serial.println(currentSymbol);

      digitalWrite(morseLedPin, HIGH);
      morseLedOn = true;

      if (currentSymbol == '.') {
        morseCurrentDuration = dotTime;
      } else {
        morseCurrentDuration = dashTime;
      }

      morseNextChangeTime = now + morseCurrentDuration;
      morsePhase = MORSE_SYMBOL_ON;
      break;

    case MORSE_SYMBOL_ON:
      digitalWrite(morseLedPin, LOW);
      morseLedOn = false;

      morseNextChangeTime = now + symbolGap;
      morsePhase = MORSE_SYMBOL_GAP;
      break;

    case MORSE_SYMBOL_GAP:
      morseSymbolIndex++;

      if (morseSymbolIndex >= 5) {
        morseSymbolIndex = 0;
        morseDigitIndex++;

        if (morseDigitIndex >= 4) {
          morseDigitIndex = 0;
          morseNextChangeTime = now + repeatGap;
          morsePhase = MORSE_REPEAT_GAP;
        } else {
          morseNextChangeTime = now + letterGap;
          morsePhase = MORSE_LETTER_GAP;
        }
      } else {
        morsePhase = MORSE_START_SYMBOL;
        morseNextChangeTime = now;
      }

      break;

    case MORSE_LETTER_GAP:
      morsePhase = MORSE_START_SYMBOL;
      morseNextChangeTime = now;
      break;

    case MORSE_REPEAT_GAP:
      Serial.println("[MORSE] Repeating full code.");
      morsePhase = MORSE_START_SYMBOL;
      morseNextChangeTime = now;
      break;
  }
}

// =====================
// Victory display blink
// =====================

void startVictoryBlink() {
  Serial.println();
  Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  Serial.println("VICTORY! Correct switches + player code 5213.");
  Serial.println("Starting 7-segment victory blink.");
  Serial.println("During blink: I2C returns RUNNING.");
  Serial.println("After blink: I2C returns VICTORY.");
  Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

  // Tijdens flikkeren blijft de main-controller nog RUNNING lezen.
  puzzleStarted = false;
  victory = false;
  puzzleFinished = false;

  responseValue = STATE_RUNNING;

  stopMorseCode();
  digitalWrite(morseLedPin, LOW);

  victoryBlinkActive = true;
  victoryBlinkStartTime = millis();
  lastVictoryBlink = millis();
  victoryDisplayState = false;

  display.clear();
}

void handleVictoryBlink() {
  if (!victoryBlinkActive) {
    return;
  }

  unsigned long now = millis();

  if (now - lastVictoryBlink >= victoryBlinkInterval) {
    lastVictoryBlink = now;
    victoryDisplayState = !victoryDisplayState;

    if (victoryDisplayState) {
      uint8_t allOn[4] = {
        0xFF, 0xFF, 0xFF, 0xFF
      };

      display.setSegments(allOn);
    } else {
      display.clear();
    }
  }

  if (now - victoryBlinkStartTime >= victoryBlinkDuration) {
    victoryBlinkActive = false;
    puzzleFinished = true;
    victory = true;

    responseValue = STATE_VICTORY;

    stopMorseCode();
    digitalWrite(morseLedPin, LOW);
    display.clear();

    Serial.println("[HARDWARE PUZZLE] Victory signaal actief, puzzel uit.");
  }
}

// =====================
// Victory check
// =====================

void checkVictory() {
  if (victory || puzzleFinished || victoryBlinkActive) {
    return;
  }

  if (!puzzleStarted) {
    return;
  }

  if (!boolPuzzleCorrect()) {
    return;
  }

  updatePlayerCodeFromPots();

  if (!potentiometerCodeCorrect()) {
    return;
  }

  startVictoryBlink();
}

// =====================
// Main loop
// =====================

void loop() {
  handleSerialInput();

  // Ontvangen I2C-data debuggen
  if (slaveDataReceived) {
    noInterrupts();

    int count = slaveBytesCount;
    uint8_t localCopy[32];

    for (int i = 0; i < count; i++) {
      localCopy[i] = slaveRxBuffer[i];
    }

    slaveDataReceived = false;

    interrupts();

    Serial.print("[SLAVE] Ontvangen ");
    Serial.print(count);
    Serial.print(" byte(s): ");

    for (int i = 0; i < count; i++) {
      Serial.print("0x");

      if (localCopy[i] < 16) {
        Serial.print("0");
      }

      Serial.print(localCopy[i], HEX);
      Serial.print(" ");

      Serial.print("(");
      Serial.print(localCopy[i]);
      Serial.print(") ");
    }

    Serial.println();
  }

  // Reset command verwerken
  if (resetPuzzleRequested) {
    noInterrupts();
    resetPuzzleRequested = false;
    interrupts();

    resetPuzzle();
  }

  // Start command verwerken
  if (startPuzzleRequested) {
    noInterrupts();
    startPuzzleRequested = false;
    interrupts();

    startPuzzle();
  }

  // Victory blink bezig
  if (victoryBlinkActive) {
    handleVictoryBlink();
    delay(5);
    return;
  }

  // Puzzel is klaar
  if (puzzleFinished || victory) {
    stopMorseCode();
    digitalWrite(morseLedPin, LOW);
    display.clear();
    delay(50);
    return;
  }

  // Nog niet gestart
  if (!puzzleStarted) {
    stopMorseCode();
    digitalWrite(morseLedPin, LOW);
    display.clear();

    // Minder debug-spam wanneer hij idle staat
    printGeneralDebug();

    delay(50);
    return;
  }

  updatePotDisplayConstantly();

  printGeneralDebug();

  bool switchesCorrect = boolPuzzleCorrect();

  if (!switchesCorrect) {
    stopMorseCode();
    display.clear();
    delay(5);
    return;
  }

  // Fase 2:
  // Switches zijn correct.
  // GPIO36 kiest waarde 0-9.
  // GPIO37 kiest actieve positie 1-4.
  // Display toont speler-code.
  // Actieve positie knippert.
  // LED knippert Morse voor code 5213.
  startMorseCode();
  updateMorse();

  checkVictory();

  delay(5);
}