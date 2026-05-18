#include <Wire.h>
#include "Adafruit_NeoTrellis.h"

// =====================================================
// PINNEN
// =====================================================
#define SLAVE_SDA   21
#define SLAVE_SCL   22

#define MASTER_SDA  25
#define MASTER_SCL  26

// =====================================================
// I2C ADRESSEN
// =====================================================
#define ESP_SLAVE_ADDR     8       // MOET 9 blijven
#define SENSOR_SLAVE_ADDR  0x55    // extern device op master-bus

// Command om de NeoTrellis puzzle te starten
#define CMD_START_NEOTRELLIS 0x2A
#define CMD_STOP_NEOTRELLIS 99

#define STATE_IDLE     0
#define STATE_RUNNING  1
#define STATE_VICTORY  2


volatile uint8_t responseValue1 = STATE_IDLE;
volatile uint8_t responseValue2 = 0;

// =====================================================
// 2 APARTE I2C CONTROLLERS
// =====================================================
TwoWire I2C_SlaveBus  = TwoWire(0);   // hardware bus 0 -> ESP32 als slave
TwoWire I2C_MasterBus = TwoWire(1);   // hardware bus 1 -> ESP32 als master

// =====================================================
// NEO TRELLIS SETUP
// Let op: NeoTrellis gebruikt de MASTER bus, NIET Wire standaard
// =====================================================
Adafruit_NeoTrellis trellis_array[2][2] = {
  { Adafruit_NeoTrellis(0x2E, &I2C_MasterBus), Adafruit_NeoTrellis(0x2F, &I2C_MasterBus) },
  { Adafruit_NeoTrellis(0x30, &I2C_MasterBus), Adafruit_NeoTrellis(0x32, &I2C_MasterBus) }
};

Adafruit_MultiTrellis trellis((Adafruit_NeoTrellis *)trellis_array, 2, 2);

// =====================================================
// GAME GRID
// =====================================================
#define GRID 8
#define ON   0xFF0000
#define OFF  0x000000

bool board[GRID][GRID];

int start_layout[8][8] = {
  {0,0,1,0,0,0,1,1},
  {0,1,1,0,0,0,0,1},
  {0,0,0,0,1,0,0,0},
  {0,0,1,0,0,0,0,0},
  {1,0,0,0,1,1,0,0},
  {1,1,0,1,0,0,0,0},
  {1,0,1,0,0,1,0,0},
  {0,0,0,1,1,0,0,0}
};

// =====================================================
// STATUS / FLAGS
// =====================================================
volatile bool slaveDataReceived = false;
volatile int  slaveBytesCount   = 0;
volatile uint8_t slaveRxBuffer[32];


// Wordt gezet zodra 0x30 op slave-bus binnenkomt
volatile bool startNeoPuzzleRequested = false;
bool victory = false;

// Puzzelstatus
bool neoPuzzleActive = false;
bool trellisReady = false;

// =====================================================
// MASTER POLL TIMING
// =====================================================
unsigned long lastMasterPoll = 0;
uint8_t masterCommand = 0;

// =====================================================
// FUNCTIE DECLARATIES
// =====================================================
void drawBoard();
void toggleCell(int x, int y);
bool checkWin();
void loadStartLayout();
void clearBoardLeds();
void startNeoPuzzle();
void initNeoTrellis();
TrellisCallback onTrellisPress(keyEvent evt);

// =====================================================
// CALLBACK: master schrijft NAAR ESP32-slave
// =====================================================
void onSlaveReceive(int numBytes)
{
  slaveBytesCount = 0;

  while (I2C_SlaveBus.available() && slaveBytesCount < 32) {
    slaveRxBuffer[slaveBytesCount++] = I2C_SlaveBus.read();
  }

  if (slaveBytesCount > 0) {
    uint8_t cmd = slaveRxBuffer[0];

    if (cmd == CMD_START_NEOTRELLIS) {
      startNeoPuzzleRequested = true;
      responseValue1 = STATE_RUNNING;   // direct melden dat hij running is
      responseValue2 = 0;
    }

  if (cmd == CMD_STOP_NEOTRELLIS) {
      neoPuzzleActive = false;

  responseValue1 = STATE_VICTORY;
  responseValue2 = 0;

  clearBoardLeds();
    }
  

  }

    

  slaveDataReceived = true;
}
// =====================================================
// CALLBACK: master leest VAN ESP32-slave
// =====================================================
void onSlaveRequest()
{
  uint8_t tx[2];
  tx[0] = responseValue1;
  tx[1] = responseValue2;
  I2C_SlaveBus.write(tx, 2);
}

