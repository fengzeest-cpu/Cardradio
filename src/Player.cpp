#include "Player.h"
#include "Library.h"
#include "RadioSource.h"
#include "HlsSource.h"
#include <M5Unified.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorAAC.h>
#include <esp_random.h>
#include <esp_heap_caps.h>

Player player;

Player::Player() : _out(&M5.Speaker, 0) {}
Player::~Player()
{
  if (_madSpace) { heap_caps_free(_madSpace); _madSpace = nullptr; }
}

bool Player::begin(fs::FS* fs)
{
  _fs = fs;
  _src = new AudioFileSourceFS(*fs);
  _q = xQueueCreate(16, sizeof(Cmd));
  _lock = xSemaphoreCreateMutex();
  _out.setup();

  // Decoder working memory, allocated once. PSRAM first — it is 29 KB and
  // internal RAM on this board is scarce. MALLOC_CAP_8BIT is required: without
  // it the caps are not a valid PSRAM request and the allocation lands in
  // internal RAM (or fails).
  const int need = AudioGeneratorMP3::preAllocSize();
  _madSpace = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!_madSpace) _madSpace = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_8BIT);
  if (_madSpace) _mp3.useSpace(_madSpace, need);
  else            _mp3.useSpace(nullptr, 0);      // let the library allocate

  // Same core and priority as M5Unified's MP3 example: the UI loop (prio 1)
  // runs whenever the decoder is waiting on the speaker, which is most of the time.
  // The stack is 8 KB: libmad decodes iteratively and needs very little of it,
  // and this board has no PSRAM to fall back on.
  return _src && _q && _lock &&
         xTaskCreatePinnedToCore(taskEntry, "mp3", 8192, this, 2, nullptr, 1) == pdPASS;
}

// ---------------------------------------------------------------------------
// UI side
// ---------------------------------------------------------------------------
void Player::send(const Cmd& c)
{
  if (xQueueSend(_q, &c, 0) != pdTRUE)
  {
    if (c.type == C_LIST) delete c.list;
  }
}

void Player::playList(std::vector<String>&& paths, int startIndex, float startSec)
{
  send({ C_LIST, startIndex, 0, startSec, new std::vector<String>(std::move(paths)) });
}

void Player::playRadio(int group, int flatIndex)
{
  // Nothing is allocated here: the station table is static data, so starting a
  // station costs no heap at all.
  send({ C_RADIO, flatIndex, group, 0, nullptr });
}

void Player::togglePause()    { send({ C_TOGGLE, 0, 0, 0, nullptr }); }
void Player::next()           { send({ C_NEXT, 0, 0, 0, nullptr }); }
void Player::prev()           { send({ C_PREV, 0, 0, 0, nullptr }); }
void Player::seekBy(float s)  { send({ C_SEEK, 0, 0, s, nullptr }); }
void Player::stop()           { send({ C_STOP, 0, 0, 0, nullptr }); }
void Player::setShuffle(bool on) { _shuffle = on; send({ C_SHUFFLE, 0, 0, 0, nullptr }); }
void Player::setRepeat(RepeatMode m) { _repeat = (uint8_t)m; }

uint32_t Player::radioRingBytes() const { return _rsrc ? _rsrc->ringBytes() : 0; }
bool     Player::radioRingInPsram() const { return _rsrc ? _rsrc->ringInPsram() : false; }
uint32_t Player::madBytes() const { return _madSpace ? AudioGeneratorMP3::preAllocSize() : 0; }
bool     Player::madInPsram() const
{
  return _madSpace && esp_ptr_external_ram(_madSpace);
}

void Player::requestSuspend()   { _suspendReq = true; }
bool Player::suspended() const  { return _suspended.load(); }
void Player::resumeAfterSD()    { send({ C_RESUME, 0, 0, 0, nullptr }); }

void Player::getInfo(TrackInfo& out)
{
  xSemaphoreTake(_lock, portMAX_DELAY);
  out = _shared;
  xSemaphoreGive(_lock);
}

