// HlsSource.cpp — HLS (HTTP Live Streaming) playback for internet radio.
//
// The chain is: .m3u8 playlist -> MPEG-TS segments -> PES packets -> audio
// frames. Everything is processed as it arrives; no segment is ever held in
// memory in full.
//
// MPEG-TS basics (ISO/IEC 13818-1):
//   - A segment is a sequence of 188-byte packets, each starting with 0x47.
//   - The 4th byte carries the payload unit start bit and the stream id.
//   - For audio (stream id 0x0F) the payload is PES: an optional header
//     followed by the elementary stream.
//   - The PES header length tells us how long that header is; for video it
//     can be extended, for audio it never is.
//   - ADTS AAC frames start with 0xFFF, MP3 frames with 0xFFEx/0xFFFx.
#include "HlsSource.h"

#include <string.h>

namespace
{
  constexpr uint32_t kOutBufSize = 24 * 1024;   // audio frames waiting for the decoder
  constexpr uint32_t kSegBufMax  = 16 * 1024;  // per-segment URL list

  // URI path without its query string: real-world playlists name their
  // segments "seg001.ts?token=..." so extension checks must ignore "?...".
  String pathOf(const String& u)
  {
    int q = u.indexOf('?');
    return q >= 0 ? u.substring(0, q) : u;
  }
}

HlsSource::HlsSource()
{
  _outBuf = (uint8_t*)malloc(kOutBufSize);
  _outCap = _outBuf ? kOutBufSize : 0;
}

HlsSource::~HlsSource()
{
  close();
  if (_outBuf) { free(_outBuf); _outBuf = nullptr; _outCap = 0; }
}

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

bool HlsSource::parseUrl(const String& url, String& host, String& path, uint16_t& port)
{
  host = "";
  path = "";
  port = 80;

  const char* p = url.c_str();
  if (!strncmp(p, "http://", 7)) p += 7;
  else if (!strncmp(p, "https://", 8)) { p += 8; port = 443; }
  else return false;

  const char* slash = strchr(p, '/');
  String hostPart = slash ? String(p, slash - p) : String(p);
  path = slash ? String(slash) : String("/");

  int colon = hostPart.lastIndexOf(':');
  if (colon > 0)
  {
    port = (uint16_t)hostPart.substring(colon + 1).toInt();
    hostPart = hostPart.substring(0, colon);
  }
  if (!hostPart.length()) return false;
  host = hostPart;
  return true;
}

