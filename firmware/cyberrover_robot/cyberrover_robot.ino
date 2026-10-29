/*
 * CyberRover — Main controller firmware (ESP32-WROOM-32U)
 * سايبر روفر — برنامج لوحة التحكم الرئيسية
 *
 * What it does:
 *   1. Patrols corridors and avoids obstacles (HC-SR04 on an SG90 servo, TB6612FNG motor driver).
 *   2. Stops when the ESP32-CAM reports a QR checkpoint over UART ("QR:QR-03").
 *   3. Stop-and-scan: runs several passive Wi-Fi scans, averages RSSI per BSSID.
 *   4. Checks every network against the whitelist (SSID + BSSID + channel + security).
 *   5. Prints one JSON line per checkpoint on USB Serial (read by ai/serial_logger.py)
 *      and shows the status on the OLED.
 *
 * Passive scanning only: the robot listens to beacons, it never connects to or attacks any network.
 * يعمل بالمسح السلبي فقط داخل نطاق المنشأة المصرّح بها.
 *
 * Board: ESP32 Dev Module (Arduino core 2.0.x or 3.x)
 * Libraries: ESP32Servo, Adafruit SSD1306, Adafruit GFX
 */

#include <WiFi.h>
#include <Wire.h>
#include <ESP32Servo.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "whitelist.h"

// ---------------- Pins ----------------
// TB6612FNG motor driver (A = left side, B = right side)
const int PIN_PWMA = 25, PIN_AIN1 = 26, PIN_AIN2 = 27;
const int PIN_PWMB = 14, PIN_BIN1 = 13, PIN_BIN2 = 33;
const int PIN_STBY = 32;
// HC-SR04: Echo goes through a 1k/2k voltage divider (5V -> 3.3V)
const int PIN_TRIG = 5, PIN_ECHO = 34;
// SG90 servo that turns the distance sensor
const int PIN_SERVO = 18;
// UART link to the ESP32-CAM (QR reader)
const int PIN_CAM_RX = 16, PIN_CAM_TX = 17;
// OLED SSD1306 on I2C: SDA 21, SCL 22

// ---------------- Tuning ----------------
const int   DRIVE_SPEED      = 170;   // 0..255
const int   TURN_SPEED       = 160;
const int   OBSTACLE_CM      = 25;    // stop and look around below this distance
const int   SCAN_ROUNDS      = 5;     // scans per checkpoint (stop-and-scan)
const int   DEAD_ZONE_DBM    = -72;   // below this the checkpoint is a dead zone
const int   WEAK_SIGNAL_DBM  = -60;   // between -60 and -72 the signal is weak
const int   MAX_NETWORKS     = 40;

Servo scanServo;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
HardwareSerial CamSerial(2);

enum Mode { PATROL, SCANNING, PAUSED };
Mode mode = PATROL;
String currentCheckpoint = "-";

// ---------------- Motors ----------------
void motor(int in1, int in2, int pwm, int speed) {
  digitalWrite(in1, speed > 0);
  digitalWrite(in2, speed < 0);
  analogWrite(pwm, abs(speed));
}
void drive(int left, int right) {
  motor(PIN_AIN1, PIN_AIN2, PIN_PWMA, left);
  motor(PIN_BIN1, PIN_BIN2, PIN_PWMB, right);
}
void stopMotors() { drive(0, 0); }

// ---------------- Distance sensor ----------------
int readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long us = pulseIn(PIN_ECHO, HIGH, 25000);   // ~4 m timeout
  if (us == 0) return 400;
  return us / 58;
}
int lookAt(int angle) {
  scanServo.write(angle);
  delay(350);
  return readDistanceCm();
}

// ---------------- OLED ----------------
void show(const String &l1, const String &l2 = "", const String &l3 = "") {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);  oled.println("CyberRover");
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  oled.setCursor(0, 16); oled.println(l1);
  oled.setCursor(0, 30); oled.println(l2);
  oled.setCursor(0, 44); oled.println(l3);
  oled.display();
}

// ---------------- Wi-Fi scan ----------------
struct Seen {
  String ssid, bssid;
  int channel;
  wifi_auth_mode_t auth;
  long rssiSum;
  int hits;
};
Seen seen[MAX_NETWORKS];
int seenCount = 0;

const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "OTHER";
  }
}
// Rank so that a weaker security mode than the whitelist expects is flagged
int authRank(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return 0;
    case WIFI_AUTH_WEP:  return 1;
    case WIFI_AUTH_WPA_PSK: case WIFI_AUTH_WPA_WPA2_PSK: return 2;
    case WIFI_AUTH_WPA2_PSK: case WIFI_AUTH_WPA2_ENTERPRISE: return 3;
    case WIFI_AUTH_WPA2_WPA3_PSK: case WIFI_AUTH_WPA3_PSK: return 4;
    default: return 2;
  }
}

void addSample(const String &ssid, const String &bssid, int ch, wifi_auth_mode_t auth, int rssi) {
  for (int i = 0; i < seenCount; i++) {
    if (seen[i].bssid == bssid) { seen[i].rssiSum += rssi; seen[i].hits++; return; }
  }
  if (seenCount < MAX_NETWORKS) seen[seenCount++] = { ssid, bssid, ch, auth, (long)rssi, 1 };
}

// Stop-and-scan: several passive scans, RSSI averaged per BSSID
void stopAndScan() {
  seenCount = 0;
  for (int round = 0; round < SCAN_ROUNDS; round++) {
    show("Checkpoint " + currentCheckpoint, "Scanning " + String(round + 1) + "/" + String(SCAN_ROUNDS));
    int n = WiFi.scanNetworks(false, true, true);   // sync, include hidden, passive
    for (int i = 0; i < n; i++) {
      addSample(WiFi.SSID(i), WiFi.BSSIDstr(i), WiFi.channel(i), WiFi.encryptionType(i), WiFi.RSSI(i));
    }
    WiFi.scanDelete();
  }
}

