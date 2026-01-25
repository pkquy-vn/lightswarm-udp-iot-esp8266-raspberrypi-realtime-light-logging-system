#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

// ===== Pins (NodeMCU / ESP8266) =====
static const uint8_t PHOTORESISTOR_PIN = A0;
static const uint8_t LED_INDICATOR     = 2;   // GPIO2  (on-board LED, active LOW)  blink by reading
static const uint8_t LED_MASTER        = 16;  // GPIO16 (on-board LED, active LOW) steady ON if Master

// ===== WiFi / UDP =====
const char* ssid     = "TMOBILE";
const char* password = "Uyen2812";

static const uint16_t UDP_PORT = 4210;
static const IPAddress BROADCAST_IP(255, 255, 255, 255);
WiFiUDP udp;

// ===== Timing =====
static const uint32_t SILENT_MS = 200;
static const uint32_t STATUS_PRINT_MS = 1000;

// ===== Packet delimiters =====
static const char* ESP_START = "~~~";
static const char* ESP_END   = "---";
static const char* RPI_START = "+++";
static const char* RPI_END   = "***";

// ===== Device state =====
static const int MAX_SWARM = 10;

int swarmID = -1;
int analogValue = 0;
int readings[MAX_SWARM];

uint32_t lastReceivedTime = 0;

// ===== LED flashing mapping (same mapping you used) =====
static const int X1 = 24;
static const int Y1 = 2010;
static const int X2 = 1024;
static const int Y2 = 10;

int slope = 0;
int intercept = 0;

// ===== LED states/timers =====
bool ledIndicatorState = LOW;
uint32_t ledIndicatorPrevMs = 0;

// ===== Logging state =====
bool isMaster = true;
bool prevIsMaster = true;

uint32_t lastStatusPrint = 0;

static inline uint32_t nowMs() {
  return millis();
}

static void computeSlopeIntercept(int x1, int y1, int x2, int y2, int* a, int* b) {
  *a = (y2 - y1) / (x2 - x1);
  *b = y1 - (*a) * x1;
}

static int clampInterval(int intervalMs) {
  if (intervalMs < 5) return 5;
  if (intervalMs > 5000) return 5000;
  return intervalMs;
}

static void flashIndicatorByReading(int analogVal) {
  int interval = slope * analogVal + intercept;
  interval = clampInterval(interval);

  uint32_t t = nowMs();
  if (t - ledIndicatorPrevMs >= (uint32_t)interval) {
    ledIndicatorPrevMs = t;
    ledIndicatorState = !ledIndicatorState;
    digitalWrite(LED_INDICATOR, ledIndicatorState);
  }
}

static void printRoleChangeIfNeeded(bool currentIsMaster, int value) {
  if (currentIsMaster == prevIsMaster) return;
  prevIsMaster = currentIsMaster;

  Serial.printf("[%lu] ROLE %s  id=%d  value=%d\n",
                (unsigned long)nowMs(),
                currentIsMaster ? "MASTER" : "SLAVE",
                swarmID,
                value);
}

static void printResetEvent() {
  Serial.printf("[%lu] EVENT reset_requested_by_rpi  id=%d\n",
                (unsigned long)nowMs(),
                swarmID);
}

static void printStatusIfDue(bool currentIsMaster, int value) {
  uint32_t t = nowMs();
  if (t - lastStatusPrint < STATUS_PRINT_MS) return;
  lastStatusPrint = t;

  Serial.printf("[%lu] STATUS id=%d role=%s value=%d\n",
                (unsigned long)t,
                swarmID,
                currentIsMaster ? "MASTER" : "SLAVE",
                value);
}

static bool startsWithEndsWith(const String& s, const char* start, const char* end) {
  return s.startsWith(start) && s.endsWith(end);
}

