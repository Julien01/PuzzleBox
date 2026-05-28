#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>

// ---------------- WIFI / GUI ----------------
const char* ssid = "iPhone van Bas";
const char* password = "sigmaboys";

static const uint16_t UDP_DISCOVERY_PORT = 4444;
static const uint16_t TCP_SERVER_PORT = 3333;

WiFiUDP udp;
WiFiClient tcpClient;

IPAddress guiIp;
bool guiDiscovered = false;
bool udpStarted = false;

String macString;
String rxBuffer;

// ---------------- I2C PUZZLES ----------------
const uint8_t SLAVES[4] = {8, 9, 10, 11};
const uint8_t START_CODES[4] = {42, 48, 54, 60};

// Kluis zit op slave-adres 11
static const uint8_t VAULT_SLAVE_ADDR = 11;

int currentPuzzle = 0;
bool running = false;
bool waitingForStart = true;
bool allPuzzlesComplete = false;

// ---------------- LED STRIPS ----------------
// LET OP: GPIO34 is op veel ESP32 boards input-only.
// Als de LEDs niet werken, gebruik bijvoorbeeld pin 4, 5, 18, 23, 25, 26, 27, 32 of 33.
#define LED_PIN 25

static const int LEDS_PER_STRIP = 13;
static const int STRIP_COUNT = 4;
static const int LED_COUNT = LEDS_PER_STRIP * STRIP_COUNT;

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_BGR + NEO_KHZ800);

// Fysieke strip-indeling:
// strip 1 = Software
// strip 2 = NeoTrellis
// strip 3 = Hardware
// strip 4 = Game time / blauwe indicator
static const int STRIP_SOFTWARE   = 0; // LED 0  - 12
static const int STRIP_NEOTRELLIS = 1; // LED 13 - 25
static const int STRIP_HARDWARE   = 2; // LED 26 - 38
static const int STRIP_GAME_TIME  = 3; // LED 39 - 51

uint32_t COLOR_OFF;
uint32_t COLOR_RED;
uint32_t COLOR_GREEN;
uint32_t COLOR_BLUE;

// Puzzle-state:
// currentPuzzle 0 = NeoTrellis  -> LED strip 2
// currentPuzzle 1 = Software    -> LED strip 1
// currentPuzzle 2 = Hardware    -> LED strip 3
// currentPuzzle 3 = Kluis       -> geen eigen LED-strip
bool puzzleSolved[4] = {false, false, false, false};

// ---------------- TIMERS ----------------
unsigned long lastDiscoveryMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastTcpConnectAttemptMs = 0;
unsigned long lastAckMs = 0;

const unsigned long DISCOVERY_INTERVAL_MS = 2000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;
const unsigned long TCP_RETRY_INTERVAL_MS = 3000;
const unsigned long ACK_INTERVAL_MS = 5000;

int loopsend = 0;
int currentLedStep = 0;

// ---------------- COMMANDS ----------------
static const int CMD_START_GAME = 100;
static const int CMD_RESET_GAME = 101;
static const int CMD_NEXT_PUZZLE = 69;
static const int CMD_END_GAME = 67;

struct LedRange
{
  int start;
  int count;
};

LedRange getStripRange(int stripIndex)
{
  switch (stripIndex)
  {
    case STRIP_SOFTWARE:
      return {0, 13};      // LED 0  t/m 12

    case STRIP_NEOTRELLIS:
      return {13, 13};     // LED 13 t/m 25

    case STRIP_HARDWARE:
      return {26, 13};     // LED 26 t/m 38

    case STRIP_GAME_TIME:
      return {39, 13};     // LED 39 t/m 51

    default:
      return {-1, 0};
  }
}

void setLedStripColor(int stripIndex, uint32_t color)
{
  if (stripIndex < 0 || stripIndex >= STRIP_COUNT)
    return;

  LedRange range = getStripRange(stripIndex);

  for (int i = 0; i < range.count; i++)
  {
    int ledIndex = range.start + i;

if (ledIndex < 0 || ledIndex >= LED_COUNT)
  continue;

    strip.setPixelColor(ledIndex - 1, color);
  }
}