// ---------------- Policy check + JSON report ----------------
void analyzeAndReport() {
  int bestProtected = -120;           // strongest whitelisted AP at this point
  int threats = 0;
  String netsJson = "", alertsJson = "";

  for (int i = 0; i < seenCount; i++) {
    Seen &s = seen[i];
    int avg = s.rssiSum / s.hits;
    String status = "UNKNOWN";

    const WhitelistEntry *w = findByBssid(s.bssid.c_str());
    if (w) {
      if (w->channel != 0 && w->channel != s.channel)        status = "CHANNEL_MISMATCH";
      else if (authRank(s.auth) < authRank(w->minAuth))      status = "SECURITY_DOWNGRADE";
      else                                                   status = "OK";
      if (status == "OK" && avg > bestProtected) bestProtected = avg;
    } else if (isProtectedSsid(s.ssid.c_str())) {
      status = "EVIL_TWIN";            // our network name, unknown radio
    } else if (s.auth == WIFI_AUTH_OPEN || s.auth == WIFI_AUTH_WEP) {
      status = "WEAK_ENCRYPTION";
    }

    if (status != "OK" && status != "UNKNOWN") {
      threats++;
      if (alertsJson.length()) alertsJson += ",";
      alertsJson += "{\"type\":\"" + status + "\",\"ssid\":\"" + s.ssid + "\",\"bssid\":\"" + s.bssid + "\"}";
    }
    if (netsJson.length()) netsJson += ",";
    netsJson += "{\"ssid\":\"" + s.ssid + "\",\"bssid\":\"" + s.bssid + "\",\"ch\":" + String(s.channel) +
                ",\"rssi\":" + String(avg) + ",\"auth\":\"" + authName(s.auth) + "\",\"status\":\"" + status + "\"}";
  }

  String coverage = bestProtected < DEAD_ZONE_DBM ? "DEAD_ZONE" : bestProtected < WEAK_SIGNAL_DBM ? "WEAK" : "GOOD";
  if (coverage == "DEAD_ZONE") {
    if (alertsJson.length()) alertsJson += ",";
    alertsJson += "{\"type\":\"DEAD_ZONE\",\"rssi\":" + String(bestProtected) + "}";
  }

  // One JSON line per checkpoint, read by ai/serial_logger.py
  Serial.print("{\"checkpoint\":\"" + currentCheckpoint + "\",\"uptime_s\":" + String(millis() / 1000) +
               ",\"best_rssi\":" + String(bestProtected) + ",\"coverage\":\"" + coverage +
               "\",\"network_count\":" + String(seenCount) + ",\"networks\":[" + netsJson + "],\"alerts\":[" + alertsJson + "]}");
  Serial.println();

  show("Checkpoint " + currentCheckpoint, "RSSI " + String(bestProtected) + " dBm " + coverage,
       threats ? "ALERTS: " + String(threats) : "Policy OK");
}

// ---------------- Patrol ----------------
void patrolStep() {
  int ahead = readDistanceCm();
  if (ahead > OBSTACLE_CM) { drive(DRIVE_SPEED, DRIVE_SPEED); return; }

  stopMotors();
  int right = lookAt(20), left = lookAt(160);
  scanServo.write(90);
  if (max(left, right) < OBSTACLE_CM) {           // dead end: back up and turn around
    drive(-DRIVE_SPEED, -DRIVE_SPEED); delay(400);
    drive(TURN_SPEED, -TURN_SPEED);    delay(700);
  } else if (left > right) {
    drive(-TURN_SPEED, TURN_SPEED);    delay(350);
  } else {
    drive(TURN_SPEED, -TURN_SPEED);    delay(350);
  }
  stopMotors();
}

// Messages from the ESP32-CAM: "QR:<id>"
void readCamera() {
  if (!CamSerial.available()) return;
  String line = CamSerial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("QR:")) {
    String id = line.substring(3);
    if (id != currentCheckpoint) {      // ignore the same code read twice
      currentCheckpoint = id;
      mode = SCANNING;
    }
  }
}

// Commands from the laptop: "p" pause, "r" resume, "s" scan here
void readCommands() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 'p') { mode = PAUSED; stopMotors(); show("Paused"); }
  if (c == 'r') { mode = PATROL; }
  if (c == 's') { currentCheckpoint = "MANUAL"; mode = SCANNING; }
}

void setup() {
  Serial.begin(115200);
  CamSerial.begin(115200, SERIAL_8N1, PIN_CAM_RX, PIN_CAM_TX);

  int outs[] = { PIN_PWMA, PIN_AIN1, PIN_AIN2, PIN_PWMB, PIN_BIN1, PIN_BIN2, PIN_STBY, PIN_TRIG };
  for (int p : outs) pinMode(p, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_STBY, HIGH);

  scanServo.attach(PIN_SERVO);
  scanServo.write(90);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("{\"error\":\"OLED not found\"}");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();     // passive: never joins a network
  show("Ready", "Passive scan mode");
  delay(1000);
}

void loop() {
  readCommands();
  readCamera();

  switch (mode) {
    case PATROL:
      patrolStep();
      break;
    case SCANNING:
      stopMotors();
      stopAndScan();
      analyzeAndReport();
      delay(800);
      mode = PATROL;
      break;
    case PAUSED:
      break;
  }
  delay(30);
}
