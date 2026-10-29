/*
 * CyberRover — Camera module firmware (ESP32-CAM AI-Thinker)
 * سايبر روفر — برنامج وحدة الكاميرا: قراءة ملصقات QR وإرسالها للوحة الرئيسية
 *
 * Reads QR checkpoint codes (e.g. "QR-03") and sends "QR:QR-03\n" over UART
 * to the main ESP32 (cam TX GPIO1 -> main RX GPIO16, shared GND).
 *
 * Library: ESP32QRCodeReader (alvarowolfx) — install from the Arduino Library Manager.
 * Board: AI Thinker ESP32-CAM. Upload with the ESP32-CAM-MB programmer.
 * Note: Serial is shared with the USB programmer, so unplug the UART wire to the main
 * board while uploading.
 */

#include <ESP32QRCodeReader.h>

ESP32QRCodeReader reader(CAMERA_MODEL_AI_THINKER);
String lastCode = "";
unsigned long lastSent = 0;
const unsigned long RESEND_MS = 8000;   // send the same code again only after 8 s

void setup() {
  Serial.begin(115200);
  reader.setup();
  reader.beginOnCore(1);
}

void loop() {
  struct QRCodeData qr;
  if (reader.receiveQrCode(&qr, 100) && qr.valid) {
    String code = String((const char *)qr.payload);
    code.trim();
    // Only accept CyberRover checkpoint codes: "QR-xx"
    if (code.startsWith("QR-") && (code != lastCode || millis() - lastSent > RESEND_MS)) {
      Serial.print("QR:");
      Serial.println(code);
      lastCode = code;
      lastSent = millis();
    }
  }
  delay(50);
}
