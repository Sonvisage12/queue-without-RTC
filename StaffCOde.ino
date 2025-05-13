#include <SPI.h>
#include <MFRC522.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <esp_now.h>
#include <WiFi.h>
#include <vector>
#include <algorithm>
#include "SharedQueue.h"

// Pin configuration for MFRC522 and LEDs
#define RST_PIN         5    // Reset pin for RFID reader
#define SS_PIN          4    // Slave Select pin for RFID reader (SPI)
#define GREEN_LED_PIN   15
#define RED_LED_PIN     2

// Initialize RFID reader and preferences
MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences prefs;

// Initialize shared queue (persistent) for patient data
SharedQueue sharedQueue("rfid-patients");

// Known MAC addresses for all nodes (update these values for your devices)
uint8_t arrivalMAC[6]  = {0x30, 0xC6, 0xF7, 0x44, 0x1D, 0x24};  // Arrival (registration) node
uint8_t doctor1MAC[6]  = {0x78, 0x42, 0x1C, 0x6C, 0xA8, 0x3C};  // Doctor node 1
//uint8_t doctor2MAC[6]  = {0x78, 0x42, 0x1C, 0x6C, 0xE4, 0x9C};  // Doctor node 2
uint8_t display1MAC[6] = {0x08, 0xD1, 0xF9, 0xD7, 0x50, 0x98};  // Display node for doctor 1
uint8_t display2MAC[6] = {0x, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // Display node for doctor 2 (PLACEHOLDER: set actual MAC)

// Role definitions for this device
enum Role { ROLE_ARRIVAL = 0, ROLE_DOCTOR1, ROLE_DOCTOR2, ROLE_DISPLAY1, ROLE_DISPLAY2, ROLE_UNKNOWN };
Role deviceRole = ROLE_UNKNOWN;

// (Optional) Define authorized staff card UIDs for doctor nodes (replace with actual UIDs of staff cards)
String doctor1CardUID = "";  // e.g., "AB12CD34" (if left empty, any unknown card on doctor node is treated as unauthorized)
String doctor2CardUID = "";

// Callback when data is received via ESP-NOW
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  QueueItem item;
  memcpy(&item, incomingData, sizeof(item));

  // Print sender MAC for debugging
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("📩 Received data from: ");
  Serial.println(macStr);

  if (deviceRole == ROLE_DISPLAY1 || deviceRole == ROLE_DISPLAY2) {
    // **Display node**: show the incoming patient's number (next to be served)
    Serial.print("🔔 Now Serving Patient Number: ");
    Serial.println(item.number);
    // (In a real display node, update the physical display here. This Serial print simulates the display output.)
  } else {
    // **Arrival or Doctor node**: update local queue state using SharedQueue
    if (item.removeFromQueue) {
      // Another node removed a patient – remove that UID from local queue
      sharedQueue.removeByUID(String(item.uid));
    } else {
      // New patient added on another node – add to local queue if not already present
      sharedQueue.addIfNew(String(item.uid), String(item.timestamp), item.number);
    }
    sharedQueue.print();
  }
}

// Callback when data is sent via ESP-NOW (for debug status)
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("📤 Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivered ✅" : "Failed ❌");
}

// Utility: Convert RFID UID bytes to a String
String getUIDString(byte *buffer, byte bufferSize) {
  String uidString = "";
  for (byte i = 0; i < bufferSize; i++) {
    if (buffer[i] < 0x10) uidString += "0";         // zero-pad single hex digit
    uidString += String(buffer[i], HEX);
  }
  uidString.toUpperCase();
  return uidString;
}

// Utility: Retrieve permanent patient number from storage (returns -1 if not found/authorized)
int getPermanentNumber(const String &uid) {
  prefs.begin("rfidMap", true);  // open preferences in read-only mode
  int pid = -1;
  if (prefs.isKey(uid.c_str())) {
    pid = prefs.getUInt(uid.c_str(), 0);
    Serial.printf("✅ Known UID: %s -> Assigned Number: %d\n", uid.c_str(), pid);
  }
  prefs.end();
  return pid;
}

// Utility: Blink an LED for feedback (500ms)
void blinkLED(int pin) {
  digitalWrite(pin, LOW);
  delay(500);
  digitalWrite(pin, HIGH);
}

