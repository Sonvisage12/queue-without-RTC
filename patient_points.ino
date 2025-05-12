#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <map>
#include <queue>
#include <SPI.h>
#include <MFRC522.h>
#include <Preferences.h>

#define GREEN_LED_PIN 15
#define RED_LED_PIN   2
#define RST_PIN       5
#define SS_PIN        4

MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences preferences;
int patientNum;

// Queue item now has a reservation flag and the MAC of the reserving node
struct QueueItem {
  char uid[20];
  char timestamp[25];
  int number;
  bool removeFromQueue;
  bool reserved;
  uint8_t reservedByMAC[6];
};

struct SyncRequest {
  char type[10]; // should be "SYNC_REQ"
};

std::map<String, QueueItem> queueMap;
std::queue<String> patientOrder;

uint8_t patientMAC[] = {0x08, 0xD1, 0xF9, 0xD7, 0x50, 0x98};
uint8_t patientMAC1[] = {0x30, 0xC6, 0xF7, 0x44, 0x1D, 0x24};
uint8_t displayMAC[] = {0xA4, 0xCF, 0x12, 0xF1, 0x6B, 0xA5};
uint8_t StaffMAC2[] = {0x78, 0x42, 0x1C, 0x6C, 0xA8, 0x3C"};

std::vector<uint8_t*> peerMACs = {
  patientMAC,
  patientMAC1,
  StaffMAC2,
  displayMAC
};

// Track local reservation state
bool hasReserved = false;
String myReservedUID = "";

// Persist the entire queue (including reservations) to flash
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

// Load queue (previously saved items) from flash
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

// Broadcast a single queue item to all peers via ESP-NOW
void broadcastQueueItem(const QueueItem& item) {
  for (auto mac : peerMACs) {
    esp_now_send(mac, (uint8_t*)&item, sizeof(item));
    delay(10);
  }
}

// Broadcast the full queue (all items) to all peers
void broadcastFullQueue() {
  for (auto& entry : queueMap) {
    broadcastQueueItem(entry.second);
  }
  Serial.println("📤 Broadcasted full queue to all peers.");
}

// Try to reserve the next unreserved patient (in FIFO order)
void tryReserveNext() {
  if (hasReserved) return;
  std::queue<String> tempQueue = patientOrder;
  while (!tempQueue.empty()) {
    String uid = tempQueue.front();
    tempQueue.pop();
    if (!queueMap.count(uid)) continue;
    QueueItem &item = queueMap[uid];
    if (!item.reserved) {
      // Reserve this patient
      item.reserved = true;
      esp_read_mac(item.reservedByMAC, ESP_MAC_WIFI_STA);
      broadcastQueueItem(item);  // inform peers of reservation
      myReservedUID = uid;
      hasReserved = true;
      Serial.print("🔒 Patient reserved: ");
      Serial.print(item.number);
      Serial.print(" | UID: ");
      Serial.println(uid);
      return;
    }
  }
}

// Handle incoming ESP-NOW data (sync requests, new patient, reservation, or removal)
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  // Handle sync request
  if (len == sizeof(SyncRequest)) {
    SyncRequest req;
    memcpy(&req, incomingData, sizeof(req));
    if (strcmp(req.type, "SYNC_REQ") == 0) {
      Serial.println("🔄 Received sync request.");
      broadcastFullQueue();
      return;
    }
  }

  // Handle queue item updates (new, reserved, or removal)
  if (len == sizeof(QueueItem)) {
    QueueItem item;
    memcpy(&item, incomingData, sizeof(item));
    String uid = String(item.uid);

    if (item.removeFromQueue) {
      // Patient removed from queue
      if (queueMap.count(uid)) {
        queueMap.erase(uid);
        // Remove from patientOrder
        std::queue<String> tempQueue;
        while (!patientOrder.empty()) {
          if (patientOrder.front() != uid)
            tempQueue.push(patientOrder.front());
          patientOrder.pop();
        }
        patientOrder = tempQueue;

        Serial.print("🗑️ Removed patient via peer sync: ");
        Serial.println(uid);
        persistQueue();
        tryReserveNext();
        displayNextPatient();
      }
    }
    else if (item.reserved) {
      // Reservation sync from another node
      if (queueMap.count(uid)) {
        queueMap[uid].reserved = true;
        memcpy(queueMap[uid].reservedByMAC, item.reservedByMAC, 6);
        Serial.print("🔒 Patient reserved by peer: ");
        Serial.println(uid);
      }
      // No queue modification needed beyond marking reserved
      return;
    }
    else {
      // New patient added
      if (!queueMap.count(uid)) {
        queueMap[uid] = item;
        patientOrder.push(uid);
        Serial.print("\n📥 New Patient Added: ");
        Serial.print(item.number);
        Serial.print(" | UID: ");
        Serial.print(uid);
        Serial.print(" | Time: ");
        Serial.println(item.timestamp);
        displayNextPatient();
        persistQueue();
        tryReserveNext();
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  SPI.begin();

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  mfrc522.PCD_Init();

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, HIGH);

  WiFi.mode(WIFI_STA);
  Serial.print("WiFi MAC Address: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error initializing ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);

  // Add all known peers for ESP-NOW
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
  Serial.println("👨‍⚕️ Doctor Node Initialized");

  // Sync queue state from peers at startup
  SyncRequest req;
  strcpy(req.type, "SYNC_REQ");
  for (auto mac : peerMACs) {
    esp_now_send(mac, (uint8_t*)&req, sizeof(req));
    delay(10);
  }
  Serial.println("🔄 Sync request sent to peers.");

  displayNextPatient();
}

void loop() {
  if (queueMap.empty() || patientOrder.empty()) return;

  // If not currently holding a reservation, try to reserve next patient
  if (!hasReserved) {
    tryReserveNext();
  }
  if (!hasReserved) return;

  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) 
    return;

  String uid = getUIDString(mfrc522.uid.uidByte, mfrc522.uid.size);
  uid.toUpperCase();

  // Check if scanned card matches our reserved patient
  if (uid == myReservedUID) {
    QueueItem item = queueMap[myReservedUID];
    Serial.print("✅ Patient No ");
    Serial.print(item.number);
    Serial.println(" attended. Removing from queue.");

    // Broadcast removal
    item.removeFromQueue = true;
    broadcastQueueItem(item);
    patientNum = item.number;
    esp_now_send(displayMAC, (uint8_t*)&patientNum, sizeof(patientNum));

    // Remove from our queue and persist
    queueMap.erase(myReservedUID);
    std::queue<String> tempQueue;
    while (!patientOrder.empty()) {
      if (patientOrder.front() != myReservedUID)
        tempQueue.push(patientOrder.front());
      patientOrder.pop();
    }
    patientOrder = tempQueue;
    persistQueue();

    // Clear reservation and move to next
    hasReserved = false;
    myReservedUID = "";
    tryReserveNext();

    blinkLED(GREEN_LED_PIN);
    displayNextPatient();
  } else {
    Serial.println("❌ Not the reserved patient. Access Denied.");
    blinkLED(RED_LED_PIN);
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(1500);
}

// Display the first unreserved patient to the external display
void displayNextPatient() {
  // Iterate in FIFO order until an unreserved item is found
  std::queue<String> tempQueue = patientOrder;
  while (!tempQueue.empty()) {
    String uid = tempQueue.front();
    tempQueue.pop();
    if (!queueMap.count(uid)) continue;
    QueueItem item = queueMap[uid];
    if (item.reserved) continue;  // skip those already reserved
    broadcastQueueItem(item);
    patientNum = item.number;
    esp_now_send(displayMAC, (uint8_t*)&patientNum, sizeof(patientNum));
    Serial.print("🔔 Next Patient Number: ");
    Serial.println(patientNum);
    return;
  }
  Serial.println("📭 Queue is now empty.");
  patientNum = 0;
  // Optionally: send patientNum=0 to displayMAC to clear display
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