String HlsSource::absoluteUrl(const String& base, const String& rel)
{
  if (rel.startsWith("http://") || rel.startsWith("https://")) return rel;

  // Keep the scheme of the playlist we resolved this from: CNR and friends
  // are https-only, so a hardwired "http://" would break every relative URI.
  const char* scheme = "http://";
  const char* bp = base.c_str();
  if (!strncmp(bp, "https://", 8)) scheme = "https://";

  String host, path;
  uint16_t port;
  if (!parseUrl(base, host, path, port)) return rel;

  String portPart = (port == 80 || port == 443) ? String("") : String(":") + port;

  if (rel.startsWith("/"))
    return String(scheme) + host + portPart + rel;

  int slash = path.lastIndexOf('/');
  String dir = slash >= 0 ? path.substring(0, slash + 1) : String("/");
  return String(scheme) + host + portPart + dir + rel;
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

HlsSource::Wait HlsSource::waitData(uint32_t timeoutMs)
{
  uint32_t start = millis();
  for (;;)
  {
    if (_client->available() > 0) return W_DATA;
    if (!_client->connected()) return W_CLOSED;
    if (millis() - start >= timeoutMs) return W_TIMEOUT;
    delay(2);
  }
}

bool HlsSource::readLine(NetworkClient& c, String& out, uint32_t timeoutMs)
{
  out = "";
  uint32_t start = millis();
  for (;;)
  {
    Wait w = waitData(50);
    if (w == W_CLOSED) return false;
    if (w == W_TIMEOUT)
    {
      if (millis() - start >= timeoutMs) return false;
      continue;
    }
    int ch = c.read();
    if (ch < 0) return false;
    if (ch == '\n')
    {
      if (out.endsWith("\r")) out.remove(out.length() - 1);
      return true;
    }
    out += (char)ch;
    if (out.length() > 512) return false;
    start = millis();
  }
}

bool HlsSource::readAll(NetworkClient& c, String& out, uint32_t maxLen, uint32_t timeoutMs)
{
  out = "";
  uint32_t start = millis();
  while (out.length() < maxLen)
  {
    Wait w = waitData(100);
    if (w == W_CLOSED) break;
    if (w == W_TIMEOUT)
    {
      if (millis() - start >= timeoutMs) break;
      continue;
    }
    int n = c.available();
    if (n <= 0) continue;
    char buf[256];
    int want = n < (int)sizeof(buf) ? n : (int)sizeof(buf);
    int got = c.read((uint8_t*)buf, want);
    if (got <= 0) break;
    out += String(buf, got);
    start = millis();
  }
  return out.length() > 0;
}

/// One-shot HTTP GET. Used for the playlist; segments are streamed instead.
/// Selects the plain or TLS client based on the URL scheme/port.
bool HlsSource::httpGet(const String& url, int& status)
{
  String host, path;
  uint16_t port;
  if (!parseUrl(url, host, path, port)) return false;

  NetworkClient* c = (port == 443) ? (NetworkClient*)&_sec : (NetworkClient*)&_plain;
  _client = c;                                 // remember for the body read
  c->stop();
  if (c == &_sec) _sec.setInsecure();          // radio TLS certs: we don't verify
  if (!c->connect(host.c_str(), port, 8000)) return false;
  c->setNoDelay(true);

  c->print(String("GET ") + path + " HTTP/1.0\r\n");
  c->print(String("Host: ") + host + (port == 80 || port == 443 ? String("") : String(":") + port) + "\r\n");
  c->print("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/126.0.0.0 Safari/537.36\r\n");
  c->print("Accept: */*\r\n");
  c->print("Connection: close\r\n\r\n");

  String line;
  if (!readLine(*c, line, 5000)) { c->stop(); return false; }
  int sp = line.indexOf(' ');
  if (sp < 0) { c->stop(); return false; }
  status = line.substring(sp + 1, line.indexOf(' ', sp + 1)).toInt();

  // Skip the headers.
  for (;;)
  {
    if (!readLine(*c, line, 5000)) { c->stop(); return false; }
    if (line.length() == 0) break;
  }
  return status == 200;
}

// ---------------------------------------------------------------------------
// Playlist
// ---------------------------------------------------------------------------

bool HlsSource::fetchPlaylist(const String& url)
{
  int status = 0;
  if (!httpGet(url, status)) return false;
  NetworkClient* c = _client;

  String body;
  if (!readAll(*c, body, kSegBufMax, 6000)) { c->stop(); return false; }
  c->stop();

  // A master playlist points at the media playlists; the media playlist has
  // the segments. Keep the raw text and decide in pickVariant().
  _variantUrl = url;
  _segCount = 0;
  _segNext = 0;

  // Collect candidate lines for the next stage.
  static String lines[64];
  uint8_t lineCount = 0;
  int start = 0;
  while (start >= 0 && lineCount < 64)
  {
    int nl = body.indexOf('\n', start);
    String ln = (nl < 0) ? body.substring(start) : body.substring(start, nl);
    ln.trim();
    if (ln.length()) lines[lineCount++] = ln;
    if (nl < 0) break;
    start = nl + 1;
  }

  // Master playlist? Any URI line whose path ends in .m3u8.
  bool isMaster = false;
  for (uint8_t i = 0; i < lineCount; ++i)
  {
    if (lines[i].startsWith("#")) continue;
    if (pathOf(lines[i]).endsWith(".m3u8")) { isMaster = true; break; }
  }

  if (isMaster)
  {
    // A master playlist lists variants as "#EXT-X-STREAM-INF" followed by the
    // URI on the next non-comment line. Fall back to any non-comment .m3u8
    // line (again checking the path only, query strings are common here).
    for (uint8_t i = 0; i < lineCount; ++i)
    {
      if (!lines[i].startsWith("#EXT-X-STREAM-INF")) continue;
      // The variant URI is the next non-comment line.
      for (uint8_t j = (uint8_t)(i + 1); j < lineCount; ++j)
      {
        if (lines[j].startsWith("#")) continue;
        String next = absoluteUrl(_variantUrl, lines[j]);
        _variantUrl = next;
        return fetchPlaylist(next);        // recurse: nested masters are legal
      }
    }
    for (uint8_t i = 0; i < lineCount; ++i)
    {
      if (lines[i].startsWith("#")) continue;
      if (pathOf(lines[i]).endsWith(".m3u8"))
      {
        String next = absoluteUrl(_variantUrl, lines[i]);
        _variantUrl = next;
        return fetchPlaylist(next);
      }
    }
    return false;
  }

  // Media playlist: the .ts entries are the segments.
  // NOTE: real-world URIs carry query strings ("seg001.ts?token=..."), so the
  // extension is checked on the path part only — the full line is kept as URL.
  for (uint8_t i = 0; i < lineCount && _segCount < 8; ++i)
  {
    String ln = lines[i];
    if (ln.startsWith("#")) continue;
    String p = pathOf(ln);
    if (!p.endsWith(".ts") && !p.endsWith(".aac") && !p.endsWith(".mp3") &&
        !p.endsWith(".m4s") && !p.endsWith(".TS"))
      continue;
    _segUrls[_segCount++] = absoluteUrl(_variantUrl, ln);
  }
  return _segCount > 0;
}

/// Refresh the segment window. Called when the list runs low.
bool HlsSource::loadSegments()
{
  if (!fetchPlaylist(_variantUrl)) return false;
  _segNext = 0;
  return _segCount > 0;
}

// ---------------------------------------------------------------------------
// Transport stream demuxing
// ---------------------------------------------------------------------------

/// Feed TS bytes through the demuxer, emitting audio frames.
void HlsSource::demuxSegment()
{
  while (_segLeft && _client->available() > 0)
  {
    int avail = _client->available();
    uint32_t want = _segLeft;
    if (want > avail) want = avail;
    uint32_t room = 188 - _tsHave;
    if (want > room) want = room;

    int n = _client->read(_tsBuf + _tsHave, want);
    if (n <= 0) break;
    _tsHave += n;
    _segLeft -= n;

    if (_tsHave == 188)
    {
      _tsHave = 0;
      handleTsPacket(_tsBuf);
    }
  }
}

bool HlsSource::handleTsPacket(const uint8_t* p)
{
  // Sync byte check: the first byte is always 0x47.
  if (p[0] != 0x47) { _syncLost++; return false; }

  bool payloadStart = (p[1] & 0x40) != 0;
  uint16_t pid = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
  bool scrambled = (p[3] & 0xC0) == 0x80;
  if (scrambled) return false;                    // we cannot decode this

  int adaptLen = (p[3] & 0x20) ? 1 + p[4] : 0;   // adaptation field present
  int payloadOff = 4 + adaptLen;
  if (payloadOff >= 188) return false;
  const uint8_t* payload = p + payloadOff;
  int payloadLen = 188 - payloadOff;

  // ---- PSI: find the audio PID via PAT -> PMT ----------------------------
  // HLS encoders use arbitrary audio PIDs (0x100..0x1FFF and up); guessing is
  // hopeless. PAT (pid 0) points at the PMT, the PMT lists the stream types.
  if (pid == 0x0000)                              // PAT
  {
    if (!payloadStart || payloadLen < 8) return true;
    // pointer_field, then table: .. section_length covers programs
    int off = 1 + payload[0];                     // skip pointer_field
    if (off + 8 > payloadLen) return true;
    const uint8_t* t = payload + off;
    if (t[0] != 0x00) return true;                // table_id
    int secLen = (int)(((t[1] & 0x0F) << 8) | t[2]);
    for (int i = 8; i + 4 <= secLen + 3 - 4 && off + i + 4 <= payloadLen; i += 4)
    {
      uint16_t prog = (uint16_t)((t[i] << 8) | t[i + 1]);
      uint16_t pmt = (uint16_t)(((t[i + 2] & 0x1F) << 8) | t[i + 3]);
      if (prog != 0 && pmt != 0) { _pmtPid = pmt; break; }  // skip NIT (prog 0)
    }
    return true;
  }

  if (_pmtPid && pid == _pmtPid)                  // PMT
  {
    if (!payloadStart || payloadLen < 14) return true;
    int off = 1 + payload[0];
    if (off + 13 > payloadLen) return true;
    const uint8_t* t = payload + off;
    if (t[0] != 0x02) return true;
    int secLen = (int)(((t[1] & 0x0F) << 8) | t[2]);
    int infoLen = (int)(((t[10] & 0x0F) << 8) | t[11]);   // program_info_length
    int i    = 12 + infoLen;                      // first ES entry (t-relative)
    int tEnd = 3 + secLen - 4;                    // section end minus CRC32
    while (i + 5 <= tEnd && off + i + 5 <= payloadLen)
    {
      uint8_t  type = t[i];
      uint16_t epid = (uint16_t)(((t[i + 1] & 0x1F) << 8) | t[i + 2]);
      int elen = (int)(((t[i + 3] & 0x0F) << 8) | t[i + 4]);
      // 0x03/0x04 = MP3, 0x0F = AAC ADTS, 0x11 = AAC LATM.
      if ((type == 0x03 || type == 0x04 || type == 0x0F || type == 0x11) && epid)
      {
        _audioPid = epid;
        _audioType = type;
        break;
      }
      i += 5 + elen;
    }
    return true;
  }

  if (pid != _audioPid) return true;              // video/other streams

  if (payloadStart)
  {
    // Start of a PES packet: 00 00 01 <stream_id> <len:2> <flags:2> <hdr_len>
    if (payloadLen < 9) return true;
    if (payload[0] != 0x00 || payload[1] != 0x00 || payload[2] != 0x01) return true;
    uint8_t sid = payload[3];
    if (sid < 0xC0 || sid > 0xDF) return true;    // audio stream ids only
    uint8_t headerLen = payload[8];
    int skip = 9 + headerLen;
    if (skip > payloadLen) skip = payloadLen;

    _pesStarted = true;
    _pesHave = 0;
    emit(payload + skip, (uint32_t)(payloadLen - skip));
    return true;
  }

  // Continuation of a PES packet.
  if (!_pesStarted) return true;
  emit(payload, (uint32_t)payloadLen);
  return true;
}

/// Copy audio bytes into the output ring, sniffing the codec on the way.
void HlsSource::emit(const uint8_t* p, uint32_t n)
{
  if (!_outBuf || !n) return;

  // Codec sniff: ADTS AAC starts with 0xFFF1/0xFFF9, MP3 with 0xFFEx/0xFFFx.
  if (!_codecKnown && _outHead == _outTail)
  {
    for (uint32_t i = 0; i + 1 < n; ++i)
    {
      if (p[i] == 0xFF && (p[i + 1] & 0xF0) == 0xF0)
      {
        // ADTS has 0xFFF1/0xFFF9 (layer bits 00); MP3 has 0xFFEx (layer != 00).
        uint8_t layer = (uint8_t)((p[i + 1] >> 1) & 0x03);
        _codec = (layer == 0) ? 1 : 2;
        _codecKnown = true;
        break;
      }
    }
  }

  uint32_t space = _outCap - (_outHead - _outTail);
  if (n > space)
  {
    // Decoder is behind: keep the newest data, drop the oldest.
    uint32_t drop = n - space;
    if (drop >= (_outHead - _outTail))
    {
      _outTail = _outHead;
      drop = n - space;
    }
    else
    {
      _outTail += drop;
    }
    space = _outCap - (_outHead - _outTail);
  }
  uint32_t copy = n < space ? n : space;
  uint32_t idx = _outHead % _outCap;
  uint32_t first = (_outCap - idx) < copy ? (_outCap - idx) : copy;
  memcpy(_outBuf + idx, p, first);
  if (copy > first) memcpy(_outBuf, p + first, copy - first);
  _outHead += copy;
  _outTotal += copy;
}

// ---------------------------------------------------------------------------
// Opening / reading
// ---------------------------------------------------------------------------

bool HlsSource::open(const char* url)
{
  close();
  _url = url;
  _pmtPid = 0;
  _audioPid = 0;
  _audioType = 0;
  _codec = 0;
  _codecKnown = false;
  _syncLost = 0;

  if (!fetchPlaylist(_url)) return false;
  _segNext = 0;

  // Open the first segment so we can report the codec right away.
  if (!openSegment()) return false;

  // Sniff the codec before handing the source to the decoder: the caller
  // has to pick AAC or MP3 before anything is decoded.
  uint32_t t0 = millis();
  while (!_codecKnown && millis() - t0 < 3000)
  {
    demuxSegment();
    if (_outHead > _outTail) break;
    if (!_client->connected() && _segLeft == 0) break;
    delay(5);
  }
  // PMT stream type is a reliable fallback when the byte sniff timed out.
  if (!_codecKnown)
  {
    if (_audioType == 0x0F || _audioType == 0x11) _codec = 1;
    else if (_audioType == 0x03 || _audioType == 0x04) _codec = 2;
    _codecKnown = _codec != 0;
  }

  _open = true;
  _pos = 0;
  return true;
}

bool HlsSource::openSegment()
{
  if (_segNext >= _segCount)
  {
    if (!loadSegments()) return false;
  }
  if (_segNext >= _segCount) return false;

  String segUrl = _segUrls[_segNext];
  _segNext = (_segNext + 1) % _segCount;
  _segments++;

  String host, path;
  uint16_t port;
  if (!parseUrl(segUrl, host, path, port)) return false;

  NetworkClient* c = (port == 443) ? (NetworkClient*)&_sec : (NetworkClient*)&_plain;
  _client = c;
  c->stop();
  if (c == &_sec) _sec.setInsecure();
  if (!c->connect(host.c_str(), port, 8000)) return false;
  c->setNoDelay(true);

  c->print(String("GET ") + path + " HTTP/1.0\r\n");
  c->print(String("Host: ") + host + (port == 80 || port == 443 ? String("") : String(":") + port) + "\r\n");
  c->print("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/126.0.0.0 Safari/537.36\r\n");
  c->print("Accept: */*\r\n");
  c->print("Connection: close\r\n\r\n");

  String line;
  if (!readLine(*c, line, 5000)) { c->stop(); return false; }
  int sp = line.indexOf(' ');
  if (sp < 0) { c->stop(); return false; }
  int status = line.substring(sp + 1, line.indexOf(' ', sp + 1)).toInt();
  _status = status;
  if (status != 200) { c->stop(); return false; }

  // Skip headers, note the content length if there is one.
  uint32_t contentLength = 0;
  for (;;)
  {
    if (!readLine(*c, line, 5000)) { c->stop(); return false; }
    if (line.length() == 0) break;
    int cl = line.indexOf(':');
    if (cl > 0)
    {
      String k = line.substring(0, cl);
      k.toLowerCase();
      if (k == "content-length")
      {
        String v = line.substring(cl + 1);
        v.trim();
        contentLength = (uint32_t)v.toInt();
      }
    }
  }

  _segOpen = true;
  _segLeft = contentLength ? contentLength : 0xFFFFFFFF;   // unknown -> read until close
  _tsHave = 0;
  _pesStarted = false;
  return true;
}

uint32_t HlsSource::read(void* dst, uint32_t len)
{
  return readNonBlock(dst, len);
}

uint32_t HlsSource::readNonBlock(void* dst, uint32_t len)
{
  if (!_open || !len) return 0;
  uint8_t* out = (uint8_t*)dst;
  uint32_t got = 0;
  uint32_t lastProgress = millis();

  while (got < len)
  {
    // Hand over whatever is already buffered.
    uint32_t have = _outHead - _outTail;
    if (have)
    {
      uint32_t want = len - got;
      if (want > have) want = have;
      uint32_t idx = _outTail % _outCap;
      uint32_t first = (_outCap - idx) < want ? (_outCap - idx) : want;
      memcpy(out + got, _outBuf + idx, first);
      if (want > first) memcpy(out + got + first, _outBuf, want - first);
      _outTail += want;
      got += want;
      _pos += want;
      lastProgress = millis();
      if (got >= len) break;
      continue;
    }

    // Stall watchdog: five seconds with no audio at all means the network
    // went away mid-segment. Bail out so the player reconnects cleanly
    // instead of spinning here forever.
    if (millis() - lastProgress > 5000) { _open = false; break; }

    // Need more audio: pull another segment's worth of TS through the demuxer.
    if (!_segOpen)
    {
      if (!openSegment()) { _open = false; break; }
      lastProgress = millis();
    }

    demuxSegment();

    if (_segLeft == 0 || !_client->connected())
    {
      _client->stop();
      _segOpen = false;
      if (!openSegment()) { _open = false; break; }
      continue;
    }

    if (_outHead == _outTail)
    {
      // Nothing came out of this segment yet.
      if (!_client->connected() && _segLeft == 0)
      {
        if (!openSegment()) { _open = false; break; }
        continue;
      }
      // Give the network a moment rather than spinning.
      delay(1);
    }
  }
  return got;
}

bool HlsSource::close()
{
  _plain.stop();
  _sec.stop();
  _client = &_plain;
  _segOpen = false;
  _open = false;
  _outHead = _outTail = 0;
  _tsHave = 0;
  _pesStarted = false;
  _segCount = 0;
  _segNext = 0;
  _status = 0;
  return true;
}
