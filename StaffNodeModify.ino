#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <map>
#include <queue>
#include <SPI.h>
#include <MFRC522.h>
#include <Preferences.h>

// Pins
#define GREEN_LED_PIN 15
#define RED_LED_PIN   2
#define RST_PIN       5
#define SS_PIN        4

MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences preferences;

int patientNum = 0;

// Queue item structure
struct QueueItem {
  char uid[20];
  char timestamp[25];
  int number;
  bool removeFromQueue;
};

// Sync request structure
struct SyncRequest {
  char type[10]; // "SYNC_REQ"
};

// Queue structures
std::map<String, QueueItem> queueMap;
std::queue<String> patientOrder;

// MAC addresses
//uint8_t displayMAC[] = {0xA4, 0xCF, 0x12, 0xF1, 0x6B, 0xA5};
uint8_t displayMAC[] = {0x68, 0xC6, 0x3A, 0xFC, 0x61, 0x3E};
uint8_t peer1[] = {0x08, 0xD1, 0xF9, 0xD7, 0x50, 0x98};
uint8_t peer2[] = {0x30, 0xC6, 0xF7, 0x44, 0x1D, 0x24};
uint8_t peer3[] = {0x78, 0x42, 0x1C, 0x6C, 0xA8, 0x3C};
//uint8_t peer3[] = {0x78, 0x42, 0x1C, 0x6C, 0xE4, 0x9C};

std::vector<uint8_t*> peerMACs = { peer1, peer2, peer3, displayMAC };

// Forward declarations
void displayNextPatient();
String getUIDString(byte *buffer, byte bufferSize);
void blinkLED(int pin);

// ======= Queue Persistence =======
void persistQueue() {
  preferences.begin("queue", false);
  preferences.clear();
  int i = 0;
  for (auto& entry : queueMap) {
    String key = "q_" + String(i);
    preferences.putBytes(key.c_str(), &entry.second, sizeof(QueueItem));
    i++;
  }
  preferences.putUInt("queueSize", i);
  preferences.end();
}

void loadQueueFromFlash() {
  preferences.begin("queue", true);
  int size = preferences.getUInt("queueSize", 0);
  for (int i = 0; i < size; i++) {
    String key = "q_" + String(i);
    QueueItem item;
    if (preferences.getBytes(key.c_str(), &item, sizeof(QueueItem))) {
      String uid = String(item.uid);
      queueMap[uid] = item;
      patientOrder.push(uid);
    }
  }
  preferences.end();
  Serial.println("🔁 Restored queue from flash.");
}

// ======= ESP-NOW Messaging =======
void broadcastQueueItem(const QueueItem& item) {
  for (auto mac : peerMACs) {
    esp_now_send(mac, (uint8_t*)&item, sizeof(item));
    delay(10);
  }
}

void broadcastFullQueue() {
  for (auto& entry : queueMap) {
    broadcastQueueItem(entry.second);
  }
  Serial.println("📤 Broadcasted full queue.");
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (len == sizeof(SyncRequest)) {
    SyncRequest req;
    memcpy(&req, incomingData, sizeof(req));
    if (strcmp(req.type, "SYNC_REQ") == 0) {
      Serial.println("🔄 Received sync request.");
      broadcastFullQueue();
      return;
    }
  }

  if (len == sizeof(QueueItem)) {
    QueueItem item;
    memcpy(&item, incomingData, sizeof(item));
    String uid = String(item.uid);

    if (item.removeFromQueue) {
      if (queueMap.count(uid)) {
        queueMap.erase(uid);

        std::queue<String> temp;
        while (!patientOrder.empty()) {
          if (patientOrder.front() != uid)
            temp.push(patientOrder.front());
          patientOrder.pop();
        }
        patientOrder = temp;

        Serial.print("🗑️ Removed: ");
        Serial.println(uid);
        persistQueue();
        displayNextPatient();
      }
    } else {
      if (!queueMap.count(uid)) {
        queueMap[uid] = item;
        patientOrder.push(uid);
        Serial.print("📥 Added Patient ");
        Serial.print(item.number);
        Serial.print(" | UID: ");
        Serial.println(uid);
        persistQueue();
        displayNextPatient();
      }
    }
  }
}

// ======= Setup & Loop =======
void setup() {
  Serial.begin(115200);
  SPI.begin();
  mfrc522.PCD_Init();

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, HIGH);

  WiFi.mode(WIFI_STA);
  Serial.print("WiFi MAC Address: ");
  Serial.println(WiFi.macAddress());

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW Init Failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  for (auto mac : peerMACs) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    if (!esp_now_is_peer_exist(mac)) {
      esp_now_add_peer(&peerInfo);
    }
  }


  loadQueueFromFlash();
  Serial.println("👨‍⚕️ Staff Node Ready");
  displayNextPatient();
}

void loop() {
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

  String uid = getUIDString(mfrc522.uid.uidByte, mfrc522.uid.size);
  uid.toUpperCase();

  if (!patientOrder.empty() && uid == patientOrder.front()) {
    QueueItem item = queueMap[uid];
    item.removeFromQueue = true;
    broadcastQueueItem(item);

    queueMap.erase(uid);
    patientOrder.pop();

    persistQueue();
    Serial.print("✅ Attended: ");
    Serial.println(item.number);
    blinkLED(GREEN_LED_PIN);
    displayNextPatient();
  } else {
    Serial.println("❌ Access Denied");
    blinkLED(RED_LED_PIN);
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(1500);
}

// ======= Helpers =======
void displayNextPatient() {
  while (!patientOrder.empty()) {
    String uid = patientOrder.front();
    if (queueMap.count(uid)) {
      QueueItem item = queueMap[uid];
      broadcastQueueItem(item);
      patientNum = item.number;
      esp_now_send(displayMAC, (uint8_t*)&patientNum, sizeof(patientNum));
      Serial.print("🔔 Next Patient: ");
      Serial.println(patientNum);
      return;
    } else {
      patientOrder.pop();
    }
  }
  patientNum = 0;
  esp_now_send(displayMAC, (uint8_t*)&patientNum, sizeof(patientNum));
  Serial.println("📭 Queue is empty");
}

String getUIDString(byte *buffer, byte bufferSize) {
  String uid = "";
  for (byte i = 0; i < bufferSize; i++) {
    if (buffer[i] < 0x10) uid += "0";
    uid += String(buffer[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void blinkLED(int pin) {
  digitalWrite(pin, LOW);
  delay(1000);
  digitalWrite(pin, HIGH);
}
