#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

// ===== Pins (NodeMCU / ESP8266) =====
#define PHOTORESISTOR_PIN A0
#define LED_INDICATOR 2   // GPIO2  (on-board LED, active LOW)
#define LED_MASTER   16   // GPIO16 (on-board LED, active LOW)

// ===== WiFi / UDP =====
const char* ssid     = "TMOBILE";
const char* password = "Uyen2812";

const unsigned int localPort = 4210;
const IPAddress broadcastIP(255, 255, 255, 255);
WiFiUDP udp;

// ===== Timing =====
unsigned long lastReceivedTime = 0;
const unsigned long silentTime = 200;

// ===== Device state =====
int swarmID = -1;
int analogValue = 0;
bool isMaster = true;
String role = "Master";

// Store other devices readings by swarmID (0..9)
int readings[10];

// ===== Packet delimiters =====
const String ESP_startBit = "~~~";
const String ESP_endBit   = "---";
const String RPi_startBit = "+++";
const String RPi_endBit   = "***";

// ===== LED flashing mapping =====
int Y_threshold = 24;
int Y_interval  = 2010;
int Z_threshold = 1024;
int Z_interval  = 10;
int slope, intercept;

// ===== LED states/timers =====
bool ledIndicatorState = LOW;
unsigned long ledIndicatorPreviousMillis = 0;

bool ledMasterState = LOW;
unsigned long ledMasterPreviousMillis = 0;

void getSlopeIntercept(int x1, int y1, int x2, int y2, int *a, int *b) {
  *a = (y2 - y1) / (x2 - x1);
  *b = y1 - (*a) * x1;
}

void ledIndicatorFlash(int analog_value) {
  int ledInterval = slope * analog_value + intercept;
  unsigned long currentMillis = millis();
  if (currentMillis - ledIndicatorPreviousMillis >= (unsigned long)ledInterval) {
    ledIndicatorPreviousMillis = currentMillis;
    ledIndicatorState = !ledIndicatorState;
    digitalWrite(LED_INDICATOR, ledIndicatorState);
  }
}

void ledMasterFlash(int analog_value) {
  int ledInterval = slope * analog_value + intercept;
  unsigned long currentMillis = millis();
  if (currentMillis - ledMasterPreviousMillis >= (unsigned long)ledInterval) {
    ledMasterPreviousMillis = currentMillis;
    ledMasterState = !ledMasterState;
    digitalWrite(LED_MASTER, ledMasterState);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_INDICATOR, OUTPUT);
  pinMode(LED_MASTER, OUTPUT);

  // active LOW LEDs: HIGH = off
  digitalWrite(LED_INDICATOR, HIGH);
  digitalWrite(LED_MASTER, HIGH);

  for (int i = 0; i < 10; i++) readings[i] = -1;

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  IPAddress ip = WiFi.localIP();
  swarmID = ip[3] % 10;
  Serial.print("Swarm ID assigned: ");
  Serial.println(swarmID);

  udp.begin(localPort);
  Serial.printf("Listening on UDP port %d\n", localPort);

  getSlopeIntercept(Y_threshold, Y_interval, Z_threshold, Z_interval, &slope, &intercept);

  lastReceivedTime = millis();
}

void loop() {
  // Flash indicator LED based on current reading value (updated when silent)
  ledIndicatorFlash(analogValue);

  // Master LED only flashes if Master, otherwise off
  if (isMaster) {
    ledMasterFlash(analogValue);
  } else {
    digitalWrite(LED_MASTER, HIGH);
  }

  // Receive packets
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char incomingPacket[255];
    int len = udp.read(incomingPacket, 254);
    if (len > 0) incomingPacket[len] = '\0';

    String packetStr = String(incomingPacket);

    // ESP -> ESP
    if (packetStr.startsWith(ESP_startBit) && packetStr.endsWith(ESP_endBit)) {
      String data = packetStr.substring(ESP_startBit.length(),
                                        packetStr.length() - ESP_endBit.length());
      int receivedSwarmID, receivedReading;
      if (sscanf(data.c_str(), "%d,%d", &receivedSwarmID, &receivedReading) == 2) {
        if (receivedSwarmID >= 0 && receivedSwarmID < 10) {
          readings[receivedSwarmID] = receivedReading;
          lastReceivedTime = millis();
        }
      }
    }

    // RPi reset
    if (packetStr.startsWith(RPi_startBit) && packetStr.endsWith(RPi_endBit)) {
      String data = packetStr.substring(RPi_startBit.length(),
                                        packetStr.length() - RPi_endBit.length());
      if (data == "RESET_REQUESTED") {
        digitalWrite(LED_INDICATOR, HIGH);
        digitalWrite(LED_MASTER, HIGH);
        isMaster = true;
        Serial.println("RESET REQUESTED BY RPI");
        delay(3000);
      }
    }
  }

  // If silent for 200ms, read sensor and broadcast
  if (millis() - lastReceivedTime > silentTime) {
    analogValue = analogRead(PHOTORESISTOR_PIN);

    // ESP -> ESP broadcast
    String message = ESP_startBit + String(swarmID) + "," + String(analogValue) + ESP_endBit;
    udp.beginPacket(broadcastIP, localPort);
    udp.write(message.c_str());
    udp.endPacket();
    lastReceivedTime = millis();

    // Decide Master (highest reading)
    isMaster = true;
    for (int i = 0; i < 10; i++) {
      if (i != swarmID && readings[i] >= 0 && readings[i] > analogValue) {
        isMaster = false;
        break;
      }
    }

    // Master -> RPi broadcast
    if (isMaster) {
      role = "Master";
      String masterMessage = RPi_startBit + role + "," +
                             String(swarmID) + "," +
                             String(analogValue) + RPi_endBit;
      udp.beginPacket(broadcastIP, localPort);
      udp.write(masterMessage.c_str());
      udp.endPacket();
    } else {
      role = "Slave";
    }

    Serial.printf("Current role: %s (Reading: %d)\n", role.c_str(), analogValue);
  }
}
