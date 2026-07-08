#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>
#include "audio.h"

// ---------------- WIFI / GUI ----------------
const char* ssid = "iPhone van Bas";
const char* password = "wachtwoord";

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

// ---------------- AUDIO / BUZZER ----------------
// Puzzelmodules kunnen via I2C een audio-request sturen.
// De main-module speelt het geluid af op deze buzzer-pin.
static const int BUZZER_PIN = 4;

AudioPlayer audio;

// ---------------- SERVO CODE DISPLAY ----------------
// Servo zit op de main-module. Deze toont de 4-cijferige kluiscode
// die vanuit de GUI binnenkomt met: VAULT:1234
static const int SERVO_PIN = 26;

Servo codeServo;

// Pulsbreedtes voor MG90S
static const int SERVO_MIN_US = 500;
static const int SERVO_MAX_US = 2400;

// Hoeken uit servo_test.ino
// index 0 hoort bij cijfer 0, index 1 bij cijfer 1, enz.
static const int SERVO_DIGIT_ANGLES[10] = {
  161,  // 0
  150,  // 1
  136,  // 2
  122,  // 3
  103,  // 4
  83,   // 5
  60,   // 6
  45,   // 7
  32,   // 8
  17    // 9
};

static const unsigned long SERVO_WAIT_BETWEEN_DIGITS_MS = 700;
static const unsigned long SERVO_WAIT_AFTER_CODE_MS = 3000;

String vaultCode = "0000";
bool vaultCodeReceived = false;

bool servoCodeActive = false;
int servoCodeIndex = 0;
unsigned long nextServoMoveMs = 0;

// Wordt true zodra de kluis-oplossing de servo heeft gestart.
// Hierdoor start de servo niet steeds opnieuw door herhaalde I2C state 2 of audio 11.
bool vaultServoStarted = false;


int currentPuzzle = 0;
bool running = false;
bool waitingForStart = true;
bool allPuzzlesComplete = false;

// ---------------- LED STRIPS ----------------
// LET OP: GPIO34 is op veel ESP32 boards input-only.
// Als de LEDs niet werken, gebruik bijvoorbeeld pin 4, 5, 18, 23, 25, 26, 27, 32 of 33.
#define LED_PIN 19

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

// Na het oplossen van de kluis blijft de kluis-module actief voor knop 1 / solenoid.
// De main pollt dan alleen nog audio-requests van de kluis, zodat AUDIO_REQ_SOLENOID_OPEN hoorbaar blijft.
const unsigned long VAULT_AUDIO_POLL_INTERVAL_MS = 80;
unsigned long lastVaultAudioPollMs = 0;

int loopsend = 0;
int currentLedStep = 0;

// ---------------- COMMANDS ----------------
static const int CMD_START_GAME = 100;
static const int CMD_RESET_GAME = 101;
static const int CMD_NEXT_PUZZLE = 69;
static const int CMD_END_GAME = 67;

// Algemeen stopcommando voor alle puzzle-slaves.
// Als een puzzle wordt geskipt, gereset of handmatig gestopt, stuurt de main dit eerst.
static const uint8_t CMD_STOP_PUZZLE = 99;

// Prototypes voor functies die eerder in de file worden aangeroepen.
void sendPuzzleCommand(uint8_t addr, uint8_t cmd, const char* reason);
void stopPuzzleByIndex(int puzzleIndex, const char* reason);
void stopAllPuzzles(const char* reason);
void sendToGui(String msg);
uint8_t readI2C(uint8_t addr);
bool handleAudioRequest(uint8_t value, int fromPuzzle);
void pollVaultAudioAfterComplete();
void startVaultServoOnce(const char* reason);

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

// ---------------- SERVO HELPERS ----------------
void setupCodeServo()
{
  // ESP32Servo gebruikt hardware-timers. Deze allocaties zijn veilig voor normale servo-aansturing.
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  codeServo.setPeriodHertz(50);
  codeServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);

  // Rustpositie: cijfer 0.
  codeServo.write(SERVO_DIGIT_ANGLES[0]);

  Serial.print("Code-servo klaar op GPIO");
  Serial.println(SERVO_PIN);
}

void storeVaultCode(const String& code)
{
  vaultCode = code;
  vaultCodeReceived = true;

  Serial.print("Servo-code opgeslagen: ");
  Serial.println(vaultCode);

  // Als de code gewijzigd wordt terwijl de servo al bezig is,
  // begin opnieuw bij het eerste cijfer van de nieuwe code.
  if (servoCodeActive)
  {
    servoCodeIndex = 0;
    nextServoMoveMs = 0;
    sendToGui("SERVO_CODE_UPDATED:" + vaultCode);
  }
}

