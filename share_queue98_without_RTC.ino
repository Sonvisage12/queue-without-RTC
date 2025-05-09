#include <SPI.h>
#include <MFRC522.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <esp_now.h>
#include <WiFi.h>
#include <vector>
#include <algorithm>

#define RST_PIN  5
#define SS_PIN   4
#define GREEN_LED_PIN 15
#define RED_LED_PIN   2

MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences prefs;

#include "SharedQueue.h"
SharedQueue sharedQueue("rfid-patients");
//uint8_t arrivalMAC[] = {0x08, 0xD1, 0xF9, 0xD7, 0x50, 0x98};
uint8_t arrivalMAC[] = {0x30, 0xC6, 0xF7, 0x44, 0x1D, 0x24};
uint8_t doctorMAC1[] = {0x78, 0x42, 0x1C, 0x6C, 0xA8, 0x3C};
uint8_t doctorMAC[]  = {0x78, 0x42, 0x1C, 0x6C, 0xE4, 0x9C};

void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  QueueItem item;
  memcpy(&item, incomingData, sizeof(item));

  if (item.removeFromQueue) {
    sharedQueue.removeByUID(String(item.uid));
  } else {
    sharedQueue.addIfNew(String(item.uid), String(item.timestamp), item.number);
  }

  Serial.print("\xF0\x9F\x93\xA9 Received from: ");
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println(macStr);

  sharedQueue.print();
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("\xF0\x9F\x93\xA4 Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivered \xF0\x9F\x9F\xA2" : "Failed \xF0\x9F\x94\xB4");
}

void setup() {
  Serial.begin(115200);
  SPI.begin();
  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  Serial.print("WiFi MAC Address: ");
  Serial.println(WiFi.macAddress());

  mfrc522.PCD_Init();

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, HIGH);

  if (esp_now_init() != ESP_OK) {
    Serial.println("\u274C Error initializing ESP-NOW");
    return;
  }

  std::vector<uint8_t*> peers = {doctorMAC, arrivalMAC, doctorMAC1};

  for (auto peer : peers) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, peer, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    if (!esp_now_is_peer_exist(peer)) {
      esp_now_add_peer(&peerInfo);
    }
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  sharedQueue.load();
  Serial.println("\xF0\x9F\x93\x8D RFID Arrival Node Initialized. Waiting for patient card...");
  sharedQueue.print();
}

void loop() {
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  String uid = getUIDString(mfrc522.uid.uidByte, mfrc522.uid.size);
  Serial.print("\xF0\x9F\x86\x94 Card UID detected: ");
  Serial.println(uid);

  if (sharedQueue.exists(uid)) {
    Serial.println("\u23F3 Already in queue. Wait for your turn.");
    blinkLED(RED_LED_PIN);
  } else {
    DateTime now(2000, 1, 1, 0, 0, 0);  // Placeholder timestamp since no RTC
    int pid = sharedQueue.getOrAssignPermanentNumber(uid, now);
    String timeStr = String(millis());

    sharedQueue.add(uid, timeStr, pid);

    QueueItem item;
    strncpy(item.uid, uid.c_str(), sizeof(item.uid));
    strncpy(item.timestamp, timeStr.c_str(), sizeof(item.timestamp));
    item.number = pid;
    item.removeFromQueue = false;

    for (auto peer : std::vector<uint8_t*>{doctorMAC, arrivalMAC, doctorMAC1}) {
      esp_now_send(peer, (uint8_t *)&item, sizeof(item));
    }

    Serial.print("\u2705 Patient Registered. Assigned Number: ");
    Serial.print(pid);
    Serial.print(" | Time: ");
    Serial.println(timeStr);

    blinkLED(GREEN_LED_PIN);
    sharedQueue.print();
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(1500);
}

String getUIDString(byte *buffer, byte bufferSize) {
  String uidString = "";
  for (byte i = 0; i < bufferSize; i++) {
    if (buffer[i] < 0x10) uidString += "0";
    uidString += String(buffer[i], HEX);
  }
  uidString.toUpperCase();
  return uidString;
}

void blinkLED(int pin) {
  digitalWrite(pin, LOW);
  delay(1000);
  digitalWrite(pin, HIGH);
}
