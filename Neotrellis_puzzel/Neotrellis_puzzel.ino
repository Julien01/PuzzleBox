#include <Wire.h>
#include "Adafruit_NeoTrellis.h"

/************* I2C PINNEN *************/

// I2C slave-bus: hierop luistert deze ESP32 naar startcommando's
#define SLAVE_SDA 26
#define SLAVE_SCL 27

// I2C master-bus: hiermee stuurt deze ESP32 de NeoTrellis aan
#define TRELLIS_SDA 25
#define TRELLIS_SCL 32

/************* I2C ADRES EN COMMANDO'S *************/

#define ESP_SLAVE_ADDR 8

// Startcommando voor de NeoTrellis-puzzel
#define CMD_START_NEOTRELLIS 0x2A   // decimaal 42
#define CMD_STOP_NEOTRELLIS  99
#define CMD_RESET_NEOTRELLIS 101

/************* STATUSWAARDES *************/

#define STATE_IDLE     0
#define STATE_RUNNING  1
#define STATE_VICTORY  2

// Audio request codes voor de main-module
#define AUDIO_REQ_BUTTON        10
#define AUDIO_REQ_VICTORY       11
#define AUDIO_REQ_MORSE_DOT     12
#define AUDIO_REQ_MORSE_DASH    13
#define AUDIO_REQ_SOLENOID_OPEN 14

#define AUDIO_QUEUE_SIZE 8
volatile uint8_t audioQueue[AUDIO_QUEUE_SIZE];
volatile uint8_t audioHead = 0;
volatile uint8_t audioTail = 0;

volatile uint8_t responseValue1 = STATE_IDLE;
volatile uint8_t responseValue2 = 0;

/************* TWEE I2C BUSSEN *************/

// Bus 0 = ESP32 als I2C slave
TwoWire I2C_SlaveBus = TwoWire(0);

// Bus 1 = ESP32 als I2C master voor NeoTrellis
TwoWire I2C_TrellisBus = TwoWire(1);

/************* TRELLIS SETUP *************/

// 4 NeoTrellis borden van 4x4 = samen 8x8
Adafruit_NeoTrellis trellis_array[2][2] = {
  { Adafruit_NeoTrellis(0x2E, &I2C_TrellisBus), Adafruit_NeoTrellis(0x2F, &I2C_TrellisBus) },
  { Adafruit_NeoTrellis(0x30, &I2C_TrellisBus), Adafruit_NeoTrellis(0x32, &I2C_TrellisBus) }
};

Adafruit_MultiTrellis trellis((Adafruit_NeoTrellis *)trellis_array, 2, 2);

/************* GAME GRID *************/

#define GRID 8

/************* LED KLEUREN *************/

#define COLOR_ON_BASE   0x350000   // zwak rood
#define COLOR_WIN_BASE  0x003500   // zwak groen
#define COLOR_OFF       0x000000

/************* GAME STATE *************/

bool board[GRID][GRID];

bool neoPuzzleActive = false;
bool trellisReady = false;

volatile bool startNeoPuzzleRequested = false;
volatile bool stopNeoPuzzleRequested = false;

volatile bool slaveDataReceived = false;
volatile int slaveBytesCount = 0;
volatile uint8_t slaveRxBuffer[32];

/************* STARTING LAYOUT *************/
/*
  1 = LED aan
  0 = LED uit
*/

const int start_layout[GRID][GRID] = {
  {0,0,1,0,0,0,1,1},
  {0,1,1,0,0,0,0,1},
  {0,0,0,0,1,0,0,0},
  {0,0,1,0,0,0,0,0},
  {1,0,0,0,1,1,0,0},
  {1,1,0,1,0,0,0,0},
  {1,0,1,0,0,1,0,0},
  {0,0,0,1,1,0,0,0}
};

/************* FUNCTION DECLARATIONS *************/

void initNeoTrellis();
void startNeoPuzzle();
void stopNeoPuzzle();