void setup() {
  Serial.begin(115200);
  delay(10);

  pinMode(LED_INDICATOR, OUTPUT);
  pinMode(LED_MASTER, OUTPUT);

  // active LOW LEDs: HIGH = off
  digitalWrite(LED_INDICATOR, HIGH);
  digitalWrite(LED_MASTER, HIGH);

  for (int i = 0; i < MAX_SWARM; i++) readings[i] = -1;

  computeSlopeIntercept(X1, Y1, X2, Y2, &slope, &intercept);

  WiFi.begin(ssid, password);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  IPAddress ip = WiFi.localIP();
  swarmID = ip[3] % 10;

  Serial.printf("WiFi OK  ip=%d.%d.%d.%d  id=%d  port=%u\n",
                ip[0], ip[1], ip[2], ip[3],
                swarmID,
                UDP_PORT);

  udp.begin(UDP_PORT);

  lastReceivedTime = nowMs();
  lastStatusPrint = nowMs();

  isMaster = true;
  prevIsMaster = true;
}

void loop() {
  // Indicator LED always blinks based on last known analogValue
  flashIndicatorByReading(analogValue);

  // Master LED steady ON if Master, otherwise OFF
  digitalWrite(LED_MASTER, isMaster ? LOW : HIGH);

  // ===== Receive packets =====
  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    char incoming[255];
    int len = udp.read(incoming, 254);
    if (len > 0) incoming[len] = '\0';

    String pkt(incoming);

    // ESP -> ESP: ~~~<id>,<reading>---
    if (startsWithEndsWith(pkt, ESP_START, ESP_END)) {
      String data = pkt.substring(strlen(ESP_START), pkt.length() - strlen(ESP_END));
      int rid = -1, rval = -1;
      if (sscanf(data.c_str(), "%d,%d", &rid, &rval) == 2) {
        if (rid >= 0 && rid < MAX_SWARM) {
          readings[rid] = rval;
          lastReceivedTime = nowMs();
        }
      }
    }

    // RPi reset: +++RESET_REQUESTED***
    if (startsWithEndsWith(pkt, RPI_START, RPI_END)) {
      String data = pkt.substring(strlen(RPI_START), pkt.length() - strlen(RPI_END));
      if (data == "RESET_REQUESTED") {
        // Turn both LEDs OFF immediately (active LOW)
        digitalWrite(LED_INDICATOR, HIGH);
        digitalWrite(LED_MASTER, HIGH);

        // Reset state
        isMaster = true;
        prevIsMaster = true;
        for (int i = 0; i < MAX_SWARM; i++) readings[i] = -1;

        printResetEvent();
        delay(3000);

        lastReceivedTime = nowMs();
      }
    }
  }

  // ===== If silent for 200ms, read sensor and broadcast =====
  if (nowMs() - lastReceivedTime > SILENT_MS) {
    analogValue = analogRead(PHOTORESISTOR_PIN);

    // ESP -> ESP broadcast
    char espMsg[64];
    snprintf(espMsg, sizeof(espMsg), "%s%d,%d%s", ESP_START, swarmID, analogValue, ESP_END);
    udp.beginPacket(BROADCAST_IP, UDP_PORT);
    udp.write((const uint8_t*)espMsg, strlen(espMsg));
    udp.endPacket();

    lastReceivedTime = nowMs();

    // Decide Master (highest reading)
    isMaster = true;
    for (int i = 0; i < MAX_SWARM; i++) {
      if (i == swarmID) continue;
      if (readings[i] >= 0 && readings[i] > analogValue) {
        isMaster = false;
        break;
      }
    }

    // Master -> RPi broadcast
    if (isMaster) {
      char rpiMsg[80];
      snprintf(rpiMsg, sizeof(rpiMsg), "%sMaster,%d,%d%s", RPI_START, swarmID, analogValue, RPI_END);
      udp.beginPacket(BROADCAST_IP, UDP_PORT);
      udp.write((const uint8_t*)rpiMsg, strlen(rpiMsg));
      udp.endPacket();
    }

    // ===== Logs (minimal) =====
    printRoleChangeIfNeeded(isMaster, analogValue);
    printStatusIfDue(isMaster, analogValue);
  }
}
