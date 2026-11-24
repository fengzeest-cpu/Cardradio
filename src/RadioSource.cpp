#include "RadioSource.h"
#include <esp_heap_caps.h>

RadioSource::RadioSource()
{
  _mux = xSemaphoreCreateMutex();
}

RadioSource::~RadioSource()
{
  close();
  if (_ring) { heap_caps_free(_ring); _ring = nullptr; }
  if (_mux)  { vSemaphoreDelete(_mux); _mux = nullptr; }
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
bool RadioSource::parseUrl(const String& url)
{
  if (!url.startsWith("http://")) return false;      // HTTPS is not linked in
  const int start = 7;
  int slash = url.indexOf('/', start);
  String hostPort = (slash < 0) ? url.substring(start) : url.substring(start, slash);
  _path = (slash < 0) ? String("/") : url.substring(slash);

  int colon = hostPort.indexOf(':');
  if (colon > 0)
  {
    _host = hostPort.substring(0, colon);
    long p = hostPort.substring(colon + 1).toInt();
    _port = (p > 0 && p < 65536) ? (uint16_t)p : 80;
  }
  else
  {
    _host = hostPort;
    _port = 80;
  }
  return _host.length() > 0;
}

bool RadioSource::openSocket()
{
  _client.stop();
  if (!_client.connect(_host.c_str(), _port, 8000)) return false;
  _client.setNoDelay(true);

  // HTTP/1.0 keeps servers from chunking the body and makes them close the
  // socket when they stop sending, which is exactly how we detect EOF.
  _client.print(String("GET ") + _path + " HTTP/1.0\r\n");
  _client.print(String("Host: ") + _host + (_port == 80 ? String("") : String(":") + _port) + "\r\n");
  // Some CDNs throttle or refuse unknown clients; an Edge UA keeps them happy.
  _client.print("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36 Edg/126.0.0.0\r\n");
  _client.print("Accept: */*\r\n");
  _client.print("Icy-MetaData: 1\r\n");
  _client.print("Connection: close\r\n\r\n");
  return true;
}

RadioSource::Wait RadioSource::waitData(uint32_t timeoutMs)
{
  uint32_t start = millis();
  for (;;)
  {
    if (_stop) return W_CLOSED;                            // close in progress: never linger
    if (_client.available() > 0) return W_DATA;
    if (!_client.connected()) return W_CLOSED;
    if (millis() - start >= timeoutMs) return W_TIMEOUT;
    delay(2);
  }
}

bool RadioSource::readLine(String& out, uint32_t timeoutMs)
{
  out = "";
  uint32_t last = millis();
  for (;;)
  {
    Wait w = waitData(50);
    if (w == W_CLOSED) return false;
    if (w == W_TIMEOUT)
    {
      if (millis() - last >= timeoutMs) return false;
      continue;
    }
    int c = _client.read();
    if (c < 0) return false;
    if (c == '\n')
    {
      if (out.endsWith("\r")) out.remove(out.length() - 1);
      return true;
    }
    out += (char)c;
    if (out.length() > 400) return false;
    last = millis();
  }
}

bool RadioSource::readHeaders(int& status, String& location)
{
  String line;
  if (!readLine(line, 6000)) return false;
  status = 0;
  int sp = line.indexOf(' ');
  if (sp > 0) status = line.substring(sp + 1).toInt();     // "HTTP/1.1 200 OK" / "ICY 200 OK"
  if (!status) return false;

  _chunked = false;
  _metaInterval = 0;
  location = "";

  for (int i = 0; i < 40; ++i)
  {
    if (!readLine(line, 4000)) return false;
    if (!line.length()) break;                             // blank line ends the headers
    int colon = line.indexOf(':');
    if (colon <= 0) continue;
    String k = line.substring(0, colon);
    k.toLowerCase();
    k.trim();
    String v = line.substring(colon + 1);
    v.trim();
    if (k == "icy-metaint") _metaInterval = v.toInt();
    else if (k == "transfer-encoding" && v.indexOf("chunked") >= 0) _chunked = true;
    else if (k == "location") location = v;
  }
  return true;
}

bool RadioSource::open(const char* url)
{
  close();
  _url = url;

  for (int hop = 0; hop < 4; ++hop)
  {
    if (!parseUrl(_url)) return false;
    if (!openSocket()) return false;

    int status = 0;
    String location;
    bool ok = readHeaders(status, location);
    _status = status;
    if (!ok) { _client.stop(); return false; }

    if (status == 200)
    {
      _pos = 0;
      _chunkLeft = 0;
      _metaLeft = _metaInterval;
      _open = true;

  // Ring buffer: PSRAM first (if the board has it), then a small internal
  // fallback so heap fragmentation can't kill playback.
  //
  // MALLOC_CAP_8BIT is not optional: without it the caps are not a valid
  // PSRAM request, and the block silently ends up in (or fails from) the
  // internal heap — which is exactly the scarce memory here.
  if (!_ring)
  {
    _ring = (uint8_t*)heap_caps_malloc(kRingMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (_ring) { _ringCap = kRingMax; _ringPsram = true; }
    if (!_ring)
    {
      _ring = (uint8_t*)heap_caps_malloc(8 * 1024, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      if (_ring) { _ringCap = 8 * 1024; _ringPsram = false; }
    }
  }
      _stop = false;
      if (_ring && !startFill()) { _open = false; _client.stop(); return false; }

      // Give the filler a head start so playback doesn't stutter from byte one.
      if (_ring)
      {
        uint32_t t0 = millis();
        for (;;)
        {
          uint32_t have;
          xSemaphoreTake(_mux, portMAX_DELAY); have = _wHead - _rTail; xSemaphoreGive(_mux);
          if (have >= kPrefill || millis() - t0 >= 800) break;
          delay(5);
        }
      }
      return true;
    }

    _client.stop();
    if (!location.length()) return false;
    if (location.startsWith("http://")) _url = location;
    else if (location.startsWith("/"))
      _url = String("http://") + _host + (_port == 80 ? String("") : String(":") + _port) + location;
    else
    {
      int slash = _url.lastIndexOf('/');
      _url = _url.substring(0, slash + 1) + location;
    }
  }
  return false;
}

bool RadioSource::close()
{
  _open = false;
  _stop = true;                    // the filler exits by itself at the next check

  // Wait for the filler to wind down and delete itself. Stopping the socket
  // or deleting the task from outside while it sits inside lwIP corrupts
  // memory and reboots the device — this is the one path that must not race.
  //
  // There is deliberately no vTaskDelete() here: killing a task that is
  // blocked in recv() is exactly what used to reboot the card when a station
  // was switched. If the filler is stuck, it closes the socket itself when it
  // finally gets out; the next open() re-uses the same client anyway.
  uint32_t t0 = millis();
  bool gone  = false;
  for (;;)
  {
    xSemaphoreTake(_mux, portMAX_DELAY);
    TaskHandle_t h = _fillTask;
    xSemaphoreGive(_mux);
    if (!h) { gone = true; break; }
    if (millis() - t0 >= 3000) break;      // give up waiting, never kill it
    delay(2);
  }

  if (gone) _client.stop();                // filler is gone — safe to tear down now
  xSemaphoreTake(_mux, portMAX_DELAY);
  _wHead = _rTail = 0;
  _srcDone = false;
  xSemaphoreGive(_mux);
  _stop = false;
  return true;
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
bool RadioSource::nextChunk()
{
  for (int i = 0; i < 4; ++i)
  {
    String line;
    if (!readLine(line, 3000)) return false;
    if (!line.length()) continue;                          // CRLF that closed the last chunk
    long v = strtol(line.c_str(), nullptr, 16);
    _chunkLeft = v > 0 ? (uint32_t)v : 0;
    return true;
  }
  return false;
}

bool RadioSource::skipMeta()
{
  _metaLeft = _metaInterval;

  if (waitData(1500) != W_DATA) return false;
  int n = _client.read(_metaBuf, 1);
  if (n != 1) return false;

  int blocks = _metaBuf[0];                                // length in 16-byte units
  if (!blocks) return true;

  // The block itself carries no information we use any more — only the
  // audio behind it matters. Skip over it so the decoder never sees it.
  int need = blocks * 16;
  while (need > 0)
  {
    int chunk = need < (int)sizeof(_metaBuf) ? need : (int)sizeof(_metaBuf);
    if (waitData(800) != W_DATA) return false;
    int r = _client.read(_metaBuf, chunk);
    if (r <= 0) return false;
    need -= r;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Ring buffer + filler task
// ---------------------------------------------------------------------------
void RadioSource::pushRing(const uint8_t* p, uint32_t n)
{
  xSemaphoreTake(_mux, portMAX_DELAY);
  uint32_t space = _ringCap - (_wHead - _rTail);
  if (n > space) n = space;                                // shouldn't happen: filler checks first
  uint32_t idx = _wHead % _ringCap;
  uint32_t first = _ringCap - idx < n ? _ringCap - idx : n;
  memcpy(_ring + idx, p, first);
  if (n > first) memcpy(_ring, p + first, n - first);
  _wHead += n;
  xSemaphoreGive(_mux);
}

uint32_t RadioSource::popRing(void* dst, uint32_t len)
{
  xSemaphoreTake(_mux, portMAX_DELAY);
  uint32_t have = _wHead - _rTail;
  if (len > have) len = have;
  uint32_t idx = _rTail % _ringCap;
  uint32_t first = _ringCap - idx < len ? _ringCap - idx : len;
  memcpy(dst, _ring + idx, first);
  if (len > first) memcpy((uint8_t*)dst + first, _ring, len - first);
  _rTail += len;
  xSemaphoreGive(_mux);
  return len;
}

bool RadioSource::startFill()
{
  xSemaphoreTake(_mux, portMAX_DELAY);
  _srcDone = false;
  xSemaphoreGive(_mux);
  // 4 KB is plenty: this task only moves bytes, all buffers are members.
  if (xTaskCreatePinnedToCore(fillTrampoline, "radiofill", 4096, this, 3, &_fillTask, 0) != pdPASS)
  {
    _fillTask = nullptr;
    return false;
  }
  return true;
}

void RadioSource::fillTrampoline(void* arg)
{
  static_cast<RadioSource*>(arg)->fillLoop();              // never returns: deletes itself
}

void RadioSource::fillLoop()
{
  for (;;)
  {
    if (_stop || !_open) break;

    if (_metaInterval > 0 && _metaLeft <= 0)
    {
      if (!skipMeta()) break;
      continue;
    }
    if (_chunked && _chunkLeft == 0)
    {
      if (!nextChunk()) break;
      if (_chunkLeft == 0) break;                          // end of body
    }

    uint32_t space;
    xSemaphoreTake(_mux, portMAX_DELAY);
    space = _ringCap - (_wHead - _rTail);
    xSemaphoreGive(_mux);
    if (space < 256) { delay(6); continue; }               // buffer is full: let the decoder catch up

    uint32_t want = space < kPullMax ? space : kPullMax;
    if (_chunked && want > _chunkLeft) want = _chunkLeft;
    if (_metaInterval > 0 && want > (uint32_t)_metaLeft) want = _metaLeft;

    Wait w = waitData(400);
    if (w == W_TIMEOUT) continue;
    if (w == W_CLOSED) break;                              // server hung up: drain, then EOF
    int avail = _client.available();
    if (avail <= 0) continue;
    if ((uint32_t)avail < want) want = avail;
    int n = _client.read(_pullBuf, want);
    if (n <= 0) { delay(2); continue; }

    pushRing(_pullBuf, n);
    _pos += n;
    if (_chunked) _chunkLeft -= n;
    if (_metaInterval > 0) _metaLeft -= n;
  }

  // The filler owns the socket: it closes it on its way out, so nobody else
  // ever pulls the plug on a task that might sit inside lwIP.
  _client.stop();

  xSemaphoreTake(_mux, portMAX_DELAY);
  _srcDone = true;                                         // any wait below is terminal
  _fillTask = nullptr;
  xSemaphoreGive(_mux);
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Decoder side
// ---------------------------------------------------------------------------
uint32_t RadioSource::read(void* dst, uint32_t len)
{
  return readInternal(dst, len, false);
}

uint32_t RadioSource::readNonBlock(void* dst, uint32_t len)
{
  return readInternal(dst, len, true);
}

uint32_t RadioSource::readInternal(void* dst, uint32_t len, bool nonBlock)
{
  if (!_open || !len) return 0;
  return _ring ? readBuffered(dst, len, nonBlock) : readDirect(dst, len, nonBlock);
}

// Buffered mode: the fill task keeps the ring topped up; we only drain it.
uint32_t RadioSource::readBuffered(void* dst, uint32_t len, bool nonBlock)
{
  uint8_t* out = (uint8_t*)dst;
  uint32_t got = 0;
  uint32_t deadline = millis() + (nonBlock ? 0 : 3000);

  while (got < len)
  {
    if (_stop) break;
    got += popRing(out + got, len - got);
    if (got >= len) break;

    xSemaphoreTake(_mux, portMAX_DELAY);
    bool hasData = (_wHead - _rTail) > 0;
    bool eof    = _srcDone;
    xSemaphoreGive(_mux);

    if (!hasData && (eof || nonBlock || (int32_t)(millis() - deadline) >= 0))
    {
      if (!got && !nonBlock) _open = false;                // stalled/ended: let the player reconnect
      break;
    }
    if (!hasData) delay(4);
  }
  return got;
}

// Unbuffered fallback (no ring memory available): pull straight from the
// socket, stripping ICY metadata on the fly — the original behaviour.
uint32_t RadioSource::readDirect(void* dst, uint32_t len, bool nonBlock)
{
  uint8_t* out = (uint8_t*)dst;
  uint32_t got = 0;

  while (got < len)
  {
    if (_metaInterval > 0 && _metaLeft <= 0)
    {
      if (!skipMeta()) { if (got) break; _open = false; break; }
      if (!got && nonBlock) break;
    }

    if (_chunked && _chunkLeft == 0)
    {
      if (!nextChunk()) { if (got) break; _open = false; break; }
      if (_chunkLeft == 0) { if (got) break; _open = false; break; }   // end of body
    }

    uint32_t want = len - got;
    if (_chunked && want > _chunkLeft) want = _chunkLeft;
    if (_metaInterval > 0 && want > (uint32_t)_metaLeft) want = _metaLeft;
    if (!want) continue;

    Wait w = waitData(nonBlock ? 0 : (got ? 300 : 4000));
    if (w == W_TIMEOUT)
    {
      if (got || nonBlock) break;
      _open = false;                                       // stalled: let the player reconnect
      break;
    }
    if (w == W_CLOSED)
    {
      if (got) break;
      _open = false;
      break;
    }

    int avail = _client.available();
    if (avail <= 0) continue;
    if ((uint32_t)avail < want) want = avail;

    int n = _client.read(out + got, want);
    if (n <= 0)
    {
      if (got) break;
      _open = false;
      break;
    }
    got += n;
    _pos += n;
    if (_chunked) _chunkLeft -= n;
    if (_metaInterval > 0) _metaLeft -= n;
  }
  return got;
}

