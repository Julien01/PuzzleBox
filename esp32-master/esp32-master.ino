#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <esp_wifi.h>

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

int currentPuzzle = 0;
bool running = false;
bool waitingForStart = true;
bool allPuzzlesComplete = false;

// ---------------- TIMERS ----------------
unsigned long lastDiscoveryMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastTcpConnectAttemptMs = 0;

const unsigned long DISCOVERY_INTERVAL_MS = 2000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;
const unsigned long TCP_RETRY_INTERVAL_MS = 3000;

int loopsend = 0;

// ---------------- COMMANDS ----------------
static const int CMD_START_GAME = 100;
static const int CMD_RESET_GAME = 101;
static const int CMD_NEXT_PUZZLE = 69;
static const int CMD_END_GAME = 67;

// Extra I2C-command naar de actieve puzzle
static const uint8_t CMD_STOP_PUZZLE = 99;

// ---------------- HELPERS ----------------
String getMacString()
{
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);

  char buf[18];
  snprintf(buf, sizeof(buf),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);

  return String(buf);
}

void sendToGui(String msg)
{
  if (!tcpClient.connected()) return;
  tcpClient.println(msg);
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

// ---------------- I2C ----------------
uint8_t readI2C(uint8_t addr)
{
  Wire.requestFrom(addr, (uint8_t)1);

  if (Wire.available())
  {
    return Wire.read();
  }

  return 255;
}

void sendStopToCurrentPuzzle()
{
  if (currentPuzzle < 0 || currentPuzzle >= 4)
  {
    Serial.println("Geen geldige huidige puzzle om te stoppen");
    return;
  }

  uint8_t addr = SLAVES[currentPuzzle];

  Serial.print("STOP sturen naar puzzle ");
  Serial.print(currentPuzzle);
  Serial.print(" op I2C adres ");
  Serial.println(addr);

  Wire.beginTransmission(addr);
  Wire.write(CMD_STOP_PUZZLE);
  byte error = Wire.endTransmission();

  if (error == 0)
  {
    Serial.println("STOP succesvol verstuurd");
  }
  else
  {
    Serial.print("Fout bij STOP versturen, I2C error: ");
    Serial.println(error);
  }
}

// ---------------- GAME STATE ----------------
void resetGameState(bool notifyGui)
{
  if (!waitingForStart && !allPuzzlesComplete)
  {
    sendStopToCurrentPuzzle();
    delay(50);
  }

  currentPuzzle = 0;
  running = false;
  waitingForStart = true;
  allPuzzlesComplete = false;
  loopsend = 0;

  Serial.println("Game reset -> waiting for start signal");

  if (notifyGui)
  {
    sendToGui("GAME_RESET");
  }
}

void startGame(bool notifyGui)
{
  currentPuzzle = 0;
  running = false;
  waitingForStart = false;
  allPuzzlesComplete = false;
  loopsend = 0;

  Serial.println("Start signal received -> puzzle loop armed");

  if (notifyGui)
  {
    sendToGui("GAME_STARTED");
  }
}

// ---------------- WIFI ----------------
void connectToWiFiIfNeeded()
{
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;

  lastWifiAttemptMs = millis();

  Serial.println("WiFi connect...");

  WiFi.disconnect(true, true);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
}

void startUdpIfNeeded()
{
  if (udpStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

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
  if (size <= 0) return;

  char buf[128];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;

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

// ---------------- TCP COMMANDS ----------------
void handleIncomingLine(const String& line)
{
  Serial.print("TCP ontvangen: ");
  Serial.println(line);

  if (!line.startsWith("NUM:"))
  {
    return;
  }

  int value = line.substring(4).toInt();

  Serial.print("Ontvangen nummer: ");
  Serial.println(value);

  tcpClient.print("OK:");
  tcpClient.println(value);

  // ---------------- START GAME ----------------
  if (value == CMD_START_GAME)
  {
    startGame(true);
    return;
  }

  // ---------------- RESET GAME ----------------
  if (value == CMD_RESET_GAME)
  {
    resetGameState(true);
    return;
  }

  // ---------------- NEXT PUZZLE ----------------
  if (value == CMD_NEXT_PUZZLE)
  {
    Serial.println("Command 69 -> huidige puzzle stoppen en naar volgende puzzle");

    if (!waitingForStart && !allPuzzlesComplete)
    {
      // Eerst huidige puzzle stoppen
      sendStopToCurrentPuzzle();
      delay(50);

      // Daarna huidige puzzle als afgerond melden
      sendStateToGui(2);

      // Naar volgende puzzle
      currentPuzzle++;
      running = false;
      loopsend = 0;

      if (currentPuzzle >= 4)
      {
        allPuzzlesComplete = true;
        waitingForStart = true;
        sendToGui("ALL_PUZZLES_COMPLETE");
      }
    }

    return;
  }

  // ---------------- END GAME ----------------
  if (value == CMD_END_GAME)
  {
    Serial.println("Command 67 -> huidige puzzle stoppen en spel beëindigen");

    if (!waitingForStart && !allPuzzlesComplete)
    {
      sendStopToCurrentPuzzle();
      delay(50);
    }

    allPuzzlesComplete = true;
    waitingForStart = true;
    running = false;
    currentPuzzle = 4;
    loopsend = 0;

    sendToGui("ALL_PUZZLES_COMPLETE");
    return;
  }
}

void handleTcpReceive()
{
  if (!tcpClient.connected()) return;

  while (tcpClient.available())
  {
    char c = (char)tcpClient.read();

    if (c == '\n')
    {
      rxBuffer.trim();

      if (rxBuffer.length() > 0)
      {
        handleIncomingLine(rxBuffer);
      }

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
  if (!guiDiscovered) return;
  if (tcpClient.connected()) return;
  if (millis() - lastTcpConnectAttemptMs < TCP_RETRY_INTERVAL_MS) return;

  lastTcpConnectAttemptMs = millis();

  if (tcpClient.connect(guiIp, TCP_SERVER_PORT))
  {
    Serial.println("TCP connected");
    tcpClient.println("HELLO:" + macString);
  }
}

// ---------------- SETUP ----------------
void setup()
{
  Serial.begin(115200);

  Wire.begin(21, 22);
  Wire.setClock(100000);

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

  // ---------------- START CURRENT PUZZLE ----------------
  if (!running)
  {
    Serial.print("Start puzzle ");
    Serial.println(currentPuzzle);

    uint8_t startCode = START_CODES[currentPuzzle];

    Serial.print("Startcode die verstuurd wordt: ");
    Serial.println(startCode);

    Wire.beginTransmission(addr);
    Wire.write(startCode);
    byte error = Wire.endTransmission();

    if (error == 0)
    {
      Serial.println("Startcode succesvol verstuurd");
    }
    else
    {
      Serial.print("Fout bij startcode versturen, I2C error: ");
      Serial.println(error);
    }

    delay(80);

    uint8_t v = readI2C(addr);

    Serial.print("ACK: ");
    Serial.println(v);

    sendStateToGui(v);

    if (v == 1)
    {
      running = true;
    }

    delay(300);
    return;
  }

  // ---------------- POLL CURRENT PUZZLE ----------------
  uint8_t v = readI2C(addr);

  if (v == 1)
  {
    Serial.print("Running puzzle ");
    Serial.println(currentPuzzle);
  }
  else if (v == 2)
  {
    Serial.println("VICTORY!");

    sendStateToGui(2);

    currentPuzzle++;
    running = false;
    loopsend = 0;

    if (currentPuzzle >= 4)
    {
      allPuzzlesComplete = true;
      waitingForStart = true;
      sendToGui("ALL_PUZZLES_COMPLETE");
    }

    delay(1000);
    return;
  }
  else if (v == 0)
  {
    Serial.print("Puzzle ");
    Serial.print(currentPuzzle);
    Serial.println(" geeft IDLE terug");
  }
  else if (v == 255)
  {
    Serial.print("Geen geldige I2C reactie van puzzle ");
    Serial.println(currentPuzzle);
  }

  delay(400);
}