void stopServoCodeDisplay()
{
  servoCodeActive = false;
  vaultServoStarted = false;
  servoCodeIndex = 0;
  nextServoMoveMs = 0;

  // Terug naar rustpositie.
  codeServo.write(SERVO_DIGIT_ANGLES[0]);
}

void startServoCodeDisplay()
{
  if (!vaultCodeReceived)
  {
    Serial.println("Geen VAULT-code ontvangen. Servo toont default code 0000.");
    vaultCode = "0000";
  }

  servoCodeActive = true;
  servoCodeIndex = 0;
  nextServoMoveMs = 0;

  Serial.print("Servo start met tonen van code: ");
  Serial.println(vaultCode);

  sendToGui("SERVO_CODE_STARTED:" + vaultCode);
}


void startVaultServoOnce(const char* reason)
{
  if (vaultServoStarted)
    return;

  vaultServoStarted = true;

  Serial.print("Kluis-servo trigger: ");
  Serial.println(reason);

  startServoCodeDisplay();
}

void updateServoCodeDisplay()
{
  if (!servoCodeActive)
    return;

  unsigned long now = millis();

  if (nextServoMoveMs != 0 && now < nextServoMoveMs)
    return;

  if (servoCodeIndex < 0 || servoCodeIndex > 3)
    servoCodeIndex = 0;

  int digit = vaultCode[servoCodeIndex] - '0';

  if (digit < 0 || digit > 9)
  {
    digit = 0;
  }

  int angle = SERVO_DIGIT_ANGLES[digit];
  codeServo.write(angle);

  Serial.print("Servo cijfer ");
  Serial.print(servoCodeIndex + 1);
  Serial.print(": ");
  Serial.print(digit);
  Serial.print(" -> ");
  Serial.print(angle);
  Serial.println(" graden");

  servoCodeIndex++;

  if (servoCodeIndex >= 4)
  {
    servoCodeIndex = 0;
    nextServoMoveMs = now + SERVO_WAIT_AFTER_CODE_MS;
  }
  else
  {
    nextServoMoveMs = now + SERVO_WAIT_BETWEEN_DIGITS_MS;
  }
}

// ---------------- AUDIO HELPERS ----------------
bool handleAudioRequest(uint8_t value, int fromPuzzle)
{
  if (!audio.playRequest(value))
    return false;

  Serial.print("Audio request ontvangen van puzzle ");
  Serial.print(fromPuzzle);
  Serial.print(": ");
  Serial.println(value);

  sendToGui("AUDIO:" + String(value) + ",PUZZLE:" + String(fromPuzzle));

  // Extra robuust voor de kluis:
  // De kluis zet OPEN en queue't direct AUDIO_REQ_VICTORY.
  // Als de main eerst die audio-byte 11 leest, starten we de servo meteen.
  // We hoeven dan niet te wachten tot de volgende poll waarin state 2 terugkomt.
  if (fromPuzzle == 3 && value == AUDIO_REQ_VICTORY)
  {
    startVaultServoOnce("victory audio van kluis");
  }

  return true;
}

void pollVaultAudioAfterComplete()
{
  if (!allPuzzlesComplete || !puzzleSolved[3])
    return;

  if (millis() - lastVaultAudioPollMs < VAULT_AUDIO_POLL_INTERVAL_MS)
    return;

  lastVaultAudioPollMs = millis();

  uint8_t v = readI2C(VAULT_SLAVE_ADDR);

  if (handleAudioRequest(v, 3))
    return;

  // Extra fallback:
  // Als de main in COMPLETE-state zit en alsnog state 2 van de kluis ziet,
  // start de servo alsnog. Dit voorkomt dat de servo uitblijft door timing.
  if (v == 2)
  {
    startVaultServoOnce("kluis state 2 na game complete");
  }
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
  // Belangrijk: eerst alle fysieke puzzelmodules echt uitzetten.
  // Dit voorkomt dat een oude RUNNING/VICTORY-state blijft hangen na reset.
  stopAllPuzzles("reset game");
  audio.stop();

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
  stopServoCodeDisplay();

  Serial.println("Game reset -> waiting for start signal");

  if (notifyGui)
    sendToGui("GAME_RESET");
}

