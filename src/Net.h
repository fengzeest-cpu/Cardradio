// Net.h — WiFi for the radio. Remembered networks are stored in NVS: on boot
// the strongest visible one is joined, the rest act as fallbacks, so moving
// between home / office / phone hotspot needs no re-typing. There is no real
// reason to cap this: 20 credentials cost about 4 KB of the 20 KB NVS
// partition, so the list grows until the storage is the limit.
#pragma once

#include <Arduino.h>

namespace net
{
struct Ap
{
  String  ssid;
  int32_t rssi;
  bool    secure;
};

constexpr int kMaxSaved = 20;

bool begin();                              // load saved networks, start joining
void loop();                               // advance the state machine
bool connected();
bool connecting();                         // connecting OR picking a network
int  rssi();
const String& ssid();                      // network we are on
bool hasSaved();
int  savedCount();
bool isSaved(const String& ssid);
bool savedPass(const String& ssid, String& out);

void connectTo(const String& ssid, const String& pass);   // saves it too
void forget();                                            // wipe all networks
void forgetSsid(const String& ssid);                       // wipe one

void scanStart();
/// -1 while the scan runs, otherwise the number of networks written to out.
int  scanPoll(Ap* out, int max);
const char* scanError();                   // why the last scan came back empty

const char* status();                      // 未连接 / 连接中… / 已连接
} // namespace net
