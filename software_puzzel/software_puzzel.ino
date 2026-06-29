#include <Wire.h>

// =====================
// I2C
// =====================

#define SLAVE_ADDR 9

// Gebaseerd op jouw werkende I2C-testcode
#define I2C_SDA 26
#define I2C_SCL 27

#define CMD_START_SOFTWARE_PUZZLE 48
#define CMD_RESET_SOFTWARE_PUZZLE 101

#define STATE_IDLE     0
#define STATE_RUNNING  1
#define STATE_VICTORY  2

volatile uint8_t responseValue = STATE_IDLE;

volatile bool slaveDataReceived = false;
volatile int slaveBytesCount = 0;
volatile uint8_t slaveRxBuffer[32];

volatile bool startPuzzleRequested = false;
volatile bool resetPuzzleRequested = false;

// =====================
// Puzzle status
// =====================

bool puzzleStarted = false;
bool victory = false;
bool puzzleFinished = false;

bool victoryBlinkActive = false;
unsigned long victoryBlinkStartTime = 0;
unsigned long lastVictoryBlink = 0;
bool victoryLedState = false;

// =====================
// Banana puzzle pins
// =====================

// PCB inputs:
// 1 = IO34
// 2 = IO35
// 3 = SENSOR_VP   = GPIO36
// 4 = SENSOR_CAPP = GPIO37
// 5 = SENSOR_CAPN = GPIO38
// 6 = SENSOR_VN   = GPIO39
const int inPins[6] = {
  34, 35, 36, 37, 38, 39
};

// PCB outputs:
// 1 = IO33
// 2 = IO32
// 3 = IO25
// 4 = IO14
// 5 = IO13
// 6 = IO5
const int outPins[6] = {
  33, 32, 25, 14, 13, 5
};

// Mapping:
// input 1 = output 4
// input 2 = output 5
// input 3 = output 6
// input 4 = output 1
// input 5 = output 2
// input 6 = output 3
const int inputToOutput[6] = {
  3,  // input 1 -> output 4
  4,  // input 2 -> output 5
  5,  // input 3 -> output 6
  0,  // input 4 -> output 1
  1,  // input 5 -> output 2
  2   // input 6 -> output 3
};

// =====================
// Shiftregister pins
// =====================

const int DATA_PIN  = 23;  // SER
const int CLOCK_PIN = 18;  // SRCLK
const int RESET_PIN = 19;  // SRCLR, active LOW
const int OE_PIN    = 21;  // OE, active LOW
const int LATCH_PIN = 22;  // RCLK

// =====================
// Timing
// =====================

const unsigned long ledInterval = 80;
const unsigned long puzzleCheckInterval = 150;

const unsigned long victoryBlinkDuration = 3000;
const unsigned long victoryBlinkInterval = 150;

unsigned long lastLedUpdate = 0;
unsigned long lastPuzzleCheck = 0;

int ledStep = 0;
int effectMode = 0;

// =====================
// Shiftregister functies
// =====================

void writeLeds(uint16_t value)
{
  byte u3 = value & 0xFF;
  byte u4 = (value >> 8) & 0xFF;

  digitalWrite(LATCH_PIN, LOW);

  // Eerst U4, daarna U3
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, u4);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, u3);

  digitalWrite(LATCH_PIN, HIGH);
}

// =====================
// I2C callbacks
// =====================

void receiveEvent(int numBytes)
{
  slaveBytesCount = 0;

  while (Wire.available() && slaveBytesCount < 32)
  {
    slaveRxBuffer[slaveBytesCount] = Wire.read();
    slaveBytesCount++;
  }

  if (slaveBytesCount > 0)
  {
    uint8_t cmd = slaveRxBuffer[0];

    if (cmd == CMD_START_SOFTWARE_PUZZLE)
    {
      startPuzzleRequested = true;

      // Direct ACK geven aan main
      responseValue = STATE_RUNNING;
    }

    if (cmd == CMD_RESET_SOFTWARE_PUZZLE)
    {
      resetPuzzleRequested = true;
    }
  }

  slaveDataReceived = true;
}

void requestEvent()
{
  // Main code leest 1 byte, dus stuur 1 byte terug
  Wire.write(responseValue);
}

// =====================
// Puzzle reset/start
// =====================