// ---------------------------------------------------------------------------
// Audio task
// ---------------------------------------------------------------------------
void Player::run()
{
  for (;;)
  {
    // UI asked us to park the decoder so the SD card sits idle during a
    // directory sweep. Closing the file here (on this task) is the only safe
    // place; the UI just waits for the handshake.
    if (_suspendReq.load())
    {
      if (_decoding && !_radioActive && _list && !_order.empty())
      {
        _suspElapsed = currentElapsed();
        _suspPos     = _orderPos;
        _suspKeep    = state();
        closeDecoder();
      }
      _suspendReq = false;
      _suspended  = true;
    }

    Cmd c;
    TickType_t wait = (_decoding && state() == PlayState::Playing) ? 0 : pdMS_TO_TICKS(20);
    while (xQueueReceive(_q, &c, wait) == pdTRUE)
    {
      handle(c);
      wait = 0;
    }

    if (_decoding && state() == PlayState::Playing)
    {
      // One call fills about one 512-frame buffer, then waits on the speaker.
      if (!(_gen && _gen->loop()))
      {
        if (_radioActive) radioDropped();
        else { closeDecoder(); trackEnded(); }
      }
    }
    else if (_radioActive && state() == PlayState::Playing && _radioRetryAt &&
             (int32_t)(millis() - _radioRetryAt) >= 0)
    {
      _radioRetryAt = 0;
      startRadio(_radioFlat);
    }

    publishElapsed();
  }
}