void loadStartLayout();
void drawBoard();
void clearDisplay();
void toggleCell(int x, int y);
bool checkWin();
void victoryFlash();

void onSlaveReceive(int numBytes);
void onSlaveRequest();
void queueAudioRequest(uint8_t requestCode);
uint8_t getNextAudioOrState(uint8_t normalState);
void clearAudioQueue();

TrellisCallback onButtonPress(keyEvent evt);

/************* AUDIO REQUEST QUEUE *************/

void queueAudioRequest(uint8_t requestCode) {
  uint8_t nextHead = (audioHead + 1) % AUDIO_QUEUE_SIZE;

  // Als de queue vol is, wordt het nieuwste geluid genegeerd.
  // Zo blokkeert audio nooit de puzzelstatus.
  if (nextHead == audioTail) {
    return;
  }

  audioQueue[audioHead] = requestCode;
  audioHead = nextHead;
}

uint8_t getNextAudioOrState(uint8_t normalState) {
  if (audioTail != audioHead) {
    uint8_t requestCode = audioQueue[audioTail];
    audioTail = (audioTail + 1) % AUDIO_QUEUE_SIZE;
    return requestCode;
  }

  return normalState;
}

void clearAudioQueue() {
  audioHead = 0;
  audioTail = 0;
}

/************* I2C SLAVE RECEIVE CALLBACK *************/

void onSlaveReceive(int numBytes) {
  slaveBytesCount = 0;

  while (I2C_SlaveBus.available() && slaveBytesCount < 32) {
    slaveRxBuffer[slaveBytesCount++] = I2C_SlaveBus.read();
  }

  if (slaveBytesCount > 0) {
    uint8_t cmd = slaveRxBuffer[0];

    if (cmd == CMD_START_NEOTRELLIS) {
      clearAudioQueue();

      // Start en stop/reset mogen niet tegelijk blijven hangen.
      stopNeoPuzzleRequested = false;
      startNeoPuzzleRequested = true;

      responseValue1 = STATE_RUNNING;
      responseValue2 = 0;
    }

    if (cmd == CMD_STOP_NEOTRELLIS || cmd == CMD_RESET_NEOTRELLIS) {
      clearAudioQueue();

      // Reset/stop heeft voorrang op een oude start-request.
      startNeoPuzzleRequested = false;
      stopNeoPuzzleRequested = true;

      // Directe ACK: main mag meteen zien dat de slave naar IDLE gaat.
      responseValue1 = STATE_IDLE;
      responseValue2 = 0;
    }
  }

  slaveDataReceived = true;
}

/************* I2C SLAVE REQUEST CALLBACK *************/

void onSlaveRequest() {
  uint8_t tx[2];

  tx[0] = getNextAudioOrState(responseValue1);
  tx[1] = responseValue2;

  I2C_SlaveBus.write(tx, 2);
}

/************* TRELLIS CALLBACK *************/

TrellisCallback onButtonPress(keyEvent evt) {

  if (!neoPuzzleActive) {
    return 0;
  }

  if (evt.bit.EDGE == SEESAW_KEYPAD_EDGE_FALLING) {

    uint8_t num = evt.bit.NUM;

    int x = num % GRID;
    int y = num / GRID;

    Serial.print("Button pressed: ");
    Serial.print(num);
    Serial.print("  x=");
    Serial.print(x);
    Serial.print(" y=");
    Serial.println(y);

    queueAudioRequest(AUDIO_REQ_BUTTON);

    // Lights Out werking:
    // knop zelf + boven/onder/links/rechts toggelen
    toggleCell(x, y);
    toggleCell(x + 1, y);
    toggleCell(x - 1, y);
    toggleCell(x, y + 1);
    toggleCell(x, y - 1);

    drawBoard();

    if (checkWin()) {
      Serial.println("[NEOTRELLIS] Puzzle opgelost!");

      queueAudioRequest(AUDIO_REQ_VICTORY);

      responseValue1 = STATE_VICTORY;
      responseValue2 = 0;
      neoPuzzleActive = false;

      victoryFlash();
      clearDisplay();
    }
  }

  return 0;
}