// =====================================================
// NEO TRELLIS CALLBACK
// =====================================================
TrellisCallback onTrellisPress(keyEvent evt)
{
  if (!neoPuzzleActive) {
    return 0;
  }

  if (evt.bit.EDGE == SEESAW_KEYPAD_EDGE_FALLING) {
    uint8_t num = evt.bit.NUM;
    uint8_t x = num % 8;
    uint8_t y = num / 8;

    toggleCell(x, y);
    toggleCell(x + 1, y);
    toggleCell(x - 1, y);
    toggleCell(x, y + 1);
    toggleCell(x, y - 1);

    drawBoard();

    if (checkWin()) {
  Serial.println("[NEOTRELLIS] Puzzle opgelost!");
  responseValue1 = STATE_VICTORY;
  responseValue2 = 0;
  neoPuzzleActive = false;
      for (int i = 0; i < 3; i++) {
        for (int yy = 0; yy < 8; yy++) {
          for (int xx = 0; xx < 8; xx++) {
            trellis.setPixelColor(xx, yy, 0x00FF00);
          }
        }
        trellis.show();
        delay(200);

        for (int yy = 0; yy < 8; yy++) {
          for (int xx = 0; xx < 8; xx++) {
            trellis.setPixelColor(xx, yy, 0x000000);
          }
        }
        trellis.show();
        delay(200);
      }
      clearBoardLeds();
      
    }
  }

  return 0;
}

// =====================================================
// NEO TRELLIS INIT
// =====================================================
void initNeoTrellis()
{
  if (trellisReady) return;

  if (!trellis.begin()) {
    Serial.println("[NEOTRELLIS] FOUT: trellis.begin() mislukt");
    return;
  }

  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      trellis.activateKey(x, y, SEESAW_KEYPAD_EDGE_FALLING);
      trellis.registerCallback(x, y, onTrellisPress);
    }
  }

  trellisReady = true;
  Serial.println("[NEOTRELLIS] Trellis gestart");
}

// =====================================================
// START NEO PUZZLE
// =====================================================
void startNeoPuzzle()
{
  

  if (!trellisReady) {
    initNeoTrellis();
  }

  if (!trellisReady) {
    Serial.println("[NEOTRELLIS] Kan puzzle niet starten, trellis niet beschikbaar");
    return;
  }

  loadStartLayout();
  drawBoard();
  neoPuzzleActive = true;
  responseValue1 = STATE_RUNNING;
  responseValue2 = 0;

  Serial.println("[NEOTRELLIS] Puzzle gestart door I2C command 42");
}

// =====================================================
// GAME FUNCTIES
// =====================================================
void loadStartLayout()
{
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      board[y][x] = start_layout[y][x];
    }
  }
}

void clearBoardLeds()
{
  if (!trellisReady) return;

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      trellis.setPixelColor(x, y, OFF);
    }
  }
  trellis.show();
}

void drawBoard()
{
  if (!trellisReady) return;

  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      trellis.setPixelColor(x, y, board[y][x] ? ON : OFF);
    }
  }

  trellis.show();
}

void toggleCell(int x, int y)
{
  if (x < 0 || x >= 8 || y < 0 || y >= 8) return;
  board[y][x] = !board[y][x];
}

bool checkWin()
{
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      if (board[y][x]) return false;
    }
  }
  return true;
}