void clearAllLeds()
{
  for (int i = 0; i < LED_COUNT; i++)
  {
    strip.setPixelColor(i, COLOR_OFF);
  }

  strip.show();
}

int puzzleToLedStrip(int puzzleNumber)
{
  /*
    Puzzle-volgorde:
    0 = NeoTrellis
    1 = Software
    2 = Hardware
    3 = Kluis

    LED-strip-volgorde:
    strip 1 = Software
    strip 2 = NeoTrellis
    strip 3 = Hardware
    strip 4 = Game time

    Dus puzzlevolgorde op LED strips:
    2 -> 1 -> 3
  */

  if (puzzleNumber == 0)
    return STRIP_NEOTRELLIS;

  if (puzzleNumber == 1)
    return STRIP_SOFTWARE;

  if (puzzleNumber == 2)
    return STRIP_HARDWARE;

  return -1; // Kluis heeft geen eigen strip
}

void updatePuzzleLedStrips()
{
  // Eerst alleen de puzzle-strips wissen.
  // De game-time strip wordt apart aangestuurd door LEDSTEP.
  setLedStripColor(STRIP_SOFTWARE, COLOR_OFF);
  setLedStripColor(STRIP_NEOTRELLIS, COLOR_OFF);
  setLedStripColor(STRIP_HARDWARE, COLOR_OFF);

  // Opgeloste puzzels groen.
  for (int p = 0; p < 4; p++)
  {
    int stripIndex = puzzleToLedStrip(p);

    // Kluis heeft geen eigen LED-strip, dus overslaan.
    if (stripIndex < 0 || stripIndex >= STRIP_COUNT)
      continue;

    if (puzzleSolved[p])
      setLedStripColor(stripIndex, COLOR_GREEN);
  }

  // Actieve puzzel rood.
  // Puzzels die nog moeten komen blijven uit.
  if (!waitingForStart && !allPuzzlesComplete && currentPuzzle >= 0 && currentPuzzle < 4)
  {
    int activeStrip = puzzleToLedStrip(currentPuzzle);

    if (activeStrip >= 0 && activeStrip < STRIP_COUNT && !puzzleSolved[currentPuzzle])
      setLedStripColor(activeStrip, COLOR_RED);
  }

  strip.show();
}

void updateGameTimerStrip(int step)
{
  if (step < 0)
    step = 0;

  if (step > LEDS_PER_STRIP)
    step = LEDS_PER_STRIP;

  /*
    Timer-logica:

    step 0  = 13 LEDs aan  -> volledige tijd over
    step 1  = 12 LEDs aan
    step 2  = 11 LEDs aan
    ...
    step 12 = 1 LED aan    -> bijna tijd voorbij
    step 13 = 0 LEDs aan   -> tijd voorbij
  */
  int ledsOn = LEDS_PER_STRIP - step;

  if (ledsOn < 0)
    ledsOn = 0;

  if (ledsOn > LEDS_PER_STRIP)
    ledsOn = LEDS_PER_STRIP;

  LedRange range = getStripRange(STRIP_GAME_TIME);

  for (int i = 0; i < range.count; i++)
  {
    int ledIndex = range.start + i;
if (ledIndex < 0 || ledIndex >= LED_COUNT)
  continue;

    if (i < ledsOn)
      strip.setPixelColor(ledIndex, COLOR_BLUE);
    else
      strip.setPixelColor(ledIndex, COLOR_OFF);
  }

  strip.show();
}

// ---------------- GENERAL HELPERS ----------------
String getMacString()
{
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);

  char buf[18];
  snprintf(
    buf,
    sizeof(buf),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0],
    mac[1],
    mac[2],
    mac[3],
    mac[4],
    mac[5]
  );

  return String(buf);
}

void sendToGui(String msg)
{
  if (!tcpClient.connected())
    return;

  tcpClient.println(msg);
}