void resetPuzzle()
{
  puzzleStarted = false;
  victory = false;
  puzzleFinished = false;
  victoryBlinkActive = false;

  responseValue = STATE_IDLE;

  ledStep = 0;
  effectMode = 0;
  victoryLedState = false;

  for (int i = 0; i < 6; i++)
  {
    digitalWrite(outPins[i], LOW);
  }

  writeLeds(0x0000);

  Serial.println("[SOFTWARE PUZZLE] Reset naar idle");
}

void startPuzzle()
{
  puzzleStarted = true;
  victory = false;
  puzzleFinished = false;
  victoryBlinkActive = false;

  responseValue = STATE_RUNNING;

  ledStep = 0;
  effectMode = 0;
  victoryLedState = false;

  for (int i = 0; i < 6; i++)
  {
    digitalWrite(outPins[i], LOW);
  }

  writeLeds(0x0000);

  Serial.println("[SOFTWARE PUZZLE] Puzzle gestart door I2C startcode 48");
}

// =====================
// Puzzle check
// =====================
//
// PCB heeft 47k pulldown op elke input.
// Dus:
// - standaard zijn inputs LOW
// - we zetten steeds 1 output HIGH
// - als de juiste kabel verbonden is, wordt de input HIGH
//

bool checkPair(int inputIndex)
{
  for (int j = 0; j < 6; j++)
  {
    digitalWrite(outPins[j], LOW);
  }

  delayMicroseconds(500);

  int outputIndex = inputToOutput[inputIndex];

  digitalWrite(outPins[outputIndex], HIGH);
  delayMicroseconds(1000);

  int state = digitalRead(inPins[inputIndex]);

  digitalWrite(outPins[outputIndex], LOW);

  return (state == HIGH);
}

