// HlsSource.h — plays an HLS (HTTP Live Streaming) radio stream.
//
// Why: the Chinese stations that matter (CNR, CRI, the provincial ones) only
// publish HLS; their Icecast/Shoutcast endpoints are either dead or AES
// encrypted. HLS is a playlist of MPEG-TS segments, so playing it means
// fetching a .m3u8, then pulling the segments it names and digging the audio
// out of the transport stream.
//
// How: the segments are demuxed *while they arrive* — a TS packet is 188
// bytes, and a PES packet is reassembled across them. Nothing bigger than a
// few kilobytes is ever held in memory, which matters because this board has
// no PSRAM: the whole thing has to live in the internal heap.
//
// The audio that comes out is raw AAC (ADTS) or MP3 frames, which is exactly
// what the existing decoders want — no extra layer in between.
#pragma once

#include <Arduino.h>
#include <AudioFileSource.h>

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #include <NetworkClient.h>
  #include <NetworkClientSecure.h>
  using HlsPlain = NetworkClient;
  using HlsSecure = NetworkClientSecure;
#else
  #include <WiFiClient.h>
  #include <WiFiClientSecure.h>
  using HlsPlain = WiFiClient;
  using HlsSecure = WiFiClientSecure;
#endif

class HlsSource : public AudioFileSource
{
public:
  HlsSource();
  ~HlsSource() override;

  bool open(const char* url) override;
  uint32_t read(void* dst, uint32_t len) override;
  uint32_t readNonBlock(void* dst, uint32_t len) override;
  bool close() override;
  bool isOpen() override { return _open; }
  uint32_t getSize() override { return 0; }
  uint32_t getPos() override { return _pos; }
  bool loop() override { return _open; }

  int      statusCode() const { return _status; }
  uint32_t received() const { return _pos; }
  /// 1 = AAC, 2 = MP3 — the caller needs this to pick a decoder.
  uint8_t  codec() const { return _codec; }
  /// How many TS segments have been played so far (diagnostics).
  uint16_t segments() const { return _segments; }

private:
  enum Wait : uint8_t { W_DATA = 0, W_TIMEOUT = 1, W_CLOSED = 2 };

  // -- playlist ------------------------------------------------------------
  bool     parseUrl(const String& url, String& host, String& path, uint16_t& port);
  bool     fetchPlaylist(const String& url);
  bool     pickVariant();
  bool     loadSegments();
  String   absoluteUrl(const String& base, const String& rel);

  // -- transport stream ----------------------------------------------------
  bool     openSegment();
  void     demuxSegment();
  bool     handleTsPacket(const uint8_t* p);
  void     emit(const uint8_t* p, uint32_t n);

  // -- helpers -------------------------------------------------------------
  bool     httpGet(const String& url, int& status);
  bool     readLine(NetworkClient& c, String& out, uint32_t timeoutMs);
  Wait     waitData(uint32_t timeoutMs);
  bool     readAll(NetworkClient& c, String& out, uint32_t maxLen, uint32_t timeoutMs);

  String   _url, _host, _path;
  uint16_t _port = 80;
  bool     _open = false;
  int      _status = 0;
  volatile uint32_t _pos = 0;

  // Playlist state
  String   _variantUrl;        // the media playlist we are actually playing
  String   _segUrls[8];        // rolling window of upcoming segments
  uint8_t  _segCount = 0;
  uint8_t  _segNext = 0;       // index of the next segment to fetch
  uint16_t _segments = 0;

  // Current segment
  HlsPlain   _plain;            // HTTP client (most segments)
  HlsSecure  _sec;              // TLS client, selected per URL
  NetworkClient* _client = &_plain;   // points at whichever is in use
  bool     _segOpen = false;
  uint32_t _segLeft = 0;       // bytes left in the current segment
  uint8_t  _tsBuf[188];
  uint32_t _tsHave = 0;

  // TS/PES reassembly
  uint32_t _pesHave = 0;
  bool     _pesStarted = false;

  // PSI: PAT -> PMT -> audio PID discovery
  uint16_t _pmtPid   = 0;
  uint16_t _audioPid = 0;
  uint8_t  _audioType = 0;      // PMT stream type: 0x03/0x04 MP3, 0x0F AAC ADTS, 0x11 LATM
  uint32_t _syncLost = 0;       // TS sync-byte mismatches (diagnostics)

  // Detected codec
  uint8_t  _codec = 0;         // 1 = AAC (ADTS), 2 = MP3
  bool     _codecKnown = false;

  // Output buffering: the decoder reads from here.
  uint8_t* _outBuf = nullptr;
  uint32_t _outCap = 0;
  uint32_t _outHead = 0, _outTail = 0;
  uint32_t _outTotal = 0;
};