void sendAckToGuiIfNeeded()
{
  if (!tcpClient.connected())
    return;

  if (millis() - lastAckMs < ACK_INTERVAL_MS)
    return;

  lastAckMs = millis();

  String msg = "ACK:";

  if (waitingForStart)
    msg += "WAITING";
  else if (allPuzzlesComplete)
    msg += "COMPLETE";
  else if (running)
    msg += "RUNNING";
  else
    msg += "ARMED";

  msg += ",PUZZLE:";
  msg += String(currentPuzzle);

  sendToGui(msg);
}

void sendStateToGui(uint8_t state)
{
  if (loopsend < 4)
  {
    loopsend++;
    return;
  }

  String msg = "PUZZLE:" + String(currentPuzzle) +
               ",STATE:" + String(state);

  sendToGui(msg);
  loopsend = 0;
}

void resetGameState(bool notifyGui)
{
  currentPuzzle = 0;
  running = false;
  waitingForStart = true;
  allPuzzlesComplete = false;
  loopsend = 0;
  currentLedStep = 0;

  for (int i = 0; i < 4; i++)
  {
    puzzleSolved[i] = false;
  }

  clearAllLeds();

  Serial.println("Game reset -> waiting for start signal");

  if (notifyGui)
    sendToGui("GAME_RESET");
}

void startGame(bool notifyGui)
{
  currentPuzzle = 0;
  running = false;
  waitingForStart = false;
  allPuzzlesComplete = false;
  loopsend = 0;
  currentLedStep = 0;

  for (int i = 0; i < 4; i++)
  {
    puzzleSolved[i] = false;
  }

  clearAllLeds();
  updatePuzzleLedStrips();
  updateGameTimerStrip(0);

  Serial.println("Start signal received -> puzzle loop armed");

  if (notifyGui)
    sendToGui("GAME_STARTED");
}

// ---------------- WIFI ----------------
void connectToWiFiIfNeeded()
{
  if (WiFi.status() == WL_CONNECTED)
    return;

  if (millis() - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS)
    return;

  lastWifiAttemptMs = millis();

  Serial.println("WiFi connect...");

  WiFi.disconnect(true, true);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
}

void startUdpIfNeeded()
{
  if (udpStarted)
    return;

  if (WiFi.status() != WL_CONNECTED)
    return;

  if (udp.begin(UDP_DISCOVERY_PORT))
  {
    udpStarted = true;
    Serial.println("UDP ready");
  }
}

// ---------------- GUI DISCOVERY ----------------
void sendDiscovery()
{
  udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_DISCOVERY_PORT);
  udp.print("DISCOVER_GUI");
  udp.endPacket();
}

void handleUdp()
{
  int size = udp.parsePacket();

  if (size <= 0)
    return;

  char buf[128];
  int len = udp.read(buf, sizeof(buf) - 1);

  if (len <= 0)
    return;

  buf[len] = '\0';

  String msg = String(buf);
  msg.trim();

  if (msg.startsWith("GUI_HERE:"))
  {
    guiIp = udp.remoteIP();
    guiDiscovered = true;

    Serial.print("GUI IP: ");
    Serial.println(guiIp);
  }
}

// ---------------- I2C ----------------
uint8_t readI2C(uint8_t addr)
{
  Wire.requestFrom(addr, (uint8_t)1);

  if (Wire.available())
    return Wire.read();

  return 255;
}

bool isValidVaultCode(const String& code)
{
  if (code.length() != 4)
    return false;

  for (int i = 0; i < 4; i++)
  {
    if (!isDigit(code[i]))
      return false;
  }

  return true;
}

void sendVaultCodeToKluis(const String& code)
{
  if (!isValidVaultCode(code))
  {
    Serial.println("Ongeldige kluiscode ontvangen");
    sendToGui("VAULT_ERROR:INVALID_CODE");
    return;
  }

  Serial.print("Kluiscode ontvangen: ");
  Serial.println(code);

  /*
    Voor nu wordt de code alleen geprint.
    Als de kluis-slave later de code moet ontvangen via I2C,
    kun je hier Wire.beginTransmission(VAULT_SLAVE_ADDR) toevoegen.
  */

  sendToGui("VAULT_OK:" + code);
}