void startGame(bool notifyGui)
{
  // Nieuwe ronde begint altijd vanaf een schone I2C-state.
  // Ook als een vorige ronde handmatig is gestopt of geskipt.
  stopAllPuzzles("start new game");
  audio.stop();

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
  stopServoCodeDisplay();
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

void sendPuzzleCommand(uint8_t addr, uint8_t cmd, const char* reason)
{
  Serial.print("I2C command ");
  Serial.print(cmd);
  Serial.print(" naar slave ");
  Serial.print(addr);
  Serial.print(" (");
  Serial.print(reason);
  Serial.println(")");

  Wire.beginTransmission(addr);
  Wire.write(cmd);
  uint8_t err = Wire.endTransmission();

  if (err == 0)
  {
    Serial.println("  -> I2C command OK");
  }
  else
  {
    Serial.print("  -> I2C foutcode: ");
    Serial.println(err);
  }

  delay(40);
}

void stopPuzzleByIndex(int puzzleIndex, const char* reason)
{
  if (puzzleIndex < 0 || puzzleIndex >= 4)
    return;

  sendPuzzleCommand(SLAVES[puzzleIndex], CMD_STOP_PUZZLE, reason);
}

void stopAllPuzzles(const char* reason)
{
  Serial.print("Alle puzzels stoppen: ");
  Serial.println(reason);

  for (int i = 0; i < 4; i++)
  {
    sendPuzzleCommand(SLAVES[i], CMD_STOP_PUZZLE, reason);
  }
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

  // Deze code wordt op de main-module opgeslagen en later door de servo getoond.
  storeVaultCode(code);

  /*
    Voor nu wordt de code alleen op de main-module gebruikt.
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
    Serial.println("Command 69 -> huidige puzzle skip/completed");

    if (!waitingForStart && !allPuzzlesComplete)
    {
      // Eerst de echte slave stoppen, daarna pas intern naar de volgende puzzel.
      // Hierdoor blijven LEDs, displays, Morse, NeoTrellis, enz. niet actief hangen.
      stopPuzzleByIndex(currentPuzzle, "skip/current puzzle completed");

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
    Serial.println("Command 67 -> game handmatig stoppen / alles completed");

    // Handmatige stop: alle fysieke slaves echt terug naar idle.
    stopAllPuzzles("manual end game");
    stopServoCodeDisplay();
    audio.stop();

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

  // Audio setup
  audio.begin(BUZZER_PIN);

  // WiFi setup
  WiFi.mode(WIFI_STA);
  macString = getMacString();

  // Servo setup
  setupCodeServo();

  resetGameState(false);

  Serial.println("MASTER ready");
}

// ---------------- LOOP ----------------
void loop()
{
  // Audio en servo blijven non-blocking doorlopen.
  audio.update();
  updateServoCodeDisplay();

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
    // Als de kluis al opgelost is, blijft die module actief in OPEN-fase.
    // Daardoor kan knop 1 nog steeds de solenoid openen en audio-request 14 sturen.
    pollVaultAudioAfterComplete();

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

    if (handleAudioRequest(v, currentPuzzle))
    {
      // Belangrijk:
      // Als een puzzel direct na starten al opgelost is, kan de eerste byte
      // een audio-request zijn, bijvoorbeeld AUDIO_REQ_VICTORY.
      // Zet running dan alvast true, anders stuurt de main steeds opnieuw
      // de startcode. Daardoor wordt de softwarepuzzel telkens opnieuw
      // gestart, blijft hij knipperen en speelt het victory-geluid eindeloos.
      running = true;
      delay(80);
      return;
    }

    sendStateToGui(v);

    if (v == 1)
    {
      running = true;
    }
    else if (v == 2)
    {
      // Puzzel was al klaar voordat de main in de normale poll-loop kwam.
      // Handel dit direct hetzelfde af als een normale victory.
      Serial.println("VICTORY direct na start!");

      if (currentPuzzle >= 0 && currentPuzzle < 4)
        puzzleSolved[currentPuzzle] = true;

      if (currentPuzzle == 3)
        startVaultServoOnce("kluis direct na start state 2");

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

    delay(200);
    return;
  }

  // ---------------- POLL ACTIVE PUZZLE ----------------
  uint8_t v = readI2C(addr);

  if (handleAudioRequest(v, currentPuzzle))
  {
    delay(80);
    return;
  }

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

    // Als de kluis-puzzel is opgelost, toont de servo de code uit de GUI.
    // Solenoid wordt in deze versie overgeslagen.
    if (currentPuzzle == 3)
      startVaultServoOnce("kluis normale victory state 2");

    currentPuzzle++;
    running = false;

    if (currentPuzzle >= 4)
    {
      allPuzzlesComplete = true;
      waitingForStart = true;

      sendToGui("ALL_PUZZLES_COMPLETE");
    }

    updatePuzzleLedStrips();

    delay(150);
    return;
  }
  else
  {
    sendStateToGui(v);
  }

  delay(120);
}