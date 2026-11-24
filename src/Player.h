// Player.h — decoding runs in its own task; the UI talks to it through a
// command queue and reads state back through atomics and a locked snapshot.
//
// Two sources feed the same decoders: files on the SD card, and live radio
// streams pulled off the network.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <atomic>
#include <memory>
#include <vector>

#include <AudioFileSourceFS.h>
#include <AudioGeneratorMP3.h>

#include "AudioOutputEQ.h"
#include "Mp3Info.h"
#include "Stations.h"

class RadioSource;
class AudioGeneratorAAC;
class RadioSource;
class HlsSource;
class AudioGenerator;

enum class RepeatMode : uint8_t { Off = 0, All = 1, One = 2 };
enum class PlayState  : uint8_t { Stopped = 0, Playing = 1, Paused = 2 };

/// The MP3 decoder wants about 29 KB of working memory. Handing it a block
/// that is allocated once (and lives in PSRAM when there is any) means
/// switching stations never has to find 29 KB of contiguous heap — which on
/// this board it usually cannot, and which is what tore the heap apart.
class Mp3Generator : public AudioGeneratorMP3
{
public:
  void useSpace(void* space, int size)
  {
    preallocateSpace = space;
    preallocateSize  = size;
  }
};

struct TrackInfo
{
  String   path, title, artist, album;
  bool     radio      = false;
  uint8_t  codec      = 0;   // radio: 0 MP3, 1 AAC
  float    duration   = 0;
  uint16_t bitrate    = 0;
  uint32_t sampleRate = 0;
  uint8_t  channels   = 0;
  bool     vbr        = false;
  int      position   = 0;   // 1-based, in play order
  int      count      = 0;
};

// RadioStationInfo used to carry a copy of every station. It is gone now: the
// station table in Stations.h is static data, so the player only ever keeps a
// group number and a flat index — and the path that starts a stream allocates
// nothing. That matters: picking a station from the list used to build two
// 170-element vectors (~5.4 KB each) on a heap already fragmented by the
// running stream, and a failing allocation there abort()s the app.
class Player
{
public:
  Player();
  ~Player();
  bool begin(fs::FS* fs);
  AudioOutputEQSpeaker& output() { return _out; }
  /// Where the radio stream buffer lives (status page).
  uint32_t radioRingBytes() const;
  bool     radioRingInPsram() const;
  /// Where the MP3 decoder's working memory lives (status page).
  uint32_t madBytes() const;
  bool     madInPsram() const;

  // ---- UI side: all of these just queue a command ----
  void playList(std::vector<String>&& paths, int startIndex, float startSec = 0);
  /// Start a station. group < 0 means "cycle through all stations".
  void playRadio(int group, int flatIndex);
  void togglePause();
  void next();
  void prev();
  void seekBy(float seconds);
  void stop();
  void setShuffle(bool on);
  void setRepeat(RepeatMode m);

  // ---- UI side: state ----
  PlayState  state()   const { return (PlayState)_state.load(); }
  float      elapsed() const { return _elapsedMs.load() / 1000.0f; }
  bool       shuffle() const { return _shuffle.load(); }
  RepeatMode repeat()  const { return (RepeatMode)_repeat.load(); }
  bool       loaded()  const { return _loaded.load(); }
  bool       radioMode() const { return _radioActive.load(); }
  uint32_t   infoVersion() const { return _infoVersion.load(); }
  void       getInfo(TrackInfo& out);

  // ---- SD enumeration handshake ----
  // Even with every card access mutex-serialised, directory enumeration keeps
  // failing while the decoder holds an open file. Parking the decoder (file
  // closed, card idle) for the sweep — like the always-working "radio stopped"
  // case — and seamlessly resuming it afterwards is the reliable fix.
  void requestSuspend();             // UI: ask the audio task to close the file
  bool suspended() const;            // true once the audio task has parked it
  void resumeAfterSD();              // UI: queue resume at the old position

private:
  enum CmdType : uint8_t { C_LIST, C_RADIO, C_TOGGLE, C_NEXT, C_PREV, C_SEEK, C_STOP, C_SHUFFLE, C_REPEAT, C_RESUME };
  struct Cmd
  {
    CmdType type;
    int i;                                  // index (track / station)
    int j;                                  // group (radio only)
    float f;
    std::vector<String>* list;
  };

  static void taskEntry(void* arg) { ((Player*)arg)->run(); }
  void run();
  void handle(const Cmd& c);
  void send(const Cmd& c);

  bool openAt(int orderPos, float startSec, bool reuseInfo);
  bool startFrom(int orderPos, int dir);   // tries following tracks if one won't open
  bool startRadio(int flat);               // opens one stream, schedules a retry on failure
  void stepRadio(int dir);                 // next/previous station inside the group
  void radioDropped();
  void closeDecoder();
  void trackEnded();
  void step(int dir);
  void buildOrder(int current);
  void publishInfo();
  void publishElapsed();
  float currentElapsed() const;

  // audio-task-only state
  fs::FS* _fs = nullptr;
  AudioFileSourceFS* _src = nullptr;
  Mp3Generator _mp3;
  uint8_t* _madSpace = nullptr;        // decoder working memory (PSRAM if possible)
  AudioGenerator* _gen = nullptr;                 // &_mp3, or the AAC decoder
  std::unique_ptr<RadioSource> _rsrc;
  std::unique_ptr<HlsSource> _hls;
  std::unique_ptr<AudioGeneratorAAC> _aac;
  AudioOutputEQSpeaker _out;
  Mp3Info _info;
  std::unique_ptr<std::vector<String>> _list;
  std::vector<uint16_t> _order;
  int _orderPos = 0;
  bool _decoding = false;
  bool _atEnd = false;
  float _timeBase = 0;
  uint32_t _frameBase = 0;

  // radio — the station table itself is static data, so only these two numbers
  // are kept: the group the listener is browsing and the flat station index.
  int _radioGroup = -1;
  int _radioFlat  = 0;
  uint8_t _radioFails = 0;
  uint32_t _radioRetryAt = 0;

  // SD enumeration parking (all touched on the audio task only)
  std::atomic<bool> _suspendReq { false };
  std::atomic<bool> _suspended  { false };
  int       _suspPos     = 0;
  float     _suspElapsed = 0;
  PlayState _suspKeep    = PlayState::Stopped;

  // shared
  QueueHandle_t _q = nullptr;
  SemaphoreHandle_t _lock = nullptr;
  TrackInfo _shared;
  std::atomic<uint8_t>  _state { (uint8_t)PlayState::Stopped };
  std::atomic<uint32_t> _elapsedMs { 0 };
  std::atomic<bool>     _shuffle { false };
  std::atomic<uint8_t>  _repeat { (uint8_t)RepeatMode::All };
  std::atomic<bool>     _loaded { false };
  std::atomic<bool>     _radioActive { false };
  std::atomic<uint32_t> _infoVersion { 0 };
};

extern Player player;