void handleLedStepCommand(int step)
{
  if (step < 0)
    step = 0;

  if (step > 13)
    step = 13;

  currentLedStep = step;

  Serial.print("LED timer step ontvangen: ");
  Serial.print(currentLedStep);
  Serial.println(" / 13");

  // Strip 4 = game time / blauwe indicator.
  updateGameTimerStrip(currentLedStep);

  sendToGui("LEDSTEP_OK:" + String(currentLedStep));
}

// ---------------- TCP COMMAND HANDLING ----------------
void handleIncomingLine(const String& line)
{
  Serial.print("TCP ontvangen: ");
  Serial.println(line);

  // --------------------------------------------------
  // Timer-led step vanuit GUI
  // Formaat: LEDSTEP:0 t/m LEDSTEP:13
  // --------------------------------------------------
  if (line.startsWith("LEDSTEP:"))
  {
    int step = line.substring(8).toInt();

    tcpClient.print("OK:");
    tcpClient.println(line);

    handleLedStepCommand(step);
    return;
  }

  // --------------------------------------------------
  // Kluiscode vanuit GUI
  // Formaat: VAULT:1234
  // --------------------------------------------------
  if (line.startsWith("VAULT:"))
  {
    String code = line.substring(6);
    code.trim();

    tcpClient.print("OK:");
    tcpClient.println(line);

    sendVaultCodeToKluis(code);
    return;
  }

  // --------------------------------------------------
  // Numerieke commands vanuit GUI
  // Formaat: NUM:100, NUM:101, NUM:69, NUM:67
  // --------------------------------------------------
  if (!line.startsWith("NUM:"))
  {
    Serial.print("Onbekend TCP commando: ");
    Serial.println(line);

    tcpClient.print("ERR:UNKNOWN_COMMAND:");
    tcpClient.println(line);

    return;
  }

  int value = line.substring(4).toInt();

  Serial.print("Ontvangen nummer: ");
  Serial.println(value);

  tcpClient.print("OK:");
  tcpClient.println(value);

  if (value == CMD_START_GAME)
  {
    startGame(true);
    return;
  }

  if (value == CMD_RESET_GAME)
  {
    resetGameState(true);
    return;
  }

  if (value == CMD_NEXT_PUZZLE)
  {
    Serial.println("Command 69 -> huidige puzzle completed");

    if (!waitingForStart && !allPuzzlesComplete)
    {
      if (currentPuzzle >= 0 && currentPuzzle < 4)
        puzzleSolved[currentPuzzle] = true;

      sendStateToGui(2);

      currentPuzzle++;
      running = false;

      if (currentPuzzle >= 4)
      {
        allPuzzlesComplete = true;
        waitingForStart = true;
        sendToGui("ALL_PUZZLES_COMPLETE");
      }

      updatePuzzleLedStrips();
    }

    return;
  }

  if (value == CMD_END_GAME)
  {
    Serial.println("Command 67 -> alle puzzles completed");

    for (int i = 0; i < 4; i++)
    {
      puzzleSolved[i] = true;
    }

    allPuzzlesComplete = true;
    waitingForStart = true;
    running = false;
    currentPuzzle = 4;

    updatePuzzleLedStrips();
    sendToGui("ALL_PUZZLES_COMPLETE");

    return;
  }

  Serial.print("Onbekend NUM commando: ");
  Serial.println(value);

  tcpClient.print("ERR:UNKNOWN_NUM:");
  tcpClient.println(value);
}

void handleTcpReceive()
{
  if (!tcpClient.connected())
    return;

  while (tcpClient.available())
  {
    char c = (char)tcpClient.read();

    if (c == '\n')
    {
      rxBuffer.trim();

      if (rxBuffer.length() > 0)
        handleIncomingLine(rxBuffer);

      rxBuffer = "";
    }
    else
    {
      rxBuffer += c;

      if (rxBuffer.length() > 200)
      {
        Serial.println("RX buffer te groot, reset buffer");
        rxBuffer = "";
      }
    }
  }
}

