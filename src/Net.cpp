#include "Net.h"

#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

namespace net
{
namespace
{
Preferences prefs;

struct Cred { String ssid, pass; };

Cred  _creds[kMaxSaved];
int   _count = 0;
String _target;

enum State : uint8_t { ST_IDLE, ST_RANKING, ST_CONNECTING, ST_UP, ST_FAILED };
State    _state    = ST_IDLE;
uint32_t _deadline = 0;
uint32_t _nextTry  = 0;
bool     _started  = false;

// Candidate chain for one connection round.
uint8_t _order[kMaxSaved];
uint8_t _nOrder = 0;
uint8_t _cand   = 0;
int16_t _seenRssi[kMaxSaved];
uint32_t _rankAt = 0;
uint32_t _downAt = 0;          // when the link first looked dead (scan blips)
uint32_t _lastRank = 0;        // when the last ranking sweep ran
uint8_t  _roundFails = 0;      // full failed rounds, drives the retry backoff

// UI scan session (the sweep itself is blocking, see doScan()).
bool     _uiScan      = false;
uint32_t _uiScanAt    = 0;
char     _scanErrBuf[48] = "";

void setScanErr(const char* s)
{
  snprintf(_scanErrBuf, sizeof(_scanErrBuf), "%s", s ? s : "");
}

// ---------------------------------------------------------------------------
// NVS: kMaxSaved slots, slot 0 = most recently used
// ---------------------------------------------------------------------------
void persist()
{
  prefs.begin("mp3net", false);
  prefs.putUChar("cnt", (uint8_t)_count);
  for (int i = 0; i < kMaxSaved; ++i)
  {
    String ks = String("s") + i, kp = String("p") + i;
    if (i < _count)
    {
      prefs.putString(ks.c_str(), _creds[i].ssid);
      prefs.putString(kp.c_str(), _creds[i].pass);
    }
    else
    {
      prefs.remove(ks.c_str());
      prefs.remove(kp.c_str());
    }
  }
  prefs.end();
}

int findCred(const String& s)
{
  if (!s.length()) return -1;
  for (int i = 0; i < _count; ++i)
    if (_creds[i].ssid == s) return i;
  return -1;
}

void loadCreds()
{
  prefs.begin("mp3net", false);
  // The single-network record written by older firmware: keep it as slot 0.
  String legacySsid = prefs.getString("ssid", "");
  String legacyPass = prefs.getString("pass", "");
  _count = prefs.getUChar("cnt", 0);
  if (_count > kMaxSaved) _count = kMaxSaved;

  Cred tmp[kMaxSaved];
  int n = 0;
  for (int i = 0; i < _count; ++i)
  {
    String s = prefs.getString((String("s") + i).c_str(), "");
    if (!s.length()) continue;                    // hole: skip, keep order
    tmp[n].ssid = s;
    tmp[n].pass = prefs.getString((String("p") + i).c_str(), "");
    ++n;
  }
  prefs.end();

  if (legacySsid.length())
  {
    bool dup = false;
    for (int i = 0; i < n; ++i) if (tmp[i].ssid == legacySsid) dup = true;
    if (!dup && n < kMaxSaved) { tmp[n].ssid = legacySsid; tmp[n].pass = legacyPass; ++n; }
  }

  _count = n;
  for (int i = 0; i < n; ++i) _creds[i] = tmp[i];
  if (legacySsid.length())
  {
    prefs.begin("mp3net", false);
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
    persist();
  }
}

/// Insert or refresh a network and move it to slot 0. Evicts the oldest.
void remember(const String& s, const String& p)
{
  if (!s.length()) return;
  int at = findCred(s);
  if (at == 0)
  {
    _creds[0].pass = p;
    persist();
    return;
  }
  int last = (_count < kMaxSaved) ? _count : kMaxSaved - 1;
  for (int i = last; i > 0; --i) _creds[i] = _creds[i - 1];
  _creds[0].ssid = s;
  _creds[0].pass = p;
  if (_count < kMaxSaved) ++_count;
  persist();
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------
// Results come back in the driver's buffer until scanDelete().
void sortAndDedupe(Ap* out, int& cnt)
{
  for (int i = 1; i < cnt; ++i)
  {
    Ap k = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].rssi < k.rssi) { out[j + 1] = out[j]; --j; }
    out[j + 1] = k;
  }
}

/// One blocking sweep.
/// An async scan is what used to reboot the device: while a station is
/// connected (and the radio is streaming) the second result buffer can fail to
/// allocate, and the core asserts. A bounded blocking sweep allocates one
/// result set, finishes in ~1.5 s and leaves the driver in a known state.
int doScan(Ap* out, int max)
{
  setScanErr("");
  if (ESP.getFreeHeap() < 22000)
  {
    setScanErr("内存不足，先停止电台再扫描");
    return -1;
  }

  WiFi.scanDelete();
  int n = WiFi.scanNetworks(false /*blocking*/, true /*hidden*/, false, 110 /*ms per channel*/);
  if (n < 0)
  {
    WiFi.scanDelete();
    setScanErr("扫描被拒绝，请重试");
    return -1;
  }

  int cnt = 0;
  for (int i = 0; i < n && cnt < max; ++i)
  {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    int32_t r = WiFi.RSSI(i);
    bool dup = false;
    for (int k = 0; k < cnt; ++k)
    {
      if (out[k].ssid == s)
      {
        if (r > out[k].rssi) out[k].rssi = r;
        dup = true;
        break;
      }
    }
    if (dup) continue;
    out[cnt].ssid   = s;
    out[cnt].rssi   = r;
    out[cnt].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    ++cnt;
  }
  WiFi.scanDelete();

  sortAndDedupe(out, cnt);
  if (!cnt) setScanErr("没扫描到网络");
  return cnt;
}

// ---------------------------------------------------------------------------
// Connection rounds
// ---------------------------------------------------------------------------
void beginCand(int k)
{
  if (k >= _nOrder)
  {
    // Every saved network failed. Back off so a card with none of them around
    // does not freeze the UI for a sweep every half minute.
    if (_roundFails < 6) ++_roundFails;
    uint32_t d = 30000UL << (_roundFails > 1 ? _roundFails - 1 : 0);
    if (d > 600000UL) d = 600000UL;
    _state = ST_FAILED;
    _nextTry = millis() + d;
    _target = "";
    return;
  }
  _cand = (uint8_t)k;
  const Cred& c = _creds[_order[k]];
  _target = c.ssid;
  WiFi.disconnect(true);
  WiFi.begin(c.ssid.c_str(), c.pass.c_str());
  _state = ST_CONNECTING;
  _deadline = millis() + 12000;
}

/// Rank the saved networks and start trying them: visible ones first, strongest
/// first, then the ones the scan could not see (they may be out of range now).
void doRank()
{
  Ap found[24];
  int n = doScan(found, 24);
  _lastRank = millis();

  for (int i = 0; i < _count; ++i)
  {
    _seenRssi[i] = -128;
    for (int k = 0; k < n; ++k)
      if (found[k].ssid == _creds[i].ssid) { _seenRssi[i] = found[k].rssi; break; }
  }

  bool used[kMaxSaved];
  for (int i = 0; i < kMaxSaved; ++i) used[i] = false;
  _nOrder = 0;
  for (;;)
  {
    int best = -1;
    for (int i = 0; i < _count; ++i)
      if (!used[i] && _seenRssi[i] > -128 && (best < 0 || _seenRssi[i] > _seenRssi[best])) best = i;
    if (best < 0) break;
    used[best] = true;
    _order[_nOrder++] = (uint8_t)best;
  }
  for (int i = 0; i < _count; ++i)
    if (!used[i]) _order[_nOrder++] = (uint8_t)i;

  beginCand(0);
}

void startRound()
{
  if (!_count) { _state = ST_IDLE; return; }
  if (_count == 1)
  {
    _nOrder = 1;
    _order[0] = 0;
    beginCand(0);
    return;
  }
  // Re-rank only when the order is stale: a sweep freezes the UI for a moment,
  // so it is not worth doing on every retry.
  bool stale = _lastRank == 0 || (millis() - _lastRank > 300000UL);
  if (stale)
  {
    _state = ST_RANKING;
    _rankAt = millis();          // sweep on the next loop() tick
  }
  else beginCand(0);
}
} // namespace

