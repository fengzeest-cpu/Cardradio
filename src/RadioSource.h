// RadioSource.h — an AudioFileSource that reads an internet radio stream.
//
// It speaks just enough HTTP to get the audio out of a Shoutcast/Icecast
// server: a HTTP/1.0 GET (servers then answer with "Connection: close" and
// no chunking), optional de-chunking for the servers that ignore that, and
// ICY metadata stripping so the decoder only ever sees audio bytes.
//
// A background FreeRTOS task ("filler") keeps a ring buffer topped up from
// the socket; read() hands the decoder bytes out of that buffer. Short
// network hiccups are therefore inaudible — at 128 kbps the 48 KB ring is
// about 3 seconds of margin.
#pragma once

#include <Arduino.h>
#include <AudioFileSource.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #include <NetworkClient.h>
  using RadioClient = NetworkClient;
#else
  #include <WiFiClient.h>
  using RadioClient = WiFiClient;
#endif

class RadioSource : public AudioFileSource
{
public:
  RadioSource();
  ~RadioSource() override;

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
  /// Where the stream buffer lives and how big it is (for the status page).
  uint32_t ringBytes() const { return _ringCap; }
  bool     ringInPsram() const { return _ringPsram; }

private:
  enum Wait : uint8_t { W_DATA = 0, W_TIMEOUT = 1, W_CLOSED = 2 };
  enum : uint32_t { kPullMax = 512, kPrefill = 32768, kRingMax = 192 * 1024 };

  bool     parseUrl(const String& url);
  bool     openSocket();
  Wait     waitData(uint32_t timeoutMs);
  bool     readLine(String& out, uint32_t timeoutMs);
  bool     readHeaders(int& status, String& location);
  bool     nextChunk();
  bool     skipMeta();
  uint32_t readInternal(void* dst, uint32_t len, bool nonBlock);

  static void fillTrampoline(void* arg);
  void     fillLoop();
  void     pushRing(const uint8_t* p, uint32_t n);
  uint32_t popRing(void* dst, uint32_t len);
  bool     startFill();
  uint32_t readBuffered(void* dst, uint32_t len, bool nonBlock);
  uint32_t readDirect(void* dst, uint32_t len, bool nonBlock);

  RadioClient _client;
  String   _url, _host, _path;
  uint16_t _port   = 80;
  bool     _open   = false;
  int      _status = 0;
  volatile uint32_t _pos = 0;

  bool     _chunked   = false;
  uint32_t _chunkLeft = 0;

  int      _metaInterval = 0;   // icy-metaint: audio bytes between metadata blocks
  int      _metaLeft     = 0;

  // ring buffer + fill task (guarded by _mux unless noted)
  SemaphoreHandle_t _mux     = nullptr;
  TaskHandle_t      _fillTask = nullptr;
  uint8_t* _ring  = nullptr;
  uint32_t _ringCap = 0;             // actual ring size (PSRAM 192K / internal 8K)
  bool     _ringPsram = false;       // true when the ring lives in PSRAM
  uint32_t _wHead = 0, _rTail = 0;   // absolute counters, wrap via modulo
  volatile bool _stop    = false;    // filler should exit (close in progress)
  volatile bool _srcDone = false;    // server EOF/fatal: no more data ever

  // ICY metadata block: one length byte plus 16-byte units. The block is only
  // skipped over, never parsed, so a small buffer is enough.
  uint8_t  _metaBuf[64];
  uint8_t  _pullBuf[kPullMax];
};