/************* SETUP *************/

void setup() {

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32 NeoTrellis puzzle met I2C startcommando");
  Serial.println("--------------------------------------------");

  /************* SLAVE BUS STARTEN *************/

  I2C_SlaveBus.onReceive(onSlaveReceive);
  I2C_SlaveBus.onRequest(onSlaveRequest);

  bool slaveOk = I2C_SlaveBus.begin(
    (uint8_t)ESP_SLAVE_ADDR,
    SLAVE_SDA,
    SLAVE_SCL,
    100000
  );

  if (!slaveOk) {
    Serial.println("FOUT: I2C slave-bus kon niet gestart worden.");
    while (true) {
      delay(1000);
    }
  }

  uint8_t initialData[2] = { responseValue1, responseValue2 };
  I2C_SlaveBus.slaveWrite(initialData, 2);

  Serial.println("I2C slave-bus gestart.");
  Serial.print("Slave adres: 0x");
  Serial.println(ESP_SLAVE_ADDR, HEX);
  Serial.print("Slave SDA: GPIO ");
  Serial.println(SLAVE_SDA);
  Serial.print("Slave SCL: GPIO ");
  Serial.println(SLAVE_SCL);

  /************* TRELLIS BUS STARTEN *************/

  bool trellisBusOk = I2C_TrellisBus.begin(
    TRELLIS_SDA,
    TRELLIS_SCL,
    100000
  );

  if (!trellisBusOk) {
    Serial.println("FOUT: I2C Trellis-bus kon niet gestart worden.");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("I2C Trellis-bus gestart.");
  Serial.print("Trellis SDA: GPIO ");
  Serial.println(TRELLIS_SDA);
  Serial.print("Trellis SCL: GPIO ");
  Serial.println(TRELLIS_SCL);

  /************* TRELLIS INITIALISEREN *************/

  initNeoTrellis();

  // Bij opstart niets tonen.
  clearDisplay();

  neoPuzzleActive = false;
  responseValue1 = STATE_IDLE;
  responseValue2 = 0;

  Serial.println();
  Serial.println("Klaar.");
  Serial.println("Wacht op I2C startcommando 0x2A...");
}

/************* LOOP *************/

void loop() {

  /************* DEBUG: ONTVANGEN I2C DATA TONEN *************/

  if (slaveDataReceived) {
    noInterrupts();

    int count = slaveBytesCount;
    uint8_t localCopy[32];

    for (int i = 0; i < count; i++) {
      localCopy[i] = slaveRxBuffer[i];
    }

    slaveDataReceived = false;

    interrupts();

    Serial.print("[I2C SLAVE] Ontvangen ");
    Serial.print(count);
    Serial.print(" byte(s): ");

    for (int i = 0; i < count; i++) {
      Serial.print("0x");

      if (localCopy[i] < 16) {
        Serial.print("0");
      }

      Serial.print(localCopy[i], HEX);
      Serial.print(" ");
    }

    Serial.println();
  }

  /************* STOPCOMMANDO AFHANDELEN *************/

  if (stopNeoPuzzleRequested) {
    noInterrupts();
    stopNeoPuzzleRequested = false;
    interrupts();

    stopNeoPuzzle();
  }

  /************* STARTCOMMANDO AFHANDELEN *************/

  if (startNeoPuzzleRequested) {
    noInterrupts();
    startNeoPuzzleRequested = false;
    interrupts();

    startNeoPuzzle();
  }

  /************* TRELLIS KNOPPEN LEZEN *************/

  if (neoPuzzleActive && trellisReady) {
    trellis.read();
  }

  delay(10);
}

/************* NEO TRELLIS INIT *************/

void initNeoTrellis() {

  if (trellisReady) {
    return;
  }

  Serial.println("[NEOTRELLIS] Initialiseren...");

  if (!trellis.begin()) {
    Serial.println("[NEOTRELLIS] FOUT: trellis.begin() mislukt.");
    Serial.println("Controleer:");
    Serial.print("- SDA naar GPIO ");
    Serial.println(TRELLIS_SDA);
    Serial.print("- SCL naar GPIO ");
    Serial.println(TRELLIS_SCL);
    Serial.println("- GND aangesloten");
    Serial.println("- Voeding aangesloten");
    Serial.println("- I2C-adressen correct: 0x2E, 0x2F, 0x30, 0x32");
    return;
  }

  for (uint8_t y = 0; y < GRID; y++) {
    for (uint8_t x = 0; x < GRID; x++) {
      trellis.activateKey(x, y, SEESAW_KEYPAD_EDGE_FALLING, true);
      trellis.registerCallback(x, y, onButtonPress);
    }
  }

  trellisReady = true;

  Serial.println("[NEOTRELLIS] Trellis gestart.");
}

/************* PUZZLE STARTEN *************/

void startNeoPuzzle() {

  Serial.println("[NEOTRELLIS] Startcommando ontvangen.");

  if (!trellisReady) {
    initNeoTrellis();
  }

  if (!trellisReady) {
    Serial.println("[NEOTRELLIS] Kan puzzle niet starten. Trellis niet beschikbaar.");
    responseValue1 = STATE_IDLE;
    responseValue2 = 1;
    return;
  }

  clearAudioQueue();

  loadStartLayout();
  drawBoard();

  neoPuzzleActive = true;

  responseValue1 = STATE_RUNNING;
  responseValue2 = 0;

  Serial.println("[NEOTRELLIS] Puzzle gestart.");
}

/************* PUZZLE STOPPEN *************/

void stopNeoPuzzle() {

  Serial.println("[NEOTRELLIS] Stopcommando ontvangen.");

  startNeoPuzzleRequested = false;
  stopNeoPuzzleRequested = false;

  neoPuzzleActive = false;
  clearAudioQueue();

  clearDisplay();

  responseValue1 = STATE_IDLE;
  responseValue2 = 0;

  Serial.println("[NEOTRELLIS] Puzzle gestopt.");
}

/************* LOAD START LAYOUT *************/

void loadStartLayout() {
  for (int y = 0; y < GRID; y++) {
    for (int x = 0; x < GRID; x++) {
      board[y][x] = start_layout[y][x];
    }
  }
}

/************* DRAW BOARD *************/

void drawBoard() {

  if (!trellisReady) {
    return;
  }

  for (int y = 0; y < GRID; y++) {
    for (int x = 0; x < GRID; x++) {

      if (board[y][x]) {
        trellis.setPixelColor(x, y, COLOR_ON_BASE);
      } else {
        trellis.setPixelColor(x, y, COLOR_OFF);
      }

    }
  }

  trellis.show();
}

/************* CLEAR DISPLAY *************/

void clearDisplay() {

  if (!trellisReady) {
    return;
  }

  for (int y = 0; y < GRID; y++) {
    for (int x = 0; x < GRID; x++) {
      trellis.setPixelColor(x, y, COLOR_OFF);
    }
  }

  trellis.show();
}

/************* TOGGLE FUNCTION *************/

void toggleCell(int x, int y) {

  if (x < 0 || x >= GRID || y < 0 || y >= GRID) {
    return;
  }

  board[y][x] = !board[y][x];
}

/************* CHECK WIN *************/

bool checkWin() {

  for (int y = 0; y < GRID; y++) {
    for (int x = 0; x < GRID; x++) {

      if (board[y][x]) {
        return false;
      }

    }
  }

  return true;
}

/************* VICTORY FLASH *************/

void victoryFlash() {

  if (!trellisReady) {
    return;
  }

  for (int i = 0; i < 3; i++) {

    for (int y = 0; y < GRID; y++) {
      for (int x = 0; x < GRID; x++) {
        trellis.setPixelColor(x, y, COLOR_WIN_BASE);
      }
    }

    trellis.show();
    delay(200);

    clearDisplay();
    delay(200);
  }
}