bool begin()
{
  loadCreds();

  WiFi.persistent(false);      // we own the NVS record, not the core
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);        // power save makes streaming stutter
  WiFi.setAutoReconnect(false); // our state machine decides who to join
  _started = true;

  if (_count) startRound();
  return _count > 0;
}

void connectTo(const String& s, const String& p)
{
  if (!s.length()) return;
  remember(s, p);              // slot 0, persisted
  _target = s;
  _roundFails = 0;

  // Rank right away: this network first, the other saved ones as fallbacks.
  _nOrder = 0;
  _order[_nOrder++] = 0;
  for (int i = 1; i < _count; ++i) _order[_nOrder++] = (uint8_t)i;
  beginCand(0);
}

void forget()
{
  _count = 0;
  _target = "";
  _order[0] = 0;
  _nOrder = 0;
  persist();
  WiFi.disconnect(true);
  _state = ST_IDLE;
}

void forgetSsid(const String& s)
{
  int at = findCred(s);
  if (at < 0) return;
  for (int i = at; i < _count - 1; ++i) _creds[i] = _creds[i + 1];
  if (_count) --_count;
  _creds[kMaxSaved - 1] = Cred();
  persist();
  if (s == _target)
  {
    _target = "";
    WiFi.disconnect(true);
    _state = ST_IDLE;
    _nextTry = millis();      // start a round immediately with what is left
  }
}