void setup() {
  Serial.begin(115200);

  // Determine this device's role by comparing its Wi-Fi MAC to known addresses
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // not connecting to AP, just use station mode for ESPNOW
  // Ensure Wi-Fi is on channel 1 for ESP-NOW communication on all devices
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  // Get our MAC address as a string for comparison
  String selfMac = WiFi.macAddress();
  selfMac.toUpperCase();
  if      (selfMac == "30:C6:F7:44:1D:24") deviceRole = ROLE_ARRIVAL;
  else if (selfMac == "78:42:1C:6C:A8:3C") deviceRole = ROLE_DOCTOR1;
  else if (selfMac == "78:42:1C:6C:E4:9C") deviceRole = ROLE_DOCTOR2;
  else if (selfMac == "08:D1:F9:D7:50:98") deviceRole = ROLE_DISPLAY1;
  else if (selfMac == "FF:FF:FF:FF:FF:FF") deviceRole = ROLE_DISPLAY2;  // placeholder match
  else                                     deviceRole = ROLE_UNKNOWN;

  // Announce role and MAC
  Serial.print("WiFi MAC Address: ");
  Serial.println(selfMac);
  switch (deviceRole) {
    case ROLE_ARRIVAL:  Serial.println("📖 Initializing Arrival (Registration) Node..."); break;
    case ROLE_DOCTOR1:  Serial.println("🏥 Initializing Doctor Node 1..."); break;
    case ROLE_DOCTOR2:  Serial.println("🏥 Initializing Doctor Node 2..."); break;
    case ROLE_DISPLAY1: Serial.println("📺 Initializing Display Node 1 (Doctor 1)..."); break;
    case ROLE_DISPLAY2: Serial.println("📺 Initializing Display Node 2 (Doctor 2)..."); break;
    default:            Serial.println("⚠️ Unknown role! Check MAC configuration."); break;
  }

  // Initialize RFID reader and LEDs for nodes that use them (arrival and doctor nodes)
  if (deviceRole == ROLE_ARRIVAL || deviceRole == ROLE_DOCTOR1 || deviceRole == ROLE_DOCTOR2) {
    SPI.begin();
    mfrc522.PCD_Init();  // Initialize MFRC522 module
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(RED_LED_PIN, OUTPUT);
    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(RED_LED_PIN, HIGH);
  }

  // Load or initialize known patient UID->number mappings in NVS preferences
  // (In a real system, this would be pre-provisioned. Here we write a sample list for demonstration.)
  if (deviceRole == ROLE_ARRIVAL || deviceRole == ROLE_DOCTOR1 || deviceRole == ROLE_DOCTOR2) {
    prefs.begin("rfidMap", false);
    // Example: Pre-load permanent numbers for known patient card UIDs
    prefs.putUInt("13B6B1E3", 1);
    prefs.putUInt("13D7ADE3", 2);
    prefs.putUInt("A339D9E3", 3);
    // ... (additional UID->Number entries up to required range) ...
    prefs.end();
  }

  // Initialize ESP-NOW and set up peers for communication
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  // List of all peers (in fixed order corresponding to Role enum)
  uint8_t *peerAddrs[] = { arrivalMAC, doctor1MAC, doctor2MAC, display1MAC, display2MAC };
  int selfIndex = (int)deviceRole;
  for (int i = 0; i < 5; ++i) {
    if (i == selfIndex) continue;                      // skip itself
    if (peerAddrs[i][0] == 0xFF) continue;             // skip placeholder MACs (not configured)
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, peerAddrs[i], 6);
    peerInfo.channel = 1;                              // using Wi-Fi channel 1
    peerInfo.encrypt = false;
    if (!esp_now_is_peer_exist(peerInfo.peer_addr)) {
      esp_now_add_peer(&peerInfo);
    }
  }

  // Load any persisted queue state and display it
  sharedQueue.load();
  Serial.println();
  Serial.println("=== System Initialized ===");
  sharedQueue.print();
}