// =====================================================
// SETUP
// =====================================================
void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32 dual I2C + NeoTrellis start");
  Serial.println("Bus A = slave");
  Serial.println("Bus B = master + NeoTrellis");
  Serial.print("Slave address = ");
  Serial.println(ESP_SLAVE_ADDR);

  // --------------------------------
  // SLAVE BUS initialiseren
  // begin(adres, sda, scl, freq)
  // --------------------------------
  I2C_SlaveBus.onReceive(onSlaveReceive);
  I2C_SlaveBus.onRequest(onSlaveRequest);

  bool slaveOk = I2C_SlaveBus.begin((uint8_t)ESP_SLAVE_ADDR, SLAVE_SDA, SLAVE_SCL, 100000);
  if (!slaveOk) {
    Serial.println("FOUT: slave bus kon niet gestart worden");
    while (true) { delay(1000); }
  }

  uint8_t initialData[2] = { responseValue1, responseValue2 };
  I2C_SlaveBus.slaveWrite(initialData, 2);

  Serial.println("Slave bus gestart");
  Serial.print("  adres: 0x");
  Serial.println(ESP_SLAVE_ADDR, HEX);
  Serial.print("  SDA: ");
  Serial.println(SLAVE_SDA);
  Serial.print("  SCL: ");
  Serial.println(SLAVE_SCL);

  // --------------------------------
  // MASTER BUS initialiseren
  // begin(sda, scl, freq)
  // --------------------------------
  bool masterOk = I2C_MasterBus.begin(MASTER_SDA, MASTER_SCL, 100000);
  if (!masterOk) {
    Serial.println("FOUT: master bus kon niet gestart worden");
    while (true) { delay(1000); }
  }

  Serial.println("Master bus gestart");
  Serial.print("  SDA: ");
  Serial.println(MASTER_SDA);
  Serial.print("  SCL: ");
  Serial.println(MASTER_SCL);

  // NeoTrellis alvast initialiseren
  initNeoTrellis();

  // Bij opstart LEDs uit laten als trellis aanwezig is
  clearBoardLeds();
  neoPuzzleActive = false;

  Serial.println("Wacht op I2C command 0x30 om NeoTrellis puzzle te starten...");
}

// =====================================================
// LOOP
// =====================================================
void loop()
{
  // --------------------------------
  // 1) Toon ontvangen data op slave-bus
  // --------------------------------
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
      if (localCopy[i] < 16) Serial.print("0");
      Serial.print(localCopy[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
  }

  // --------------------------------
  // 2) Start NeoTrellis puzzle als 0x30 ontvangen is
  // --------------------------------
if (startNeoPuzzleRequested) {
  noInterrupts();
  startNeoPuzzleRequested = false;
  interrupts();

    startNeoPuzzle();
  
}

  // --------------------------------
  // 3) NeoTrellis events lezen als puzzel actief is
  // --------------------------------
  if (neoPuzzleActive && trellisReady) {
    trellis.read();
  }

  // --------------------------------
  // 4) ESP32 werkt als master op andere bus
  //    Schrijf 1 byte, lees daarna 2 bytes terug
  // --------------------------------
  if (millis() - lastMasterPoll >= 1000) {
    lastMasterPoll = millis();

    Serial.print("[MASTER] Schrijf commando naar 0x");
    Serial.print(SENSOR_SLAVE_ADDR, HEX);
    Serial.print(": 0x");
    Serial.println(masterCommand, HEX);

    I2C_MasterBus.beginTransmission(SENSOR_SLAVE_ADDR);
    I2C_MasterBus.write(masterCommand);
    uint8_t err = I2C_MasterBus.endTransmission(true);

    if (err != 0) {
      Serial.print("[MASTER] endTransmission foutcode: ");
      Serial.println(err);
    } else {
      uint8_t bytesRead = I2C_MasterBus.requestFrom((int)SENSOR_SLAVE_ADDR, 2, true);

      Serial.print("[MASTER] Bytes gelezen: ");
      Serial.println(bytesRead);

      if (bytesRead == 2) {
        uint8_t a = I2C_MasterBus.read();
        uint8_t b = I2C_MasterBus.read();

        Serial.print("[MASTER] Antwoord: 0x");
        Serial.print(a, HEX);
        Serial.print(" 0x");
        Serial.println(b, HEX);
      } else {
        while (I2C_MasterBus.available()) {
          I2C_MasterBus.read();
        }
      }
    }

    masterCommand++;
  }

  delay(20);
}