void loop()
{
  if (!_started) return;
  uint32_t now = millis();

  switch (_state)
  {
    case ST_RANKING:
      if ((int32_t)(now - _rankAt) >= 0) doRank();
      break;

    case ST_CONNECTING:
      if (WiFi.status() == WL_CONNECTED)
      {
        _state = ST_UP;
        _downAt = 0;
        _roundFails = 0;
        break;
      }
      if ((int32_t)(now - _deadline) >= 0)
      {
        WiFi.disconnect(true);
        beginCand(_cand + 1);
      }
      break;

    case ST_UP:
      // A channel sweep can make the link look dead for a moment; only believe
      // it after it stays down, otherwise scanning would kill a good connection.
      if (WiFi.status() != WL_CONNECTED)
      {
        if (!_downAt) _downAt = now ? now : 1;
        else if (now - _downAt > 3000)
        {
          _downAt = 0;
          _state = ST_IDLE;
          _nextTry = now + 2000;
        }
      }
      else _downAt = 0;
      break;

    case ST_IDLE:
    case ST_FAILED:
      if (_count && (int32_t)(now - _nextTry) >= 0) startRound();
      break;
  }
}

bool connected()  { return WiFi.status() == WL_CONNECTED; }
bool connecting() { return _state == ST_CONNECTING || _state == ST_RANKING; }
int  rssi()       { return connected() ? WiFi.RSSI() : 0; }
bool hasSaved()   { return _count > 0; }
int  savedCount() { return _count; }
bool isSaved(const String& s) { return findCred(s) >= 0; }

bool savedPass(const String& s, String& out)
{
  int at = findCred(s);
  if (at < 0) return false;
  out = _creds[at].pass;
  return true;
}

const String& ssid()
{
  static String live;
  if (connected())
  {
    String s = WiFi.SSID();
    if (s.length()) live = s;
    return live;
  }
  return _target;
}

const char* status()
{
  if (connected())          return "已连接";
  if (_state == ST_CONNECTING) return "连接中…";
  if (_state == ST_RANKING)    return "挑选网络…";
  if (_state == ST_FAILED)     return "连不上";
  return "未连接";
}

// ---------------------------------------------------------------------------
// UI scan
// ---------------------------------------------------------------------------
void scanStart()
{
  // A pending join and a sweep fight over the radio; drop the join, the state
  // machine starts a fresh round once the list is closed.
  if (_state == ST_CONNECTING || _state == ST_RANKING)
  {
    WiFi.disconnect(true);
    _state = ST_IDLE;
    _nextTry = millis() + 15000;
  }
  _uiScan = true;
  _uiScanAt = millis() + 200;   // let the UI paint "正在扫描…" first
  setScanErr("");
}

int scanPoll(Ap* out, int max)
{
  if (!_uiScan) return 0;
  if ((int32_t)(millis() - _uiScanAt) < 0) return -1;
  int n = doScan(out, max);
  _uiScan = false;
  return n < 0 ? 0 : n;
}

const char* scanError() { return _scanErrBuf; }

} // namespace net