void loop() {
  // **RFID Card Scanning Logic** (only active on arrival and doctor nodes)
  if (deviceRole == ROLE_ARRIVAL || deviceRole == ROLE_DOCTOR1 || deviceRole == ROLE_DOCTOR2) {
    // Check for a new RFID card present
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
      return;  // No card detected
    }
    // Convert card UID to string
    String uid = getUIDString(mfrc522.uid.uidByte, mfrc522.uid.size);
    Serial.print("🆔 Card UID detected: ");
    Serial.println(uid);

    if (deviceRole == ROLE_ARRIVAL) {
      // **Arrival Node**: Patients tapping their cards to join the queue
      if (sharedQueue.exists(uid)) {
        // Card already in queue (duplicate check-in)
        Serial.println("⏳ Patient is already in queue. Please wait your turn.");
        blinkLED(RED_LED_PIN);
      } else {
        int pid = getPermanentNumber(uid);
        if (pid == -1) {
          // Unknown card - not registered as a patient
          Serial.println("❌ Unrecognized card! Access denied.");
          blinkLED(RED_LED_PIN);
        } else {
          // Add new patient to the queue
          String timeStr = String(millis());  // use current uptime as timestamp (for order)
          sharedQueue.add(uid, timeStr, pid);
          // Prepare QueueItem for broadcast (add event)
          QueueItem newItem;
          memset(&newItem, 0, sizeof(newItem));
          strncpy(newItem.uid, uid.c_str(), sizeof(newItem.uid) - 1);
          strncpy(newItem.timestamp, timeStr.c_str(), sizeof(newItem.timestamp) - 1);
          newItem.number = pid;
          newItem.removeFromQueue = false;
          // Broadcast to doctor nodes (to update their queues)
          esp_now_send(doctor1MAC, (uint8_t*)&newItem, sizeof(newItem));
          esp_now_send(doctor2MAC, (uint8_t*)&newItem, sizeof(newItem));
          Serial.printf("✅ Patient registered. Assigned Number: %d | Time: %s\n", pid, timeStr.c_str());
          blinkLED(GREEN_LED_PIN);
          sharedQueue.print();
        }
      }

    } else {
      // **Doctor Node** (ROLE_DOCTOR1 or ROLE_DOCTOR2)
      bool isStaffCard = false;
      // Check if scanned UID matches this doctor's authorized card (if configured)
      if ((deviceRole == ROLE_DOCTOR1 && doctor1CardUID.length() && uid == doctor1CardUID) ||
          (deviceRole == ROLE_DOCTOR2 && doctor2CardUID.length() && uid == doctor2CardUID)) {
        isStaffCard = true;
      }
      if (isStaffCard) {
        // *** Staff (doctor) card scanned: call next patient ***
        if (sharedQueue.getQueue().empty()) {
          Serial.println("📭 No patients in queue to call.");
          blinkLED(RED_LED_PIN);
        } else {
          // Remove the first patient in the queue (the next patient to be served)
          QueueEntry nextEntry = sharedQueue.getQueue().front();
          sharedQueue.removeByUID(nextEntry.uid);
          // Prepare QueueItem for removal broadcast
          QueueItem remItem;
          memset(&remItem, 0, sizeof(remItem));
          strncpy(remItem.uid, nextEntry.uid.c_str(), sizeof(remItem.uid) - 1);
          strncpy(remItem.timestamp, nextEntry.timestamp.c_str(), sizeof(remItem.timestamp) - 1);
          remItem.number = nextEntry.number;
          remItem.removeFromQueue = true;
          // Broadcast removal to other syncing nodes (arrival and the other doctor)
          esp_now_send(arrivalMAC, (uint8_t*)&remItem, sizeof(remItem));
          if (deviceRole == ROLE_DOCTOR1) {
            esp_now_send(doctor2MAC, (uint8_t*)&remItem, sizeof(remItem));
          } else if (deviceRole == ROLE_DOCTOR2) {
            esp_now_send(doctor1MAC, (uint8_t*)&remItem, sizeof(remItem));
          }
          // Send the called patient's info to this doctor's display node
          QueueItem displayItem = remItem;
          displayItem.removeFromQueue = false;  // mark as a normal data message for display
          if (deviceRole == ROLE_DOCTOR1) {
            esp_now_send(display1MAC, (uint8_t*)&displayItem, sizeof(displayItem));
          } else if (deviceRole == ROLE_DOCTOR2) {
            esp_now_send(display2MAC, (uint8_t*)&displayItem, sizeof(displayItem));
          }
          Serial.printf("📢 Calling Patient Number %d...\n", nextEntry.number);
          blinkLED(GREEN_LED_PIN);
          sharedQueue.print();
        }

      } else {
        // *** Patient card scanned on a doctor node ***
        if (sharedQueue.exists(uid)) {
          // The patient is in the queue – remove this specific patient (possibly out-of-order)
          QueueEntry entryToRemove;
          for (const auto &e : sharedQueue.getQueue()) {
            if (e.uid == uid) { entryToRemove = e; break; }
          }
          sharedQueue.removeByUID(uid);
          // Prepare removal message and display message
          QueueItem remItem;
          memset(&remItem, 0, sizeof(remItem));
          strncpy(remItem.uid, entryToRemove.uid.c_str(), sizeof(remItem.uid) - 1);
          strncpy(remItem.timestamp, entryToRemove.timestamp.c_str(), sizeof(remItem.timestamp) - 1);
          remItem.number = entryToRemove.number;
          remItem.removeFromQueue = true;
          // Broadcast removal to arrival and other doctor
          esp_now_send(arrivalMAC, (uint8_t*)&remItem, sizeof(remItem));
          if (deviceRole == ROLE_DOCTOR1) {
            esp_now_send(doctor2MAC, (uint8_t*)&remItem, sizeof(remItem));
          } else {
            esp_now_send(doctor1MAC, (uint8_t*)&remItem, sizeof(remItem));
          }
          // Inform this doctor's display of the called patient
          QueueItem displayItem = remItem;
          displayItem.removeFromQueue = false;
          if (deviceRole == ROLE_DOCTOR1) {
            esp_now_send(display1MAC, (uint8_t*)&displayItem, sizeof(displayItem));
          } else {
            esp_now_send(display2MAC, (uint8_t*)&displayItem, sizeof(displayItem));
          }
          Serial.printf("✅ Removed patient %d from queue (UID: %s).\n", entryToRemove.number, entryToRemove.uid.c_str());
          Serial.println("⚠️ (Patient removed out of turn)");
          blinkLED(GREEN_LED_PIN);
          sharedQueue.print();
        } else {
          // The card is not in the queue
          int pid = getPermanentNumber(uid);
          if (pid != -1) {
            // Card belongs to a known patient but they're not queued currently
            Serial.printf("ℹ️ Patient #%d is not in the queue.\n", pid);
          } else {
            // Card is completely unknown/unrecognized
            Serial.println("❌ Unauthorized card – access denied.");
          }
          blinkLED(RED_LED_PIN);
        }
      }
    }  // end of doctor node handling

    // Halt PICC and stop encryption to ready for the next card read
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(1500);  // small delay to debounce card removal and provide visual feedback
  }
}