void connectToGuiIfNeeded()
{
  if (!guiDiscovered)
    return;

  if (tcpClient.connected())
    return;

  if (millis() - lastTcpConnectAttemptMs < TCP_RETRY_INTERVAL_MS)
    return;

  lastTcpConnectAttemptMs = millis();

  if (tcpClient.connect(guiIp, TCP_SERVER_PORT))
  {
    Serial.println("TCP connected");
    tcpClient.println("HELLO:" + macString);
    lastAckMs = 0;
  }
}

// ---------------- SETUP ----------------
void setup()
{
  Serial.begin(115200);

  // LED setup
  strip.begin();
  strip.setBrightness(50);

  COLOR_OFF = strip.Color(0, 0, 0);
  COLOR_RED = strip.Color(255, 0, 0);
  COLOR_GREEN = strip.Color(0, 255, 0);
  COLOR_BLUE = strip.Color(0, 0, 255);

  clearAllLeds();

  // I2C setup
  Wire.begin(21, 22);
  Wire.setClock(100000);

  // WiFi setup
  WiFi.mode(WIFI_STA);
  macString = getMacString();

  resetGameState(false);

  Serial.println("MASTER ready");
}

// ---------------- LOOP ----------------
void loop()
{
  connectToWiFiIfNeeded();

  if (WiFi.status() == WL_CONNECTED)
  {
    startUdpIfNeeded();
    handleUdp();

    if (!guiDiscovered)
    {
      if (millis() - lastDiscoveryMs > DISCOVERY_INTERVAL_MS)
      {
        lastDiscoveryMs = millis();
        sendDiscovery();
      }

      delay(50);
      return;
    }

    connectToGuiIfNeeded();
    handleTcpReceive();
    sendAckToGuiIfNeeded();
  }

  if (waitingForStart || allPuzzlesComplete)
  {
    delay(100);
    return;
  }

  if (currentPuzzle < 0 || currentPuzzle >= 4)
  {
    allPuzzlesComplete = true;
    waitingForStart = true;

    sendToGui("ALL_PUZZLES_COMPLETE");

    delay(100);
    return;
  }

  uint8_t addr = SLAVES[currentPuzzle];

  // ---------------- START PUZZLE ----------------
  if (!running)
  {
    Serial.print("Start puzzle ");
    Serial.println(currentPuzzle);

    // Zet actieve puzzle-strip rood.
    // Puzzels die nog moeten komen blijven uit.
    updatePuzzleLedStrips();

    uint8_t startCode = START_CODES[currentPuzzle];

    Serial.print("Startcode die verstuurd wordt: ");
    Serial.println(startCode);

    Wire.beginTransmission(addr);
    Wire.write(startCode);
    Wire.endTransmission();

    delay(80);

    uint8_t v = readI2C(addr);

    Serial.print("ACK: ");
    Serial.println(v);

    sendStateToGui(v);

    if (v == 1)
      running = true;

    delay(300);
    return;
  }

  // ---------------- POLL ACTIVE PUZZLE ----------------
  uint8_t v = readI2C(addr);

  if (v == 1)
  {
    Serial.print("Running puzzle ");
    Serial.println(currentPuzzle);

    sendStateToGui(v);
  }
  else if (v == 2)
  {
    Serial.println("VICTORY!");

    if (currentPuzzle >= 0 && currentPuzzle < 4)
      puzzleSolved[currentPuzzle] = true;

    sendStateToGui(2);

    currentPuzzle++;
    running = false;

    if (currentPuzzle >= 4)
    {
      allPuzzlesComplete = true;
      waitingForStart = true;

      sendToGui("ALL_PUZZLES_COMPLETE");
    }

    updatePuzzleLedStrips();

    delay(1000);
    return;
  }
  else
  {
    sendStateToGui(v);
  }

  delay(400);
}