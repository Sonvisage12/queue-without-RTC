#include <Preferences.h>

Preferences prefs;

void setup() {
  Serial.begin(115200);
  prefs.begin("rfidMap", false);  // false = write mode
  //prefs.clear();
  // Example: add multiple UID→Number entries
  prefs.putUInt("13B6B1E3", 1);
  prefs.putUInt("13D7ADE3", 2);
  prefs.putUInt("A339D9E3", 3);
  prefs.putUInt("220C1805", 4);
  prefs.putUInt("638348E3", 5);
  prefs.putUInt("A3E9C7E3", 6);
  prefs.putUInt("5373BEE3", 7);
  prefs.putUInt("62EDFF51", 8);
  prefs.putUInt("131DABE3", 9);
  prefs.putUInt("B3D4B0E3", 10);
  prefs.putUInt("23805EE3", 11);
  prefs.putUInt("1310BAE3", 12);
  prefs.putUInt("D38A47E3", 13);
  prefs.putUInt("6307D8E3", 14);
  prefs.putUInt("D35FC4E3", 15);
  prefs.putUInt("C394B9E3", 16);
  // ... up to UID_XXXXXXX = 2000

  prefs.end();
  Serial.println("✅ UID→Number mapping saved to Preferences.");
}

void loop() {}