void Player::handle(const Cmd& c)
{
  switch (c.type)
  {
    case C_LIST:
    {
      closeDecoder();
      _radioActive = false;
      _radioRetryAt = 0;
      _list.reset(c.list);
      _suspKeep = PlayState::Stopped;      // a fresh playlist supersedes the parked one
      int start = c.i;
      if (start < 0 || start >= (int)_list->size()) start = 0;
      buildOrder(start);
      _atEnd = false;
      // A start offset means "carry on where you left off": open that spot
      // directly instead of the top of the file.
      if (c.f > 0.5f && openAt(_orderPos, c.f, false))
      {
        _state = (uint8_t)PlayState::Playing;
        _loaded = true;
      }
      else startFrom(_orderPos, +1);
      break;
    }

    case C_RADIO:
    {
      closeDecoder();
      _list.reset();
      _order.clear();
      _orderPos = 0;
      _atEnd = false;
      _radioActive = true;
      _radioFails = 0;
      _radioRetryAt = 0;
      _radioGroup = c.j;                   // group the listener browses (-1 = all)
      int start = c.i;
      if (start < 0 || start >= stations::total())
      {
        _radioActive = false;
        _state = (uint8_t)PlayState::Stopped;
        _loaded = false;
        publishInfo();
        break;
      }
      startRadio(start);
      break;
    }

    case C_TOGGLE:
      if (state() == PlayState::Playing)      _state = (uint8_t)PlayState::Paused;
      else if (state() == PlayState::Paused)  _state = (uint8_t)PlayState::Playing;
      else if (_radioActive)
      {
        _radioFails = 0;
        startRadio(_radioFlat);
      }
      else if (_list && !_list->empty())
      {
        if (_atEnd) { _orderPos = 0; _atEnd = false; }
        startFrom(_orderPos, +1);
      }
      break;

    case C_NEXT:
      if (_radioActive) { _radioFails = 0; stepRadio(+1); }
      else step(+1);
      break;

    case C_PREV:
      if (_radioActive) { _radioFails = 0; stepRadio(-1); }
      else if (_decoding && currentElapsed() > 3.0f) startFrom(_orderPos, +1);   // restart this one
      else step(-1);
      break;

    case C_SEEK:
    {
      if (_radioActive) break;                 // live streams can't be seeked
      if (!_decoding) break;
      // Held seek keys queue several of these: take them as one jump.
      float delta = c.f;
      Cmd more;
      while (xQueuePeek(_q, &more, 0) == pdTRUE && more.type == C_SEEK)
      {
        xQueueReceive(_q, &more, 0);
        delta += more.f;
      }
      float t = currentElapsed() + delta;
      if (t < 0) t = 0;
      if (_info.duration > 0 && t > _info.duration - 1.0f)
      {
        // Seeking past the end: same as finishing the track.
        closeDecoder();
        trackEnded();
        break;
      }
      PlayState keep = state();
      if (openAt(_orderPos, t, true)) _state = (uint8_t)keep;
      else { _state = (uint8_t)PlayState::Stopped; publishInfo(); }
      break;
    }

    case C_STOP:
      closeDecoder();
      _radioActive = false;               // stop means stop: no reconnect, no radio leftovers
      _radioRetryAt = 0;
      _radioFails   = 0;
      _suspKeep = PlayState::Stopped;     // never resurrect the parked track after a stop
      _state = (uint8_t)PlayState::Stopped;
      _timeBase = 0;
      publishInfo();
      break;

    case C_SHUFFLE:
      if (_list && !_list->empty())
      {
        int cur = _order.empty() ? 0 : _order[_orderPos];
        buildOrder(cur);
        publishInfo();
      }
      break;

    case C_REPEAT:
      break;

    case C_RESUME:
      // A new track / station / stop command between park and resume wins:
      // it clears _suspKeep, and the !decoding guard skips this reopen.
      if (_suspKeep != PlayState::Stopped && !_decoding)
      {
        if (openAt(_suspPos, _suspElapsed, true)) _state = (uint8_t)_suspKeep;
        else { _state = (uint8_t)PlayState::Stopped; _loaded = false; publishInfo(); }
      }
      _suspended = false;
      _suspKeep  = PlayState::Stopped;
      break;
  }
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------
bool Player::startRadio(int flat)
{
  // The station table is static data: look the entry up, never copy it.
  if (flat < 0 || flat >= stations::total()) return false;
  int g = -1;
  const Station* st = stations::at(flat, &g);
  if (!st) return false;

  closeDecoder();
  _radioFlat = flat;

  // An .m3u8 URL is HLS: the audio comes out of MPEG-TS segments, so it needs
  // the HLS source. Everything else is a plain HTTP audio stream.
  const bool hls = strstr(st->url, ".m3u8") != nullptr;

  if (hls)
  {
    if (!_hls) _hls.reset(new HlsSource());
    if (!_rsrc) _rsrc.reset(new RadioSource());
  }
  else
  {
    if (!_rsrc) _rsrc.reset(new RadioSource());
  }

  bool ok = hls ? _hls->open(st->url) : _rsrc->open(st->url);
  if (ok)
  {
    _out.eq().reset();
    _frameBase = _out.frames();
    _timeBase = 0;

    // For HLS the codec comes from the stream itself (ADTS AAC or MP3);
    // for a plain stream it is what the station table says.
    uint8_t codec = hls ? _hls->codec() : st->codec;
    if (codec == 1)
    {
      if (!_aac) _aac.reset(new AudioGeneratorAAC());
      _gen = _aac.get();
    }
    else _gen = &_mp3;

    ok = _gen->begin(hls ? (AudioFileSource*)_hls.get() : (AudioFileSource*)_rsrc.get(), &_out);
  }

  _loaded = true;
  if (!ok)
  {
    closeDecoder();
    if (++_radioFails <= 4)
    {
      _radioRetryAt = millis() + 1500;         // keep playing state, show "重连中"
      _state = (uint8_t)PlayState::Playing;
    }
    else
    {
      _state = (uint8_t)PlayState::Stopped;
      _radioRetryAt = 0;
    }
    publishInfo();
    return false;
  }

  _radioFails = 0;
  _radioRetryAt = 0;
  _decoding = true;
  _state = (uint8_t)PlayState::Playing;
  publishInfo();
  publishElapsed();
  return true;
}

void Player::radioDropped()
{
  closeDecoder();
  if (++_radioFails <= 4)
  {
    _radioRetryAt = millis() + 1500;
    _state = (uint8_t)PlayState::Playing;
  }
  else
  {
    _radioRetryAt = 0;
    _state = (uint8_t)PlayState::Stopped;
  }
  publishInfo();
}

/// Next / previous station inside the group the listener is browsing.
/// group < 0 cycles through the whole table.
void Player::stepRadio(int dir)
{
  int start = stations::groupStart(_radioGroup);
  int count = stations::groupCount(_radioGroup);
  if (count <= 0) return;

  int rel = _radioFlat - start;
  if (rel < 0 || rel >= count) rel = 0;
  rel = (rel + dir + count) % count;
  startRadio(start + rel);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
void Player::buildOrder(int current)
{
  int n = _list ? (int)_list->size() : 0;
  _order.resize(n);
  for (int i = 0; i < n; ++i) _order[i] = i;
  if (_shuffle && n > 1)
  {
    for (int i = n - 1; i > 0; --i) std::swap(_order[i], _order[esp_random() % (i + 1)]);
    for (int i = 0; i < n; ++i) if (_order[i] == current) { std::swap(_order[0], _order[i]); break; }
    _orderPos = 0;
  }
  else
  {
    _orderPos = (current >= 0 && current < n) ? current : 0;
  }
}

void Player::closeDecoder()
{
  if (_decoding)
  {
    if (_gen) _gen->stop();       // also drains the output and closes the source
    _decoding = false;
  }
  _gen = nullptr;
  if (_rsrc) _rsrc->close();          // idempotent: also drops a stale socket
  if (_hls) _hls->close();
  if (_src && _src->isOpen()) _src->close();
}

bool Player::openAt(int orderPos, float startSec, bool reuseInfo)
{
  if (!_list || orderPos < 0 || orderPos >= (int)_order.size()) return false;
  closeDecoder();
  _radioActive = false;
  _orderPos = orderPos;
  const String& path = (*_list)[_order[_orderPos]];

  if (!reuseInfo)
  {
    bool okParse = false;
    File f = _fs->open(path);
    if (f) { okParse = _info.parse(f); f.close(); }
    if (!okParse) return false;
  }
  if (!_src->open(path.c_str())) return false;

  uint32_t off = _info.offsetFor(startSec);
  if (!_src->seek(off, SEEK_SET)) { _src->close(); return false; }

  _out.eq().reset();
  _frameBase = _out.frames();
  _timeBase = startSec > 0 ? startSec : 0;

  _gen = &_mp3;
  if (!_mp3.begin(_src, &_out))
  {
    if (_src->isOpen()) _src->close();
    return false;
  }
  _decoding = true;
  if (!reuseInfo) publishInfo();
  publishElapsed();
  return true;
}

bool Player::startFrom(int orderPos, int dir)
{
  int n = _order.size();
  for (int tries = 0; tries < n; ++tries)
  {
    int pos = ((orderPos + dir * tries) % n + n) % n;
    if (openAt(pos, 0, false))
    {
      _state = (uint8_t)PlayState::Playing;
      _loaded = true;
      return true;
    }
  }
  // Nothing in the list would open.
  _state = (uint8_t)PlayState::Stopped;
  _loaded = false;
  publishInfo();
  return false;
}

void Player::step(int dir)
{
  int n = _order.size();
  if (!n) return;
  _atEnd = false;
  startFrom(((_orderPos + dir) % n + n) % n, dir);
}

void Player::trackEnded()
{
  int n = _order.size();
  if (!n) return;
  RepeatMode rep = repeat();

  if (rep == RepeatMode::One) { startFrom(_orderPos, +1); return; }

  if (_orderPos + 1 < n) { startFrom(_orderPos + 1, +1); return; }

  if (rep == RepeatMode::All)
  {
    if (_shuffle && n > 1)
    {
      // New random order for the next lap, without repeating the last song first.
      int last = _order[_orderPos];
      buildOrder(-1);
      if (_order[0] == last) std::swap(_order[0], _order[1]);
    }
    startFrom(0, +1);
    return;
  }

  // End of the list.
  _state = (uint8_t)PlayState::Stopped;
  _atEnd = true;
  _timeBase = 0;
  publishInfo();
}

float Player::currentElapsed() const
{
  uint32_t rate = _out.rate();
  float t = _timeBase;
  if (_decoding && rate) t += (float)(_out.frames() - _frameBase) / rate;
  return t;
}

void Player::publishElapsed()
{
  float t = (_decoding || state() != PlayState::Stopped) ? currentElapsed() : 0;
  _elapsedMs = (uint32_t)(t * 1000.0f);
}

void Player::publishInfo()
{
  TrackInfo t;
  int g = -1;
  const Station* st = (_radioFlat >= 0 && _radioFlat < stations::total())
                    ? stations::at(_radioFlat, &g) : nullptr;
  if (_radioActive && st)
  {
    t.radio      = true;
    t.codec      = st->codec;
    t.path       = st->url;
    t.title      = st->name;
    t.artist     = (g >= 0 && g < stations::kGroupCount) ? stations::kGroups[g].name : "";
    t.bitrate    = st->kbps;
    t.sampleRate = _out.rate();
    t.channels   = 2;
    t.duration   = 0;
    t.position   = _radioFlat - stations::groupStart(_radioGroup) + 1;
    t.count      = stations::groupCount(_radioGroup);
  }
  else if (_list && !_order.empty() && _orderPos < (int)_order.size())
  {
    t.path = (*_list)[_order[_orderPos]];
    t.title = _info.title;
    if (t.title.isEmpty())
    {
      String b = Library::baseName(t.path);
      if (Library::isMp3(b)) b.remove(b.length() - 4);
      t.title = foldUtf8(b);
    }
    t.artist     = _info.artist;
    t.album      = _info.album;
    t.duration   = _info.duration;
    t.bitrate    = _info.bitrate;
    t.sampleRate = _info.sampleRate;
    t.channels   = _info.channels;
    t.vbr        = _info.vbr;
    t.position   = _orderPos + 1;
    t.count      = _order.size();
  }
  xSemaphoreTake(_lock, portMAX_DELAY);
  _shared = t;
  xSemaphoreGive(_lock);
  _infoVersion++;
}