void checkPuzzle()
{
  bool allComplete = true;

  for (int i = 0; i < 6; i++)
  {
    bool ok = checkPair(i);

    Serial.print("Pair ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(ok ? "OK" : "NO");

    if (!ok)
    {
      allComplete = false;
    }
  }

  Serial.println();

  if (allComplete)
  {
    Serial.println("[SOFTWARE PUZZLE] Opgelost, victory blink gestart");

    puzzleStarted = false;

    // Tijdens knipperen blijft main nog RUNNING lezen
    responseValue = STATE_RUNNING;

    victoryBlinkActive = true;
    victoryBlinkStartTime = millis();
    lastVictoryBlink = millis();
    victoryLedState = false;

    ledStep = 0;

    for (int i = 0; i < 6; i++)
    {
      digitalWrite(outPins[i], LOW);
    }
  }
}

// =====================
// LED effecten
// =====================

void runningLedEffect()
{
  if (millis() - lastLedUpdate < ledInterval)
  {
    return;
  }

  lastLedUpdate = millis();

  uint16_t value = 0x0000;

  if (effectMode == 0)
  {
    int pos = ledStep;

    if (pos >= 16)
    {
      pos = 30 - pos;
    }

    value = 1 << pos;

    ledStep++;

    if (ledStep >= 31)
    {
      ledStep = 0;
      effectMode = 1;
    }
  }
  else if (effectMode == 1)
  {
    for (int i = 0; i <= ledStep; i++)
    {
      value |= (1 << i);
    }

    ledStep++;

    if (ledStep >= 16)
    {
      ledStep = 0;
      effectMode = 2;
    }
  }
  else if (effectMode == 2)
  {
    if (ledStep % 2 == 0)
    {
      value = 0b1010101010101010;
    }
    else
    {
      value = 0b0101010101010101;
    }

    ledStep++;

    if (ledStep >= 10)
    {
      ledStep = 0;
      effectMode = 0;
    }
  }

  writeLeds(value);
}

void idleLedEffect()
{
  writeLeds(0x0000);
}

void handleVictoryBlink()
{
  if (!victoryBlinkActive)
  {
    return;
  }

  unsigned long now = millis();

  if (now - lastVictoryBlink >= victoryBlinkInterval)
  {
    lastVictoryBlink = now;
    victoryLedState = !victoryLedState;

    if (victoryLedState)
    {
      writeLeds(0xFFFF);
    }
    else
    {
      writeLeds(0x0000);
    }
  }

  if (now - victoryBlinkStartTime >= victoryBlinkDuration)
  {
    Serial.println("[SOFTWARE PUZZLE] Victory signaal actief, puzzel uit");

    victoryBlinkActive = false;
    puzzleFinished = true;
    victory = true;

    responseValue = STATE_VICTORY;

    writeLeds(0x0000);

    for (int i = 0; i < 6; i++)
    {
      digitalWrite(outPins[i], LOW);
    }
  }
}

// =====================
// Setup
// =====================

void setup()
{
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Software puzzle PCB gestart");
  Serial.println("----------------------------");

  Serial.print("I2C adres decimaal: ");
  Serial.println(SLAVE_ADDR);

  Serial.print("I2C adres hex: 0x");
  Serial.println(SLAVE_ADDR, HEX);

  Serial.print("I2C SDA: GPIO ");
  Serial.println(I2C_SDA);

  Serial.print("I2C SCL: GPIO ");
  Serial.println(I2C_SCL);

  Serial.print("Startcode: ");
  Serial.print(CMD_START_SOFTWARE_PUZZLE);
  Serial.print(" / 0x");
  Serial.println(CMD_START_SOFTWARE_PUZZLE, HEX);

  // I2C slave
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  bool ok = Wire.begin(
    (uint8_t)SLAVE_ADDR,
    I2C_SDA,
    I2C_SCL,
    100000
  );

  if (!ok)
  {
    Serial.println("FOUT: I2C slave kon niet gestart worden");
    while (true)
    {
      delay(1000);
    }
  }

  // Banana outputs
  for (int i = 0; i < 6; i++)
  {
    pinMode(outPins[i], OUTPUT);
    digitalWrite(outPins[i], LOW);
  }

  // Banana inputs
  for (int i = 0; i < 6; i++)
  {
    pinMode(inPins[i], INPUT);
  }

  // Shiftregister setup
  pinMode(DATA_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(RESET_PIN, OUTPUT);
  pinMode(OE_PIN, OUTPUT);
  pinMode(LATCH_PIN, OUTPUT);

  digitalWrite(DATA_PIN, LOW);
  digitalWrite(CLOCK_PIN, LOW);
  digitalWrite(LATCH_PIN, LOW);

  // SRCLR is active LOW, dus HIGH = normaal werken
  digitalWrite(RESET_PIN, HIGH);

  // OE is active LOW, dus LOW = outputs actief
  digitalWrite(OE_PIN, LOW);

  writeLeds(0x0000);

  responseValue = STATE_IDLE;

  Serial.println("PCB ready");
  Serial.println("Wacht op I2C startcode 48...");
}

// =====================
// Loop
// =====================

void loop()
{
  // Ontvangen I2C-data debuggen
  if (slaveDataReceived)
  {
    noInterrupts();

    int count = slaveBytesCount;
    uint8_t localCopy[32];

    for (int i = 0; i < count; i++)
    {
      localCopy[i] = slaveRxBuffer[i];
    }

    slaveDataReceived = false;

    interrupts();

    Serial.print("[SLAVE] Ontvangen ");
    Serial.print(count);
    Serial.print(" byte(s): ");

    for (int i = 0; i < count; i++)
    {
      Serial.print("0x");

      if (localCopy[i] < 16)
      {
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

  // Reset command
  if (resetPuzzleRequested)
  {
    noInterrupts();
    resetPuzzleRequested = false;
    interrupts();

    resetPuzzle();
  }

  // Start puzzle als I2C startcode 48 is ontvangen
  if (startPuzzleRequested)
  {
    noInterrupts();
    startPuzzleRequested = false;
    interrupts();

    startPuzzle();
  }

  // Victory blink bezig
  if (victoryBlinkActive)
  {
    handleVictoryBlink();
    return;
  }

  // Puzzel is klaar
  if (puzzleFinished)
  {
    writeLeds(0x0000);
    delay(100);
    return;
  }

  // Nog niet gestart
  if (!puzzleStarted && !victory)
  {
    idleLedEffect();
    delay(50);
    return;
  }

  // Actief
  runningLedEffect();

  if (millis() - lastPuzzleCheck >= puzzleCheckInterval)
  {
    lastPuzzleCheck = millis();
    checkPuzzle();
  }
}