// CardRadio v1.1 — MP3 player + internet radio for the M5Stack Cardputer ADV
//
// Plays .mp3 files from the microSD card through the speaker or headphone
// jack, with a folder browser, shuffle/repeat, seeking and a 3-band EQ.

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <bitset>
#include <vector>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include <esp_sleep.h>
#include <WiFi.h>

#include "Library.h"
#include "Player.h"
#include "Stations.h"
#include "Net.h"

// ---------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
namespace col
{
  constexpr uint16_t bg     = rgb(10, 12, 16);
  constexpr uint16_t panel  = rgb(28, 32, 42);
  constexpr uint16_t panel2 = rgb(46, 52, 66);
  constexpr uint16_t text   = rgb(236, 236, 236);
  constexpr uint16_t dim    = rgb(130, 138, 152);
  constexpr uint16_t accent = rgb(255, 145, 40);
  constexpr uint16_t cool   = rgb(80, 200, 255);
  constexpr uint16_t folder = rgb(240, 196, 80);
  constexpr uint16_t good   = rgb(120, 220, 120);
  constexpr uint16_t warn   = rgb(255, 90, 80);
}

static constexpr int W = 240, H = 135;
// Bound to M5.Display, never M5Cardputer.Display: that one is a reference that
// only gets set when M5Cardputer is constructed, and ESP-IDF runs global
// constructors in reverse link order, so this file's globals come first.
// Taking its address here gave the canvas a null parent -> crash on first draw.
static M5Canvas cv(&M5.Display);

// Did the PSRAM make it into the allocatable heap? (set in setup())
static bool psramReady = false;

enum class Screen : uint8_t
{
  Home,                         // 开机主菜单：音乐库 / 网络电台
  NowPlaying, FolderPicker,     // 播放页 / 文件夹选择器（播放哪个文件夹在这里选）
  Equalizer, Help,              // 均衡器 / 快捷键总览
  Status,                       // 运行状态（按 I）：剩余内存等
  RadioMenu, RadioStations, RadioSearch,  // 网络电台：分类页 / 台列表 / 搜索
  WifiList, WifiPass,           // 配网
  SleepTimer                    // 定时关闭（T）
};
static Screen screen = Screen::Home;
static Screen helpBack = Screen::Home;   // 帮助页从哪来、关掉回哪去
static void beginSwitch(const char* text, uint8_t target, const char* want = "");

// ---------------------------------------------------------------------------
// Settings (kept in NVS)
// ---------------------------------------------------------------------------
struct Settings
{
  int     volume     = 50;      // percent, 5 % steps
  int     eq[3]      = {0, 0, 0};
  bool    shuffle    = false;
  uint8_t repeat     = (uint8_t)RepeatMode::All;
  uint8_t brightness = 1;
  String  dir        = "/";
  String  lastPath;            // 上次播放的本地文件，用来「接着听」
  uint16_t lastPos  = 0;       // 该文件停下的秒数
};
static Settings cfg;
static Preferences prefs;
static uint32_t saveDue = 0;
static const uint8_t kBrightness[] = { 40, 100, 180, 255 };

// ---------------------------------------------------------------------------
// 收听历史：最近 20 个电台，NVS 持久化，新的在最前
// ---------------------------------------------------------------------------
static constexpr int kRecentMax = 20;
static String  histUrl[kRecentMax];
static String  histName[kRecentMax];
static int     histCount = 0;

static void markDirty();   // 定义在下面（批量写 NVS）

static void loadRecents()
{
  histCount = prefs.getUChar("hCnt", 0);
  if (histCount > kRecentMax) histCount = kRecentMax;
  for (int i = 0; i < histCount; ++i)
  {
    char k[8];
    snprintf(k, sizeof(k), "hU%d", i);
    histUrl[i] = prefs.getString(k, "");
    snprintf(k, sizeof(k), "hN%d", i);
    histName[i] = prefs.getString(k, "");
  }
  // 老版本只有一条（stUrl/stName），迁移进历史
  if (!histCount)
  {
    String u = prefs.getString("stUrl", "");
    if (u.length())
    {
      histUrl[0] = u;
      histName[0] = prefs.getString("stName", "");
      histCount = 1;
    }
  }
}

static void saveRecents()
{
  prefs.putUChar("hCnt", (uint8_t)histCount);
  for (int i = 0; i < histCount; ++i)
  {
    char k[8];
    snprintf(k, sizeof(k), "hU%d", i);
    prefs.putString(k, histUrl[i]);
    snprintf(k, sizeof(k), "hN%d", i);
    prefs.putString(k, histName[i]);
  }
}

/// 记一次收听：去重（已存在则提到最前），最多保留 20 条。
static void pushRecent(const String& url, const String& name)
{
  int at = -1;
  for (int i = 0; i < histCount; ++i) if (histUrl[i] == url) { at = i; break; }
  if (at < 0)
  {
    at = min(histCount, kRecentMax - 1);   // 满了就挤掉最旧的一条
    if (histCount < kRecentMax) ++histCount;
  }
  for (int i = at; i > 0; --i)
  {
    histUrl[i] = histUrl[i - 1];
    histName[i] = histName[i - 1];
  }
  histUrl[0] = url;
  histName[0] = name;
  markDirty();
}

static void loadSettings()
{
  prefs.begin("mp3player", false);
  cfg.volume     = prefs.getUChar("vol", 50);
  cfg.eq[0]      = prefs.getChar("eqL", 0);
  cfg.eq[1]      = prefs.getChar("eqM", 0);
  cfg.eq[2]      = prefs.getChar("eqH", 0);
  cfg.shuffle    = prefs.getBool("shuf", false);
  cfg.repeat     = prefs.getUChar("rep", (uint8_t)RepeatMode::All) % 3;
  cfg.brightness = prefs.getUChar("bri", 1) % 4;
  cfg.dir        = prefs.getString("dir", "/");
  cfg.lastPath   = prefs.getString("trkPath", "");
  cfg.lastPos    = prefs.getUShort("trkPos", 0);
  loadRecents();
  prefs.end();
  if (cfg.volume > 100) cfg.volume = 100;
  for (int& g : cfg.eq) g = constrain(g, -ThreeBandEQ::kMaxDb, ThreeBandEQ::kMaxDb);
}

static void saveSettings()
{
  prefs.begin("mp3player", false);
  prefs.putUChar("vol", cfg.volume);
  prefs.putChar("eqL", cfg.eq[0]);
  prefs.putChar("eqM", cfg.eq[1]);
  prefs.putChar("eqH", cfg.eq[2]);
  prefs.putBool("shuf", cfg.shuffle);
  prefs.putUChar("rep", cfg.repeat);
  prefs.putUChar("bri", cfg.brightness);
  prefs.putString("dir", cfg.dir);
  prefs.putString("trkPath", cfg.lastPath);
  prefs.putUShort("trkPos", cfg.lastPos);
  saveRecents();
  prefs.end();
}

static void markDirty() { saveDue = millis() + 3000; }   // batch writes, spare the flash

// ---------------------------------------------------------------------------
// Audio settings
// ---------------------------------------------------------------------------
static bool muted = false;

static void applyVolume()
{
  // M5.Speaker squares this, so even steps here already sound even.
  M5.Speaker.setVolume(muted ? 0 : (uint8_t)(cfg.volume * 255 / 100));
}

static void applyEq() { player.output().eq().setGains(cfg.eq[0], cfg.eq[1], cfg.eq[2]); }

// ---------------------------------------------------------------------------
// Screen power
// ---------------------------------------------------------------------------
static constexpr uint32_t kAutoOffMs = 60000;   // screen off after 1 min idle while playing
static bool screenOn = true;
static uint32_t lastInput = 0;

static void setScreen(bool on)
{
  screenOn = on;
  M5.Display.setBrightness(on ? kBrightness[cfg.brightness] : 0);
}

// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------
static String toastText;
static int toastBar = -1;
static uint32_t toastUntil = 0;

static void toast(const String& s, int barPct = -1, uint32_t ms = 1100)
{
  toastText = s;
  toastBar = barPct;
  toastUntil = millis() + ms;
}

// ---------------------------------------------------------------------------
// Folder picker — the ONLY place that browses the card. Row 0 is
// "play this folder", the rows below are its subfolders and MP3s. Playback
// never happens inside a browsing screen, so the decode-vs-enumerate
// conflict that used to blank the library is structurally gone: a picker
// enumeration parks the decoder, and starting playback enumerates nothing.
// ---------------------------------------------------------------------------
static bool mountSD();          // defined near setup()
static String sdFailDiag;       // shown on the picker failure screen
static bool listDir(const String& path, std::vector<LibEntry>& out, bool& trunc);
static String pickDir = "/";
static std::vector<LibEntry> pickList;   // subfolders + MP3s of pickDir
static bool pickFailed = false;
static bool pickTruncated = false;
static int pickSel = 0, pickTop = 0;
static Screen pickerBack = Screen::NowPlaying;
static constexpr int kRowH = 17, kRows = 5, kListY = 30;

static int pickRowCount() { return 1 + (int)pickList.size(); }   // row 0 = play this folder

static void openFolderPicker(const String& startAt)
{
  pickerBack = screen == Screen::FolderPicker ? pickerBack : screen;
  pickTruncated = false;
  pickFailed = !listDir(startAt, pickList, pickTruncated);
  pickDir = startAt;
  pickSel = 0;
  pickTop = 0;
  if (cfg.dir != pickDir) { cfg.dir = pickDir; markDirty(); }
}

static void pickGoUp()
{
  if (pickDir == "/") return;
  openFolderPicker(Library::parent(pickDir));
}

/// Start playing the folder the picker is currently in (row 0), or the
/// folder's MP3s from the highlighted file.
static void pickActivate()
{
  if (pickFailed) return;
  const LibEntry* chosen = nullptr;
  int start = 0;
  if (pickSel > 0)
  {
    const LibEntry& e = pickList[pickSel - 1];
    if (e.dir) { openFolderPicker(Library::join(pickDir, e.name)); return; }
    chosen = &e;
  }

  std::vector<String> paths;
  for (const auto& x : pickList)
  {
    if (x.dir) continue;
    if (&x == chosen) start = paths.size();
    if (ESP.getFreeHeap() < 24 * 1024 && paths.size()) break;   // low heap: play what we have
    paths.push_back(Library::join(pickDir, x.name));
  }
  if (paths.empty()) { toast("这个文件夹里没有 MP3"); return; }

  // Local and radio are exclusive: picking a folder ends the stream.
  if (player.radioMode())
  {
    beginSwitch("正在切换到音乐库", 0);
    player.stop();
    uint32_t t0 = millis();
    while (player.radioMode() && millis() - t0 < 2500) delay(10);
  }
  player.playList(std::move(paths), start);
  cfg.lastPath = Library::join(pickDir, chosen ? chosen->name : String());
  cfg.lastPos  = 0;
  markDirty();
  screen = Screen::NowPlaying;
}

static bool mountSD();          // defined near setup()

/// Park the decoder (close its file, so the card sits completely idle) for the
/// current scope, then seamlessly resume at the old position. Even with every
/// card access strictly serialised, directory enumeration kept failing while a
/// local MP3 was being decoded — but listing after "radio stopped" always
/// worked, so a closed decoder handle is what the FATFS/SDSPI layer needs.
struct SdSuspend
{
  bool armed = false, parked = false;
  SdSuspend()
  {
    if (player.loaded() && !player.radioMode())
    {
      player.requestSuspend();
      armed = true;
      uint32_t t0 = millis();
      while (!player.suspended() && millis() - t0 < 1500) delay(2);
      parked = player.suspended();
    }
  }
  ~SdSuspend() { if (armed) player.resumeAfterSD(); }
  /// True when no decoder file is open (parked, or nothing was decoding).
  bool cardIdle() const { return !armed || parked; }
};

/// Read a directory defensively. The decoder is parked for the sweep, so the
/// card is idle; if it still answers wrong, remount and read once more.
static bool listDir(const String& path, std::vector<LibEntry>& out, bool& trunc)
{
  trunc = false;
  SdSuspend suspend;
  bool ok = Library::list(SD, path, out, trunc);
  if (ok && !out.empty()) return true;

  // Remounting pulls the card out from under any open file handle — only do
  // it when the handshake actually parked the decoder (or none was running).
  if (!suspend.cardIdle()) return false;
  SD.end();
  delay(60);
  bool t2 = false;
  ok = mountSD() && Library::list(SD, path, out, t2);
  trunc = trunc || t2;
  if (!ok)
  {
    Library::Diag d = Library::diag();
    sdFailDiag = String("step=") + d.step + " err=" + d.err +
                 " heap=" + (int)(ESP.getFreeHeap() / 1024) + "K " + path;
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
// Two faces live in the firmware: efont CN (bundled with M5GFX, 7545 glyphs,
// covers all of GB2312) and the Latin-only DejaVu faces. A string is drawn
// entirely in one face, so anything that may hold Chinese is drawn with the
// efont face and everything else keeps the smaller DejaVu one.
// efont is a bitmap family generated on a 16x16 grid; the 10/12/14 px sizes
// are that grid with pixels knocked out, so their strokes never look crisp on
// this panel. 14 px was picked over 12 (same cost, visibly cleaner) and over
// 16 (the sharpest, but it crowds every list on a 135 px tall screen — the
// layout rework it forces was judged not worth it).
static const lgfx::IFont* const kFaceSmall = &fonts::efontCN_14;   // chrome, hints
static const lgfx::IFont* const kFaceBody  = &fonts::efontCN_16;   // names, titles

static bool hasWide(const String& s)
{
  for (size_t i = 0; i < s.length(); ++i)
    if ((uint8_t)s[i] >= 0x80) return true;
  return false;
}

/// Select the face for s: efont when it holds non-ASCII, else the Latin one.
static void useFace(const String& s, const lgfx::IFont* latin, bool small = false)
{
  cv.setFont(hasWide(s) ? (small ? kFaceSmall : kFaceBody) : latin);
}

/// Byte offset of the next UTF-8 code point, so cuts never split a character.
static size_t nextCp(const String& s, size_t i)
{
  uint8_t c = (uint8_t)s[i];
  size_t step = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
  return (i + step <= s.length()) ? i + step : s.length();
}

/// Truncate to maxW with "...", on a character boundary. Also leaves the
/// matching face selected, so the caller can draw the result straight away.
static String fitText(const String& s, int maxW, const lgfx::IFont* latin, bool small = false)
{
  useFace(s, latin, small);
  if (cv.textWidth(s) <= maxW) return s;
  String out;
  for (size_t i = 0; i < s.length(); )
  {
    size_t j = nextCp(s, i);
    if (cv.textWidth(out + s.substring(i, j) + "...") > maxW) break;
    out += s.substring(i, j);
    i = j;
  }
  out.trim();
  return out + "...";
}

static String fmtTime(float sec)
{
  if (sec < 0) sec = 0;
  uint32_t s = (uint32_t)sec;
  char b[16];
  if (s >= 3600) snprintf(b, sizeof(b), "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(b, sizeof(b), "%u:%02u", s / 60, s % 60);
  return b;
}

static int batteryLevel = -1;
static TrackInfo info;          // 最近一次发布的播放信息，drawHeader 也要用

static void drawPill(int& xr, const char* label, uint16_t fg, uint16_t bgc)
{
  String s(label);
  useFace(s, &fonts::DejaVu9, true);
  int w = cv.textWidth(s) + 8;
  xr -= w;
  cv.fillRoundRect(xr, 1, w, 12, 3, bgc);
  cv.setTextColor(fg);
  cv.setTextDatum(middle_center);
  cv.drawString(s, xr + w / 2, 7);
  xr -= 3;
}

static void drawHeader(const char* title, bool showModes = true)
{
  cv.fillRect(0, 0, W, 14, col::panel);
  String t(title);
  useFace(t, &fonts::DejaVu9, true);
  cv.setTextDatum(middle_left);
  cv.setTextColor(col::dim);
  cv.drawString(t, 5, 7);

  int xr = W - 4;
  // battery
  if (batteryLevel >= 0)
  {
    int bx = xr - 18;
    uint16_t c = batteryLevel <= 15 ? col::warn : col::text;
    cv.drawRoundRect(bx, 3, 16, 8, 2, c);
    cv.fillRect(bx + 16, 5, 2, 4, c);
    cv.fillRect(bx + 2, 5, (12 * batteryLevel) / 100, 4, batteryLevel <= 15 ? col::warn : col::good);
    xr = bx - 3;
    char b[8];
    snprintf(b, sizeof(b), "%d%%", batteryLevel);
    cv.setTextDatum(middle_right);
    cv.setTextColor(col::dim);
    cv.drawString(b, xr, 7);
    xr -= cv.textWidth(b) + 7;
  }
  // volume
  {
    char b[8];
    snprintf(b, sizeof(b), "%d", muted ? 0 : cfg.volume);
    cv.setTextDatum(middle_right);
    cv.setTextColor(muted ? col::warn : col::dim);
    cv.drawString(b, xr, 7);
    xr -= cv.textWidth(b) + 2;
    uint16_t c = muted ? col::warn : col::dim;
    cv.fillRect(xr - 9, 5, 3, 4, c);
    cv.fillTriangle(xr - 7, 7, xr - 3, 2, xr - 3, 12, c);
    xr -= 14;
  }
  // modes. Skippable: on Help the "H / ESC 关闭" label reaches into this area
  // and collided with the "直播" pill whenever a station was playing.
  if (showModes && info.radio)
  {
    drawPill(xr, "直播", col::bg, col::accent);
  }
  else
  {
    RepeatMode rep = player.repeat();
    if (rep != RepeatMode::Off) drawPill(xr, rep == RepeatMode::One ? "单曲" : "循环", col::bg, col::cool);
    if (player.shuffle())       drawPill(xr, "随机", col::bg, col::accent);
  }
}

/// One hint row, bottom-aligned at y. Strings that would still be wider than
/// the panel get clipped with "..." instead of running off the screen edge.
static void drawHintAt(const char* s, int y)
{
  String t(s);
  useFace(t, &fonts::DejaVu9, true);
  if (cv.textWidth(t) > W - 4) t = fitText(t, W - 4, &fonts::DejaVu9, true);
  cv.setTextDatum(bottom_center);
  cv.setTextColor(col::dim);
  cv.drawString(t, W / 2, y);
}

static void drawHint(const char* s) { drawHintAt(s, H - 1); }

static void drawCentered(const char* a, const char* b = nullptr, int y = 62)
{
  String sa(a);
  cv.setTextDatum(middle_center);
  useFace(sa, &fonts::DejaVu12);
  cv.setTextColor(col::text);
  cv.drawString(sa, W / 2, y);
  if (b)
  {
    String sb(b);
    useFace(sb, &fonts::DejaVu9, true);
    cv.setTextColor(col::dim);
    cv.drawString(sb, W / 2, y + 19);
  }
}

static void iconFolder(int x, int y, uint16_t c)
{
  cv.fillRect(x, y + 3, 5, 2, c);
  cv.fillRoundRect(x, y + 4, 12, 8, 1, c);
}

static void iconNote(int x, int y, uint16_t c)
{
  cv.fillCircle(x + 3, y + 10, 2, c);
  cv.drawFastVLine(x + 5, y + 2, 9, c);
  cv.drawLine(x + 5, y + 2, x + 9, y + 4, c);
  cv.drawLine(x + 5, y + 3, x + 9, y + 5, c);
}

static void iconUp(int x, int y, uint16_t c)
{
  cv.fillTriangle(x + 5, y + 2, x, y + 8, x + 10, y + 8, c);
  cv.fillRect(x + 3, y + 8, 5, 4, c);
}

// ---------------------------------------------------------------------------
// Resume: remember which file was playing and where it stopped
// ---------------------------------------------------------------------------
static void saveResume()
{
  if (info.radio || !player.loaded() || !info.path.length()) return;
  uint16_t pos = (uint16_t)player.elapsed();
  if (cfg.lastPath == info.path && cfg.lastPos == pos) return;
  cfg.lastPath = info.path;
  cfg.lastPos  = pos;
  markDirty();
}

// ---------------------------------------------------------------------------
// Source-switch transition page: shown while the audio task swaps streams, so
// the swap never flashes a stale page that looks like a glitch.
// ---------------------------------------------------------------------------
static String   switchText;
static String   switchWant;           // for radio: URL the player must land on
static uint8_t  switchTarget = 0;     // 0 -> mp3 (info.radio clears), 1 -> radio (info.radio sets)
static uint32_t switchUntil  = 0;

static void beginSwitch(const char* text, uint8_t target, const char* want)
{
  switchText   = text;
  switchTarget = target;
  switchWant   = want;
  switchUntil  = millis() + 3000;
}

/// Home → 音乐库: carry on with the last file when there is one; the first
/// time round there is nothing to carry on with, so open the browser and let
/// the folder be picked.
static void openMusic()
{
  // The two modes are exclusive: opening the library ends the radio stream.
  // That is what "switch" means to the user, and it also frees the memory and
  // CPU the stream holds — which is what keeps SD reads reliable afterwards.
  if (player.radioMode())
  {
    beginSwitch("正在切换到音乐库", 0);
    player.stop();
    uint32_t t0 = millis();
    while (player.radioMode() && millis() - t0 < 2500) delay(10);
  }
  // Park the decoder while probing the card: SD.exists / listDir under an
  // open decode handle are exactly what kept failing (see SdSuspend).
  if (player.loaded() && !player.radioMode())
  {
    player.requestSuspend();
    uint32_t t0 = millis();
    while (!player.suspended() && millis() - t0 < 1500) delay(2);
  }
  bool haveResume = cfg.lastPath.length() && SD.exists(cfg.lastPath.c_str());
  if (haveResume)
  {
    String dir = Library::parent(cfg.lastPath);
    std::vector<LibEntry> tmp;
    bool trunc = false;
    if (listDir(dir, tmp, trunc))
    {
      std::vector<String> paths;
      int idx = 0;
      for (const auto& e : tmp)
      {
        if (e.dir || !Library::isMp3(e.name)) continue;
        if (ESP.getFreeHeap() < 24 * 1024 && paths.size()) break;   // low heap: play what we have
        String full = Library::join(dir, e.name);
        if (full == cfg.lastPath) idx = (int)paths.size();
        paths.push_back(full);
      }
      if (!paths.empty())
      {
        player.playList(std::move(paths), idx, (float)cfg.lastPos);
        player.resumeAfterSD();   // only clears the parked flag; C_LIST already restarted decode
        screen = Screen::NowPlaying;
        return;
      }
    }
  }
  // Nothing to resume with: straight to the player page, the folder picker is
  // one key away (F). Entering the library NEVER browses the card on its own.
  player.resumeAfterSD();
  switchUntil = 0;                        // not switching to a player page after all
  screen = Screen::NowPlaying;
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
static uint32_t infoVer = UINT32_MAX;

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------
static int    menuSel = 0, menuTop = 0;          // 分类页
static int    radioGroup = -3;                   // -3 搜索结果, -2 最近收听, -1 全部, >=0 分类
static int    stationSel = 0, stationTop = 0;
// The station list is only a list of flat indices into the static table in
// Stations.h — no copies, no heap. Copying the stations (170 of them, ~5.4 KB
// per copy) used to be enough to abort the app when it happened while a
// stream was running.
static std::vector<int> radioItems;

// -- 搜索 ------------------------------------------------------------------
#include "StationsPinyin.h"
static String searchQ;                // 已输入的搜索词（ASCII）
static std::vector<int> searchHits;   // 命中的 flat 索引
static std::bitset<128> prevSearchChars;
static bool prevSearchEnter = false, prevSearchBksp = false, prevSearchEsc = false;

/// 台名 / 全拼 / 拼音首字母任一含搜索词即命中。searchQ 必须是小写。
static bool stationMatch(int flat, const char* q)
{
  const Station* st = stations::at(flat, nullptr);
  if (!st) return false;
  if (strstr(st->name, q))
  {
    // 台名是 UTF-8，大小写敏感匹配只对数字和字母有意义，中文总命中
    return true;
  }
  if (flat >= STATIONS_PINYIN_COUNT) return false;
  if (strstr_P(kPinyinFull[flat], q)) return true;
  // 纯字母的词才查首字母表（数字/混合词在首字母表里没有意义）
  bool letters = true;
  for (const char* p = q; *p; ++p)
    if (*p < 'a' || *p > 'z') { letters = false; break; }
  if (letters && strstr_P(kPinyinInit[flat], q)) return true;
  return false;
}

static void runSearch()
{
  searchHits.clear();
  if (!searchQ.length()) return;
  String q = searchQ;
  q.toLowerCase();
  for (int f = 0; f < stations::total(); ++f)
    if (stationMatch(f, q.c_str())) searchHits.push_back(f);
}

static void resetSearchInput()
{
  searchQ = "";
  searchHits.clear();
  prevSearchChars.reset();
  prevSearchEnter = prevSearchBksp = prevSearchEsc = true;
}

static void buildRadioItems(int group)
{
  radioItems.clear();
  if (group == -2)                                   // 最近收听
  {
    radioItems.reserve(histCount);
    for (int i = 0; i < histCount; ++i)
    {
      int f = stations::indexOfUrl(histUrl[i].c_str());
      if (f >= 0) radioItems.push_back(f);           // 台已被移出表就不再显示
    }
    return;
  }
  if (group == -3)                                   // 搜索结果
  {
    radioItems = searchHits;
    return;
  }
  if (group < 0)                                     // 全部电台
  {
    radioItems.reserve(stations::total());
    for (int f = 0; f < stations::total(); ++f) radioItems.push_back(f);
    return;
  }
  int start = stations::groupStart(group);
  int n = stations::groupCount(group);
  radioItems.reserve(n);
  for (int i = 0; i < n; ++i) radioItems.push_back(start + i);
}

static int menuCount() { return 3 + stations::kGroupCount; }

static const char* menuTitle(int i)
{
  static const char* kHead[3] = { "最近收听", "搜索电台", "全部电台" };
  return i < 3 ? kHead[i] : stations::kGroups[i - 3].name;
}

static String menuRight(int i)
{
  char b[20];
  if (i == 0) return histCount ? histName[0] : String("还没有");
  if (i == 1) { snprintf(b, sizeof(b), "%d 台", (int)searchHits.size()); return searchQ.length() ? String(b) : String("输入关键词"); }
  if (i == 2) { snprintf(b, sizeof(b), "%d 台", stations::total()); return b; }
  snprintf(b, sizeof(b), "%d 台", stations::kGroups[i - 3].count);
  return b;
}

static void iconClock(int x, int y, uint16_t c)
{
  cv.drawCircle(x + 6, y + 6, 5, c);
  cv.drawFastVLine(x + 6, y + 3, 4, c);
  cv.drawFastHLine(x + 6, y + 6, 3, c);
}

static void iconList(int x, int y, uint16_t c)
{
  for (int i = 0; i < 3; ++i) cv.fillRect(x, y + i * 4, 12, 2, c);
}

static void iconRadio(int x, int y, uint16_t c)
{
  cv.drawCircle(x + 6, y + 6, 5, c);
  cv.fillCircle(x + 6, y + 6, 2, c);
}

static void iconSearch(int x, int y, uint16_t c)
{
  cv.drawCircle(x + 5, y + 5, 4, c);
  cv.drawFastHLine(x + 8, y + 8, 5, c);
  cv.drawFastHLine(x + 9, y + 7, 3, c);
  cv.drawFastHLine(x + 10, y + 6, 3, c);
}

static void iconLock(int x, int y, uint16_t c)
{
  cv.drawRect(x + 3, y + 1, 4, 3, c);
  cv.fillRect(x + 1, y + 4, 8, 5, c);
}

/// Signal bars: -50 dBm and better is four bars.
static void iconWifi(int x, int y, int rssi, uint16_t c)
{
  int bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
  for (int i = 0; i < 4; ++i)
    cv.fillRect(x + i * 4, y + 10 - (i + 1) * 2, 2, (i + 1) * 2, i < bars ? c : col::panel2);
}

static void scrollBar(int n, int top)
{
  if (n <= kRows) return;
  int trackH = kRows * kRowH - 2;
  int th = max(8, trackH * kRows / n);
  int ty = kListY + (trackH - th) * top / (n - kRows);
  cv.fillRoundRect(235, kListY, 3, trackH, 1, col::panel);
  cv.fillRoundRect(235, ty, 3, th, 1, col::dim);
}

static void drawRadioMenu()
{
  drawHeader("网络电台");

  String st;
  uint16_t c = col::dim;
  if (net::connected())          { st = String("WiFi ") + net::ssid(); }
  else if (net::connecting())    { st = "WiFi 连接中…"; c = col::cool; }
  else                           { st = "未连接 WiFi，按 W 设置"; c = col::warn; }
  cv.setTextDatum(top_left);
  cv.setTextColor(c);
  useFace(st, &fonts::DejaVu9, true);
  cv.drawString(fitText(st, 226, &fonts::DejaVu9, true), 6, 15);

  int n = menuCount();
  if (menuSel < menuTop) menuTop = menuSel;
  if (menuSel >= menuTop + kRows) menuTop = menuSel - kRows + 1;

  for (int i = menuTop; i < n && i < menuTop + kRows; ++i)
  {
    int y = kListY + (i - menuTop) * kRowH;
    if (i == menuSel)
    {
      cv.fillRoundRect(2, y, 230, kRowH - 1, 3, col::panel2);
      cv.fillRect(2, y + 3, 2, kRowH - 7, col::accent);
    }
    if (i == 0)      iconClock(8, y + 4, col::cool);
    else if (i == 1) iconSearch(8, y + 3, col::cool);
    else if (i == 2) iconList(8, y + 5, col::cool);
    else             iconFolder(8, y + 3, col::folder);

    cv.setTextDatum(middle_left);
    cv.setTextColor(col::text);
    String label(menuTitle(i));
    useFace(label, &fonts::DejaVu12);
    cv.drawString(fitText(label, 110, &fonts::DejaVu12), 25, y + kRowH / 2);

    cv.setTextDatum(middle_right);
    cv.setTextColor(col::dim);
    String right = menuRight(i);
    useFace(right, &fonts::DejaVu9, true);
    cv.drawString(fitText(right, 96, &fonts::DejaVu9, true), 228, y + kRowH / 2);
  }
  scrollBar(n, menuTop);
  drawHint("ENT 打开　W WiFi　L 音乐库　H 帮助");
}

static void drawStationList()
{
  drawHeader("网络电台");

  String crumb;
  if (radioGroup == -3)      crumb = String("搜索：") + searchQ;
  else if (radioGroup == -2) crumb = "最近收听";
  else if (radioGroup == -1) crumb = "全部电台";
  else                       crumb = stations::kGroups[radioGroup].name;
  char pos[20];
  snprintf(pos, sizeof(pos), "　%d/%d", stationSel + 1, (int)radioItems.size());
  crumb += pos;
  cv.setTextDatum(top_left);
  cv.setTextColor(col::cool);
  useFace(crumb, &fonts::DejaVu9, true);
  cv.drawString(crumb, 6, 15);

  int n = radioItems.size();
  if (!n)
  {
    if (radioGroup == -2) drawCentered("还没有最近收听", "去电台里挑一个吧");
    else                  drawCentered("没有匹配的电台", "换个关键词试试");
    drawHint("BKSP 返回");
    return;
  }

  if (stationSel < stationTop) stationTop = stationSel;
  if (stationSel >= stationTop + kRows) stationTop = stationSel - kRows + 1;

  for (int i = stationTop; i < n && i < stationTop + kRows; ++i)
  {
    int y = kListY + (i - stationTop) * kRowH;
    int g = -1;
    const Station* st = stations::at(radioItems[i], &g);
    if (!st) continue;
    bool playing = player.radioMode() && info.radio && info.path == st->url;
    if (i == stationSel)
    {
      cv.fillRoundRect(2, y, 230, kRowH - 1, 3, col::panel2);
      cv.fillRect(2, y + 3, 2, kRowH - 7, col::accent);
    }
    iconRadio(8, y + 4, playing ? col::accent : col::dim);

    cv.setTextDatum(middle_left);
    cv.setTextColor(playing ? col::accent : col::text);
    String label(st->name);
    useFace(label, &fonts::DejaVu12);
    cv.drawString(fitText(label, radioGroup == -1 ? 158 : 190, &fonts::DejaVu12), 25, y + kRowH / 2);

    String right;
    if (radioGroup == -1) right = (g >= 0 && g < stations::kGroupCount) ? stations::kGroups[g].name : "";
    else
    {
      char b[16];
      snprintf(b, sizeof(b), "%s %dk", st->codec ? "AAC" : "MP3", st->kbps);
      right = b;
    }
    cv.setTextDatum(middle_right);
    cv.setTextColor(col::dim);
    useFace(right, &fonts::DejaVu9, true);
    cv.drawString(fitText(right, 78, &fonts::DejaVu9, true), 228, y + kRowH / 2);
  }
  scrollBar(n, stationTop);
  drawHint("ENT 收听　BKSP 返回　N/P 切台");
}

// -- 搜索输入页：输入行 + 实时命中预览 -------------------------------------
static void drawRadioSearch()
{
  drawHeader("搜索电台");

  // 输入行（y 30..48）
  cv.fillRoundRect(4, kListY, 228, 18, 4, col::panel2);
  cv.setTextDatum(middle_left);
  cv.setTextColor(col::text);
  String line = searchQ.length() ? (searchQ + "_") : String("| 输入拼音或台名…");
  useFace(line, &fonts::DejaVu12);
  cv.drawString(fitText(line, 220, &fonts::DejaVu12), 12, kListY + 9);

  int n = searchHits.size();
  char pos[28];
  snprintf(pos, sizeof(pos), n ? "命中 %d 台，ENT 查看" : "无匹配", (int)n);
  cv.setTextDatum(top_left);
  cv.setTextColor(n ? col::cool : col::dim);
  useFace(String(pos), &fonts::DejaVu9, true);
  cv.drawString(pos, 6, kListY + 22);

  if (!n)
  {
    cv.setTextDatum(middle_center);
    cv.setTextColor(col::dim);
    cv.drawString(searchQ.length() ? "换个关键词试试" : "例：fm97 / zgz / 文艺",
                  W / 2, kListY + 45);
    drawHint("ENT 查看　BKSP 删字　ESC 关闭");
    return;
  }

  // 命中预览（前 3 行，屏高有限）
  for (int i = 0; i < n && i < 3; ++i)
  {
    int y = kListY + 34 + i * kRowH;
    const Station* st = stations::at(searchHits[i], nullptr);
    if (!st) continue;
    cv.setTextDatum(middle_left);
    cv.setTextColor(i == 0 ? col::text : col::dim);
    String label(st->name);
    useFace(label, &fonts::DejaVu12);
    cv.drawString(fitText(label, 216, &fonts::DejaVu12), 10, y + kRowH / 2);
  }
  drawHint("ENT 查看全部　BKSP 删字　ESC 关闭");
}

// ---------------------------------------------------------------------------
// Home: the boot menu, so neither mode is entered behind the user's back
// ---------------------------------------------------------------------------
static int homeSel = 0;

static void homeCard(int y, bool sel, bool radio)
{
  cv.fillRoundRect(8, y, 224, 42, 6, sel ? col::panel2 : col::panel);
  if (sel) cv.drawRoundRect(8, y, 224, 42, 6, col::accent);

  // icon plate, left
  cv.fillRoundRect(16, y + 8, 26, 26, 6, sel ? col::accent : col::panel2);
  uint16_t ic = sel ? col::bg : (radio ? col::cool : col::folder);
  if (radio) iconRadio(24, y + 15, ic);
  else       iconFolder(24, y + 15, ic);

  String title(radio ? "网络电台" : "音乐库");
  cv.setTextDatum(middle_left);
  cv.setTextColor(col::text);
  useFace(title, &fonts::DejaVu12);
  cv.drawString(title, 52, y + 12);

  String sub;
  if (radio)
  {
    // Radio card: streaming state first, connection state otherwise.
    if (player.radioMode() && player.loaded()) sub = String("正在收听 ") + info.title;
    else if (net::connected())                 sub = String("WiFi ") + net::ssid();
    else if (net::connecting())                sub = "WiFi 连接中…";
    else                                       sub = "未连 WiFi，进去按 W";
  }
  else
  {
    // Library card: local MP3 state only — never the radio's.
    if (player.loaded() && !player.radioMode()) sub = String("正在播放 ") + info.title;
    else if (cfg.lastPath.length())             sub = String("接着听 ") + Library::baseName(cfg.lastPath);
    else                                        sub = "先选一个音乐文件夹";
  }

  cv.setTextColor(sel ? col::cool : col::dim);
  useFace(sub, &fonts::DejaVu9, true);
  cv.drawString(fitText(sub, 172, &fonts::DejaVu9, true), 52, y + 30);
}

static void drawHome()
{
  drawHeader("CardRadio v1.1");
  homeCard(18, homeSel == 0, false);
  homeCard(64, homeSel == 1, true);
  drawHint("上下选择　ENT 进入　W WiFi");
}

static void openHome(int which)
{
  if (which == 0) { openMusic(); return; }
  menuSel = 0;
  menuTop = 0;
  screen = Screen::RadioMenu;
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static net::Ap aps[12];
static int    apCount = 0, apSel = 0, apTop = 0;
static bool   scanning = false;
static String pendingSsid, passBuf;
static bool   pendingSecure = false;
static Screen wifiBack = Screen::RadioMenu;

// Raw key edges for password entry (declared here so beginConnect can reset them).
static std::bitset<128> prevRawChars;

// ---------------------------------------------------------------------------
// Sleep timer (T): stop playback and cut power at a chosen minute mark.
// Only the deep-sleep variant exists: stopping alone keeps WiFi's modem at
// full draw (streaming needs WiFi.setSleep(false)) and a battery would not
// survive the night. Waking from deep sleep is a full reboot — everything
// worth keeping already lives in NVS.
static Screen timerBack = Screen::NowPlaying;       // where T was pressed
static const uint16_t kSleepOpts[] = { 15, 30, 45, 60, 90, 120, 150 };  // minutes
static uint16_t sleepMins = 0;                      // 0 = timer off
static uint32_t sleepDue  = 0;                      // millis() stamp, signed compare
static int      sleepSel  = 0;                      // cursor on the timer page

static void setSleepTimer(uint16_t mins)
{
  sleepMins = mins;
  sleepDue  = mins ? millis() + mins * 60000UL : 0;
  markDirty();
}

// "MM:SS" left until shutdown, or "" when the timer is off.
static String sleepRemaining()
{
  if (!sleepMins) return String();
  int32_t left = (int32_t)(sleepDue - millis());
  if (left < 0) left = 0;
  char b[10];
  snprintf(b, sizeof(b), "%02d:%02d", (int)(left / 60000), (int)((left % 60000) / 1000));
  return String(b);
}

// Timer fired: stop everything and hand the board to deep sleep. G0 (GPIO0,
// the side button, RTC-capable and active-low) is the only wake source.
// esp_deep_sleep_start() never returns; the reboot it eventually performs
// boots straight into setup().
static void doSleepShutdown()
{
  saveResume();
  player.stop();
  setSleepTimer(0);
  saveSettings();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  M5.Display.setBrightness(0);

  cv.fillScreen(col::bg);
  drawCentered("睡眠关机", "按 G0 键唤醒");
  delay(1800);                       // let whoever is still looking read it

  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();            // never returns
}
static bool prevEnter = false, prevBksp = false, prevEsc = false;

static void drawWifiList()
{
  drawHeader("设置 · WiFi");

  String st = String("当前：") + net::status();
  if (net::connected()) st += String(" ") + net::ssid();
  cv.setTextDatum(top_left);
  cv.setTextColor(net::connected() ? col::good : (net::connecting() ? col::cool : col::warn));
  useFace(st, &fonts::DejaVu9, true);
  cv.drawString(fitText(st, 168, &fonts::DejaVu9, true), 6, 15);

  char cap[20];
  snprintf(cap, sizeof(cap), "已存 %d/%d", net::savedCount(), net::kMaxSaved);
  cv.setTextDatum(top_right);
  cv.setTextColor(col::dim);
  useFace(String(cap), &fonts::DejaVu9, true);
  cv.drawString(cap, 233, 15);

  if (scanning)
  {
    drawCentered("正在扫描…", "大约 2 秒", 68);
    drawHint("请稍候");
    return;
  }
  if (!apCount)
  {
    String why = net::scanError();
    drawCentered("没扫描到网络", why.length() ? why.c_str() : "按 R 重新扫描");
    drawHint("R 重扫　BKSP 返回");
    return;
  }

  if (apSel < apTop) apTop = apSel;
  if (apSel >= apTop + kRows) apTop = apSel - kRows + 1;

  for (int i = apTop; i < apCount && i < apTop + kRows; ++i)
  {
    int y = kListY + (i - apTop) * kRowH;
    bool here = net::connected() && aps[i].ssid == net::ssid();
    bool kept = net::isSaved(aps[i].ssid);
    if (i == apSel)
    {
      cv.fillRoundRect(2, y, 230, kRowH - 1, 3, col::panel2);
      cv.fillRect(2, y + 3, 2, kRowH - 7, col::accent);
    }
    // Bars: how many are lit says the strength, the colour repeats it — green
    // strong, amber medium, red weak. Filling them all grey hid the difference.
    uint16_t sig = aps[i].rssi > -62 ? col::good
                 : aps[i].rssi > -76 ? col::folder
                                     : col::warn;
    iconWifi(7, y + 2, aps[i].rssi, sig);

    cv.setTextDatum(middle_left);
    cv.setTextColor(here ? col::good : (kept ? col::cool : col::text));
    String label = aps[i].ssid;
    useFace(label, &fonts::DejaVu12);
    cv.drawString(fitText(label, 150, &fonts::DejaVu12), 27, y + kRowH / 2);

    if (here)
    {
      String t("已连接");
      cv.setTextDatum(middle_right);
      cv.setTextColor(col::good);
      useFace(t, &fonts::DejaVu9, true);
      cv.drawString(t, 214, y + kRowH / 2);
    }
    else
    {
      if (kept)
      {
        String t("已存");
        cv.setTextDatum(middle_right);
        cv.setTextColor(col::cool);
        useFace(t, &fonts::DejaVu9, true);
        cv.drawString(t, 212, y + kRowH / 2);
      }
      if (aps[i].secure) iconLock(218, y + 4, col::dim);
    }
  }
  scrollBar(apCount, apTop);
  drawHint("ENT 连接　X 忘记　R 重扫　ESC 返回");
}

static void drawWifiPass()
{
  drawHeader("设置 · WiFi");

  cv.setTextDatum(top_left);
  cv.setTextColor(col::dim);
  String l1("网络");
  useFace(l1, &fonts::DejaVu9, true);
  cv.drawString(l1, 12, 22);

  cv.setTextColor(col::cool);
  String s(pendingSsid);
  useFace(s, &fonts::DejaVu12);
  cv.drawString(fitText(s, 212, &fonts::DejaVu12), 12, 35);

  cv.fillRoundRect(10, 57, 220, 26, 5, col::panel);
  cv.drawRoundRect(10, 57, 220, 26, 5, col::accent);
  String dots;
  for (size_t i = 0; i < passBuf.length(); ++i) dots += '*';
  if (dots.length() > 26) dots = dots.substring(dots.length() - 26);
  cv.setFont(&fonts::DejaVu12);
  cv.setTextDatum(middle_left);
  cv.setTextColor(col::text);
  cv.drawString(dots, 18, 70);
  cv.fillRect(19 + cv.textWidth(dots), 63, 2, 14, col::text);

  char cnt[28];
  snprintf(cnt, sizeof(cnt), "键盘输入　%d / 63", (int)passBuf.length());
  cv.setTextColor(col::dim);
  String c2(cnt);
  useFace(c2, &fonts::DejaVu9, true);
  cv.drawString(c2, 12, 90);

  String tip("输入完按 ENT 连接，ESC 取消");
  useFace(tip, &fonts::DejaVu9, true);
  cv.drawString(tip, 12, 104);

  drawHint("ENT 连接　BKSP 删除　ESC 取消");
}

static void openStationList(int group)
{
  radioGroup = group;
  buildRadioItems(group);
  stationSel = 0;
  stationTop = 0;
  screen = Screen::RadioStations;
}

static void playSelectedStation()
{
  if (stationSel < 0 || stationSel >= (int)radioItems.size()) return;
  if (!net::connected())
  {
    toast("先连 WiFi：按 W 设置");
    return;
  }
  int flat = radioItems[stationSel];
  int g = -1;
  const Station* st = stations::at(flat, &g);
  if (!st) return;

  // MP3 → radio, or radio → a different station: hold the transition page
  // until the player actually lands on the new stream.
  if (!player.radioMode() || info.path != st->url)
    beginSwitch("正在连接电台", 1, st->url);

  // Only the group and the index are handed over: the player looks the
  // station up in the static table itself. No copies, no heap.
  int group = (radioGroup >= 0) ? radioGroup : -1;      // -1 = cycle through all
  player.playRadio(group, flat);

  pushRecent(st->url, st->name);

  screen = Screen::NowPlaying;
}

static void startWifiScan()
{
  apCount = 0;
  apSel = 0;
  apTop = 0;
  scanning = true;
  net::scanStart();
}

static void beginConnect(int index)
{
  if (index < 0 || index >= apCount) return;
  pendingSsid = aps[index].ssid;
  pendingSecure = aps[index].secure;
  passBuf = "";

  // Already in one of the five slots → join with the stored password. Re-typing
  // a 20-character key just to move between home and office is what this whole
  // multi-network thing exists to avoid. A wrong stored key can be cleared
  // with X in the list and entered again.
  String kept;
  if (net::savedPass(pendingSsid, kept))
  {
    net::connectTo(pendingSsid, kept);
    toast(String("连接 ") + pendingSsid);
    return;
  }

  if (pendingSecure)
  {
    prevRawChars.reset();
    // The ENTER that opened this screen is still physically held down: mark it
    // as already consumed, otherwise the next poll "confirms" an empty password
    // and bounces straight back to the list.
    prevEnter = prevBksp = prevEsc = true;
    screen = Screen::WifiPass;
  }
  else
  {
    net::connectTo(pendingSsid, "");
    toast(String("连接 ") + pendingSsid);
  }
}

static void drawFolderPicker()
{
  drawHeader("选择文件夹");

  cv.setTextDatum(top_left);
  cv.setTextColor(col::cool);
  String p = pickDir;
  useFace(p, &fonts::DejaVu9, true);
  if (cv.textWidth(p) > 228)
  {
    while (p.length() > 1 && cv.textWidth("..." + p) > 228) p = p.substring(nextCp(p, 0));
    p = "..." + p;
  }
  cv.drawString(p, 6, 15);

  int n = pickRowCount();
  if (pickFailed)
  {
    drawCentered("无法读取存储卡", "R 重读　ESC 返回");
    cv.setTextColor(col::dim);
    useFace(sdFailDiag, &fonts::DejaVu9, true);
    cv.drawString(sdFailDiag, 6, H - 14);
    return;
  }

  if (pickSel < pickTop) pickTop = pickSel;
  if (pickSel >= pickTop + kRows) pickTop = pickSel - kRows + 1;

  for (int i = pickTop; i < n && i < pickTop + kRows; ++i)
  {
    int y = kListY + (i - pickTop) * kRowH;
    bool isSel = (i == pickSel);
    if (isSel)
    {
      cv.fillRoundRect(2, y, 230, kRowH - 1, 3, col::panel2);
      cv.fillRect(2, y + 3, 2, kRowH - 7, col::accent);
    }
    uint16_t tc = col::text;
    String label;
    if (i == 0)
    {
      iconNote(9, y + 2, isSel ? col::accent : col::dim);
      label = "播放此文件夹";
      tc = isSel ? col::accent : col::cool;
    }
    else
    {
      const LibEntry& e = pickList[i - 1];
      if (e.dir) iconFolder(8, y + 3, col::folder);
      else
      {
        bool playing = player.loaded() && info.path == Library::join(pickDir, e.name);
        if (playing) tc = col::accent;
        iconNote(9, y + 2, playing ? col::accent : col::dim);
      }
      label = e.label;
    }
    cv.setTextDatum(middle_left);
    cv.setTextColor(tc);
    String t = fitText(label, 200, &fonts::DejaVu12);
    cv.drawString(t, 25, y + kRowH / 2);
  }

  if (n > kRows)
  {
    int trackH = kRows * kRowH - 2;
    int th = max(8, trackH * kRows / n);
    int ty = kListY + (trackH - th) * pickTop / (n - kRows);
    cv.fillRoundRect(235, kListY, 3, trackH, 1, col::panel);
    cv.fillRoundRect(235, ty, 3, th, 1, col::dim);
  }
  drawHint(pickTruncated ? "只显示前 600 项　ENT 确认　BKSP 上级" : "ENT 确认　BKSP 上级　ESC 取消");
}

static void drawTransportIcon(int cx, int cy, PlayState st)
{
  cv.fillCircle(cx, cy, 12, col::accent);
  if (st == PlayState::Playing)
  {
    cv.fillRect(cx - 5, cy - 6, 4, 12, col::bg);
    cv.fillRect(cx + 1, cy - 6, 4, 12, col::bg);
  }
  else
  {
    cv.fillTriangle(cx - 4, cy - 7, cx - 4, cy + 7, cx + 7, cy, col::bg);
  }
}

static void drawSkipIcon(int cx, int cy, bool fwd)
{
  int d = fwd ? 1 : -1;
  cv.fillTriangle(cx - 5 * d, cy - 5, cx - 5 * d, cy + 5, cx + 2 * d, cy, col::text);
  cv.fillRect(fwd ? cx + 2 : cx - 4, cy - 5, 2, 11, col::text);
}

/// One-line marquee: static when the text fits, otherwise a slow leftward
/// scroll with a pause at the start. The caller's clip keeps it inside its box.
static void drawMarquee(const String& s, int x, int y, int w, int clipH,
                        const lgfx::IFont* latin, uint16_t c, bool small = true)
{
  if (!s.length()) return;
  useFace(s, latin, small);
  cv.setTextDatum(top_left);
  cv.setTextColor(c);
  int tw = cv.textWidth(s);
  cv.setClipRect(x, y - 1, w, clipH);
  if (tw <= w) cv.drawString(s, x, y);
  else
  {
    const int gap = 30, hold = 60;
    int period = tw + gap;
    int t = (millis() / 30) % (period + hold);
    int off = t < hold ? 0 : t - hold;
    cv.drawString(s, x - off, y);
    cv.drawString(s, x - off + period, y);
  }
  cv.clearClipRect();
}

static void drawNowPlayingRadio()
{
  // ---- station card -----------------------------------------------------
  cv.fillRoundRect(4, 16, 232, 44, 5, col::panel);

  // LIVE badge on the card's right edge, so the name never runs into it
  const char* live = "LIVE";
  useFace(String(live), &fonts::DejaVu9, true);
  int lw = cv.textWidth(live) + 10;
  int bx = 232 - lw;
  cv.fillRoundRect(bx, 20, lw, 13, 3, col::accent);
  cv.setTextDatum(middle_center);
  cv.setTextColor(col::bg);
  cv.drawString(live, bx + lw / 2, 27);

  // station name: big, and it scrolls instead of being cut short.
  // The card is only 44 px high, so the name uses the 12 px Chinese face —
  // the 16 px one gets cut in half by the clip box.
  drawMarquee(info.title.length() ? info.title : String("网络电台"),
              11, 20, bx - 15, 16, &fonts::FreeSansBold9pt7b, col::text, true);

  // category · on air
  cv.fillCircle(13, 44, 2, col::warn);
  String sub = info.artist.length() ? info.artist : String("网络电台");
  sub += " · 直播中";
  useFace(sub, &fonts::DejaVu9, true);
  cv.setTextDatum(top_left);
  cv.setTextColor(col::accent);
  cv.drawString(fitText(sub, 212, &fonts::DejaVu9, true), 19, 38);

  // ---- transport --------------------------------------------------------
  drawSkipIcon(88, 80, false);
  drawTransportIcon(W / 2, 80, player.state());
  drawSkipIcon(152, 80, true);

  // ---- footer row: listened time | stream format ------------------------
  cv.setTextDatum(top_left);
  cv.setTextColor(col::dim);
  String el = String("已收听 ") + fmtTime(player.elapsed());
  useFace(el, &fonts::DejaVu9, true);
  cv.drawString(el, 8, 107);

  cv.setTextDatum(top_right);
  if (sleepMins)                          // sleep timer countdown takes the slot
  {
    String left = String("关 ") + sleepRemaining();
    useFace(left, &fonts::DejaVu9, true);
    cv.setTextColor(col::warn);
    cv.drawString(left, 232, 107);
  }
  else
  {
    String fmt = String(info.codec ? "AAC " : "MP3 ") + info.bitrate + "k";
    uint32_t sr = player.output().rate();
    if (sr) fmt += String(" · ") + String(sr / 1000.0f, 1) + "k";
    useFace(fmt, &fonts::DejaVu9, true);
    cv.drawString(fmt, 232, 107);
  }

  drawHint("L 音乐库　N/P 切台　H 帮助　I 状态");
}

static void drawNowPlaying()
{
  drawHeader("正在播放");
  if (!player.loaded())
  {
    if (player.radioMode())   // C_RADIO accepted, decoder not up yet (DNS/TCP/prefill)
    {
      drawCentered("连接缓冲中…", "正在连接电台，请稍候");
      drawHint("BKSP 返回");
    }
    else
    {
      drawCentered("还没有播放曲目", "按 F 选择文件夹");
      drawHint("F 文件夹　E 均衡　H 帮助");
    }
    return;
  }

  if (info.radio) { drawNowPlayingRadio(); return; }

  // title, scrolling when it doesn't fit
  useFace(info.title, &fonts::FreeSansBold9pt7b);
  cv.setTextDatum(top_left);
  cv.setTextColor(col::text);
  const int tx = 8, tw = 224;
  int w = cv.textWidth(info.title);
  cv.setClipRect(tx, 17, tw, 20);
  if (w <= tw) cv.drawString(info.title, tx, 18);
  else
  {
    const int gap = 40, hold = 60;
    int period = w + gap;
    int t = (millis() / 28) % (period + hold);
    int off = t < hold ? 0 : t - hold;
    cv.drawString(info.title, tx - off, 18);
    cv.drawString(info.title, tx - off + period, 18);
  }
  cv.clearClipRect();

  cv.setTextColor(col::accent);
  String artist = fitText(info.artist.length() ? info.artist : String("未知艺术家"), tw, &fonts::DejaVu12, true);
  cv.drawString(artist, tx, 40);
  if (info.album.length())
  {
    cv.setTextColor(col::dim);
    String album = fitText(info.album, tw, &fonts::DejaVu9, true);
    cv.drawString(album, tx, 54);
  }

  // progress
  float el = player.elapsed();
  float du = info.duration;
  if (du > 0 && el > du) el = du;
  const int px = 8, py = 72, pw = 224;
  cv.fillRoundRect(px, py, pw, 4, 2, col::panel2);
  int fill = du > 0 ? (int)(pw * el / du) : 0;
  if (fill > 0) cv.fillRoundRect(px, py, fill, 4, 2, col::accent);
  if (du > 0) cv.fillCircle(px + fill, py + 1, 4, col::text);

  cv.setTextColor(col::dim);
  cv.setTextDatum(top_left);
  cv.setFont(&fonts::DejaVu9);
  cv.drawString(fmtTime(el), px, 81);
  cv.setTextDatum(top_right);
  cv.drawString(du > 0 ? fmtTime(du) : String("--:--"), px + pw, 81);
  cv.setTextDatum(top_center);
  char pos[16];
  snprintf(pos, sizeof(pos), "%d / %d", info.position, info.count);
  cv.drawString(pos, px + pw / 2, 81);

  // transport
  drawSkipIcon(88, 105, false);
  drawTransportIcon(W / 2, 105, player.state());
  drawSkipIcon(152, 105, true);

  // format, left
  cv.setTextDatum(top_left);
  cv.setTextColor(col::dim);
  String fmt;
  fmt = String(info.vbr ? "VBR " : "MP3 ") + info.bitrate + "k";
  useFace(fmt, &fonts::DejaVu9, true);
  cv.drawString(fmt, 8, 93);
  fmt = String(info.sampleRate / 1000.0f, 1) + "k ";
  fmt += (info.channels == 1) ? "单声道" : "立体声";
  useFace(fmt, &fonts::DejaVu9, true);
  cv.drawString(fmt, 8, 106);

  // EQ, right: three little bars around a centre line
  bool flat = !cfg.eq[0] && !cfg.eq[1] && !cfg.eq[2];
  cv.setTextDatum(top_right);
  String eqLabel = flat ? "均衡 平直" : "均衡";
  useFace(eqLabel, &fonts::DejaVu9, true);
  cv.drawString(eqLabel, 232, 93);
  if (!flat)
  {
    for (int b = 0; b < 3; ++b)
    {
      int x = 206 + b * 9, cy = 112;
      int hgt = cfg.eq[b] * 6 / ThreeBandEQ::kMaxDb;
      cv.drawFastHLine(x - 1, cy, 7, col::panel2);
      if (hgt > 0) cv.fillRect(x, cy - hgt, 5, hgt, col::accent);
      else if (hgt < 0) cv.fillRect(x, cy + 1, 5, -hgt, col::cool);
    }
  }

  drawHint("F 文件夹　E 均衡　R 循环　H 帮助　I 状态");
}

static int eqSel = 0;

// ---------------------------------------------------------------------------
// Help — every shortcut on one page, two columns. The footer hint on each page
// only names the few keys that fit there; H shows the whole map. H or ESC
// closes it and returns to wherever it was opened from.
// ---------------------------------------------------------------------------
// -- 定时关闭页：取消 / 15-150 分钟档位，首行顺带显示当前剩余 ----------------
static void drawSleepTimer()
{
  drawHeader("定时关闭");

  const int n = 1 + (int)(sizeof(kSleepOpts) / sizeof(kSleepOpts[0]));
  const int vis = kRows;                              // 5 rows, like every list
  int top = sleepSel >= vis ? sleepSel - (vis - 1) : 0;

  for (int i = top; i < n && i < top + vis; ++i)
  {
    int y = kListY + (i - top) * kRowH;
    bool cur = (i == sleepSel);
    if (cur) cv.fillRoundRect(2, y, 230, kRowH - 1, 3, col::panel2);
    cv.setTextDatum(middle_left);
    cv.setTextColor(cur ? col::text : col::dim);
    String label = (i == 0) ? String("关闭倒计时")
                            : String(kSleepOpts[i - 1]) + " 分钟";
    useFace(label, kFaceSmall);
    cv.drawString(label, 12, y + kRowH / 2);

    if (i == 0 && sleepMins)                          // active timer on row 0
    {
      cv.setTextDatum(middle_right);
      cv.setTextColor(col::warn);
      String left = sleepRemaining() + " 后关机";
      useFace(left, kFaceSmall);
      cv.drawString(left, 226, y + kRowH / 2);
    }
  }
  if (n - top > vis) scrollBar(n, top);
  drawHint("ENT 确认　BKSP 返回");
}

static void drawHelp()
{
  // No mode pills: header already carries the "H / ESC 关闭" label, which
  // collided with the "直播" pill while a station was streaming.
  drawHeader("快捷键", false);

  // Header mini-labels are 14 px efont when they contain CJK — two of them
  // collided with each other and with the battery cluster, so only one is
  // kept; "I" moved into the table itself.
  cv.setTextDatum(middle_left);
  cv.setTextColor(col::dim);
  cv.setFont(kFaceSmall);
  cv.drawString("H / ESC 关闭", 54, 7);

  cv.drawFastVLine(120, 16, 112, col::panel2);

  static const char* lKey[]  = { "空格", "N/P", "左/右", "上/下", "F", "E", "R", "S" };
  static const char* lText[] = { "播放/暂停", "切歌", "快退/快进", "音量", "文件夹", "均衡器", "循环模式", "随机播放" };
  static const char* rKey[]  = { "M", "O", "L", "TAB", "BKSP", "I", "T", "W/B" };
  static const char* rText[] = { "静音", "电台", "音乐库", "主界面", "上一级", "状态页", "定时关机", "配网/亮度" };

  cv.setFont(kFaceSmall);
  for (int i = 0; i < 8; ++i)
  {
    int y = 16 + i * 15;                 // 14 px font: 15 px pitch, 8 rows fill the screen
    cv.setTextDatum(top_right);
    cv.setTextColor(col::text);
    cv.drawString(lKey[i], 52, y);
    cv.setTextDatum(top_left);
    cv.setTextColor(col::dim);
    cv.drawString(lText[i], 56, y);

    cv.setTextDatum(top_right);
    cv.setTextColor(col::text);
    cv.drawString(rKey[i], 168, y);
    cv.setTextDatum(top_left);
    cv.setTextColor(col::dim);
    cv.drawString(rText[i], 172, y);
  }
}

/// 运行状态页（按 I 打开）：内存、WiFi、运行时长。
/// 这页是唯一显示剩余内存的地方 —— 不写 NVS，不占帮助页。
static void drawStatus()
{
  drawHeader("运行状态");

  uint32_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t minHeap = ESP.getMinFreeHeap();
  uint32_t big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  // Can we actually allocate from PSRAM? It may be present but not part of
  // the allocatable heap, in which case every request falls back to internal
  // RAM. Probe it instead of guessing.
  uint32_t psTot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  bool psUsable = false;
  {
    void* probe = heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (probe)
    {
      psUsable = esp_ptr_external_ram(probe);
      heap_caps_free(probe);
    }
  }

  char b[40];
  struct Row { const char* k; String v; };
  Row rows[9];
  int n = 0;

  snprintf(b, sizeof(b), "%u KB", (unsigned)(heap / 1024));
  rows[n].k = "内部剩余"; rows[n].v = b; ++n;
  snprintf(b, sizeof(b), "%u KB", (unsigned)(minHeap / 1024));
  rows[n].k = "内部最低"; rows[n].v = b; ++n;
  snprintf(b, sizeof(b), "%u KB", (unsigned)(big / 1024));
  rows[n].k = "最大连续块"; rows[n].v = b; ++n;

  // The frame buffer is the single biggest RAM consumer: 240x135 at 16 bpp is
  // 64.8 KB. esp_ptr_external_ram() is the reliable way to tell where a block
  // really lives — heap_caps_get_total_size() does not count PSRAM here.
  int depth  = (int)cv.getColorDepth() & (int)lgfx::color_depth_t::bit_mask;
  uint32_t cvKB = (uint32_t)(W * H * depth / 8) / 1024;
  {
    void* buf = cv.getBuffer();
    bool inPsram = buf && esp_ptr_external_ram(buf);
    snprintf(b, sizeof(b), "%u KB %s", (unsigned)cvKB, inPsram ? "PSRAM" : "内部");
    rows[n].k = "显示缓冲"; rows[n].v = b; ++n;
  }
  uint32_t mb = player.madBytes();
  if (mb)
  {
    snprintf(b, sizeof(b), "%u KB %s", (unsigned)(mb / 1024),
             player.madInPsram() ? "PSRAM" : "内部");
    rows[n].k = "解码缓冲"; rows[n].v = b; ++n;
  }

  uint32_t rb = player.radioRingBytes();
  if (rb)
  {
    snprintf(b, sizeof(b), "%u KB %s", (unsigned)(rb / 1024),
             player.radioRingInPsram() ? "PSRAM" : "内部");
    rows[n].k = "取流缓冲"; rows[n].v = b; ++n;
  }

  // Always shown, even when the total is zero: the point of this line is to
  // show whether PSRAM can be allocated at all. Skipped while the sleep
  // timer is running: with the decoder buffers listed too, 9 rows would
  // overflow the 135 px tall panel.
  if (!sleepMins)
  {
    snprintf(b, sizeof(b), "%u MB %s", (unsigned)(psTot / (1024 * 1024)),
             psUsable ? "可用" : (psramReady ? "不可用" : "未启用"));
    rows[n].k = "PSRAM"; rows[n].v = b; ++n;
  }

  rows[n].k = "WiFi";
  rows[n].v = net::connected() ? String(net::ssid()) : String("未连接");
  ++n;

  if (sleepMins)
  {
    rows[n].k = "定时关闭";
    rows[n].v = sleepRemaining() + " 后关机";
    ++n;
  }

  int y = 18;
  for (int i = 0; i < n; ++i)
  {
    cv.setTextDatum(middle_left);
    cv.setTextColor(col::dim);
    useFace(String(rows[i].k), &fonts::DejaVu12, true);
    cv.drawString(rows[i].k, 8, y);

    cv.setTextDatum(middle_right);
    uint16_t c = col::text;
    if (i == 0 && heap < 24 * 1024) c = col::warn;       // low memory
    if (i == 2 && big < 8 * 1024) c = col::warn;         // fragmented
    useFace(rows[i].v, &fonts::DejaVu12);
    cv.setTextColor(c);
    cv.drawString(rows[i].v, 232, y);
    y += 15;                     // 14 px font needs more than 14 px of pitch
  }
}

static void drawEqualizer()
{
  drawHeader("均衡器");
  static const char* names[3] = { "低频", "中频", "高频" };
  static const char* freqs[3] = { "200 Hz", "1 kHz", "4 kHz" };
  const int cx[3] = { 48, 120, 192 };
  const int top = 30, bottom = 90, zero = 60;   // 30 px = 12 dB

  for (int b = 0; b < 3; ++b)
  {
    bool s = (b == eqSel);
    int g = cfg.eq[b];
    if (s) cv.drawRoundRect(cx[b] - 32, 16, 64, 105, 6, col::panel2);

    char v[12];
    snprintf(v, sizeof(v), g > 0 ? "+%d dB" : "%d dB", g);
    cv.setFont(&fonts::DejaVu9);
    cv.setTextDatum(top_center);
    cv.setTextColor(s ? col::accent : col::dim);
    cv.drawString(v, cx[b], 18);

    cv.fillRoundRect(cx[b] - 3, top, 6, bottom - top + 1, 3, col::panel2);
    for (int d = -12; d <= 12; d += 6)
    {
      int ty = zero - d * (zero - top) / 12;
      int len = d == 0 ? 6 : 3;
      cv.drawFastHLine(cx[b] - 6 - len, ty, len, col::dim);
      cv.drawFastHLine(cx[b] + 7, ty, len, col::dim);
    }
    int vy = zero - g * (zero - top) / 12;
    if (vy != zero) cv.fillRect(cx[b] - 3, min(vy, zero), 6, abs(vy - zero) + 1, s ? col::accent : col::cool);
    cv.fillRoundRect(cx[b] - 11, vy - 3, 22, 7, 3, s ? col::accent : col::text);

    cv.setTextColor(s ? col::accent : col::text);
    String band(names[b]);
    useFace(band, &fonts::DejaVu12);
    cv.drawString(band, cx[b], 91);
    cv.setTextColor(col::dim);
    String fq(freqs[b]);
    useFace(fq, &fonts::DejaVu9, true);
    cv.drawString(fq, cx[b], 108);
  }
  drawHint(",/ 选频段  ;. 调整  0 归零  F 平直");
}

static void drawToast()
{
  if ((int32_t)(millis() - toastUntil) >= 0) return;

  // Size the box from the text: CJK glyphs are 16x16 px (efont), Latin ~8.
  // A fixed-width box made Chinese toasts overflow its border.
  int tw = 0;
  for (unsigned i = 0; i < toastText.length();)
  {
    uint8_t c = (uint8_t)toastText[i];
    if (c >= 0x80) { tw += 16; i += (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2; }
    else           { tw += 8;  ++i; }
  }
  int w = min(W - 8, max(150, tw + 28));
  int h = toastBar >= 0 ? 44 : 32;
  int x = (W - w) / 2, y = (H - h) / 2 + 6;
  cv.fillRoundRect(x, y, w, h, 6, col::panel);
  cv.drawRoundRect(x, y, w, h, 6, col::accent);
  cv.setTextDatum(middle_center);
  cv.setTextColor(col::text);
  useFace(toastText, &fonts::DejaVu12);
  cv.drawString(toastText, W / 2, y + (toastBar >= 0 ? (h - 12) / 2 : h / 2));
  if (toastBar >= 0)
  {
    int bw = w - 24;
    cv.fillRoundRect(x + 12, y + h - 11, bw, 4, 2, col::panel2);
    cv.fillRoundRect(x + 12, y + h - 11, bw * toastBar / 100, 4, 2, col::accent);
  }
}

static void drawTransition()
{
  cv.setTextDatum(middle_center);
  cv.setTextColor(col::text);
  useFace(switchText, &fonts::DejaVu12);
  cv.drawCenterString(fitText(switchText, 200, &fonts::DejaVu12), W / 2, 58);

  int n = (millis() / 350) % 4;
  String d;
  for (int i = 0; i < n; ++i) d += '.';
  cv.setTextColor(col::dim);
  useFace(d, kFaceSmall, true);
  cv.drawCenterString(d, W / 2, 84);
}

static void render()
{
  cv.fillScreen(col::bg);

  // While a source switch is in flight, hold the transition page instead of
  // flashing whatever the playing screen still shows.
  if (switchUntil)
  {
    bool done = (switchTarget == 0)
      ? (!info.radio && player.loaded())
      : (info.radio && player.loaded() && (!switchWant.length() || info.path == switchWant));
    if (done || (int32_t)(millis() - switchUntil) >= 0) switchUntil = 0;
    else { drawTransition(); drawToast(); cv.pushSprite(0, 0); return; }
  }

  switch (screen)
  {
    case Screen::Home:           drawHome(); break;
    case Screen::FolderPicker:   drawFolderPicker(); break;
    case Screen::NowPlaying:     drawNowPlaying(); break;
    case Screen::Equalizer:     drawEqualizer(); break;
    case Screen::Help:          drawHelp(); break;
    case Screen::Status:        drawStatus(); break;
    case Screen::RadioMenu:     drawRadioMenu(); break;
    case Screen::RadioStations: drawStationList(); break;
    case Screen::RadioSearch:   drawRadioSearch(); break;
    case Screen::WifiList:      drawWifiList(); break;
    case Screen::WifiPass:      drawWifiPass(); break;
    case Screen::SleepTimer:    drawSleepTimer(); break;
  }
  drawToast();
  cv.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
enum : uint8_t { K_UP = 0x80, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_BKSP, K_TAB, K_ESC };

static uint8_t normKey(char c)
{
  switch (c)
  {
    case ';': case ':': return K_UP;        // the arrow keys, with or without Fn
    case '.': case '>': return K_DOWN;
    case ',': case '<': return K_LEFT;
    case '/': case '?': return K_RIGHT;
    case '`': case '~': return K_ESC;
    case '+': return '=';
    case '_': return '-';
    case ')': return '0';
  }
  return (uint8_t)tolower((unsigned char)c);
}

static void readKeys(std::bitset<256>& now)
{
  auto& ks = M5Cardputer.Keyboard.keysState();
  for (char c : ks.word) now.set(normKey(c));
  if (ks.enter) now.set(K_ENTER);
  if (ks.backspace || ks.del) now.set(K_BKSP);
  if (ks.tab) now.set(K_TAB);
  if (ks.esc) now.set(K_ESC);
  if (ks.up) now.set(K_UP);
  if (ks.down) now.set(K_DOWN);
  if (ks.left) now.set(K_LEFT);
  if (ks.right) now.set(K_RIGHT);
}

static bool repeatable(uint8_t k)
{
  return k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT || k == '=' || k == '-';
}

static void changeVolume(int d)
{
  muted = false;
  cfg.volume = constrain(cfg.volume + d, 0, 100);
  applyVolume();
  toast(String("音量 ") + cfg.volume + "%", cfg.volume, 900);
  markDirty();
}

static void backFromEq() { screen = player.loaded() ? Screen::NowPlaying : Screen::Home; }

static void handlePassKey(uint8_t k, bool rep)
{
  if (k == K_ENTER && !rep)
  {
    if (!passBuf.length()) { toast("密码不能为空"); return; }   // stay here, it is editable
    net::connectTo(pendingSsid, passBuf);
    toast(String("连接 ") + pendingSsid);
    screen = Screen::WifiList;
  }
  else if (k == K_BKSP && !rep)
  {
    if (passBuf.length()) passBuf.remove(passBuf.length() - 1);
  }
  else if (k == K_ESC && !rep)
  {
    screen = Screen::WifiList;
  }
}

static void onKey(uint8_t k, bool rep)
{
  if (screen == Screen::WifiPass) { handlePassKey(k, rep); return; }

  // ---- everywhere ----
  switch (k)
  {
    case ' ':
      if (rep) return;
      player.togglePause();
      saveResume();
      return;
    case 'n': if (!rep) player.next(); return;
    case 'p': if (!rep) player.prev(); return;
    case '=': changeVolume(+5); return;
    case '-': changeVolume(-5); return;
    case 'm':
      if (rep) return;
      muted = !muted;
      applyVolume();
      toast(muted ? "已静音" : "取消静音");
      return;
    case 's':
      if (rep) return;
      cfg.shuffle = !cfg.shuffle;
      player.setShuffle(cfg.shuffle);
      toast(cfg.shuffle ? "随机播放 开" : "随机播放 关");
      markDirty();
      return;
    case 'r':
    {
      // R = cycle repeat mode everywhere, except: WiFi list rescans, and the
      // folder picker re-reads the card (repeat is pointless there).
      if (screen == Screen::WifiList || screen == Screen::FolderPicker) break;
      if (rep) return;
      cfg.repeat = (cfg.repeat + 1) % 3;
      player.setRepeat((RepeatMode)cfg.repeat);
      static const char* names[3] = { "不循环", "列表循环", "单曲循环" };
      toast(names[cfg.repeat]);
      markDirty();
      return;
    }
    case 'e':
      if (rep) return;
      if (screen == Screen::Equalizer) backFromEq(); else screen = Screen::Equalizer;
      return;
    case 'l': if (!rep) openMusic(); return;
    case 'h':
      if (rep) return;
      if (screen == Screen::Help) screen = helpBack;
      else { helpBack = screen; screen = Screen::Help; }
      return;
    case 'i':
      // Status page: free heap, WiFi, uptime. Deliberately its own page so
      // the help screen stays a clean key map.
      if (rep) return;
      if (screen == Screen::Status) screen = helpBack;
      else { helpBack = screen; screen = Screen::Status; }
      return;
    case 'o':
      if (rep) return;
      screen = Screen::RadioMenu;
      menuSel = 0;
      menuTop = 0;
      return;
    case K_TAB:
      if (rep) return;
      if (screen == Screen::NowPlaying) screen = Screen::Home;
      else if (screen != Screen::FolderPicker) screen = Screen::NowPlaying;
      return;
    case 'd': if (!rep) setScreen(false); return;
    case 'b':
      if (rep) return;
      cfg.brightness = (cfg.brightness + 1) % 4;
      setScreen(true);
      toast("亮度", kBrightness[cfg.brightness] * 100 / 255, 900);
      markDirty();
      return;
    case 't':                              // sleep timer page, from anywhere
      if (rep || screen == Screen::SleepTimer) return;
      timerBack = screen;
      sleepSel  = sleepMins ? 1 : 0;       // cursor: first duration when off
      screen    = Screen::SleepTimer;
      return;
  }

  // ---- per screen ----
  if (screen == Screen::Help)
  {
    if ((k == K_ESC || k == K_BKSP) && !rep) screen = helpBack;
  }
  else if (screen == Screen::Status)
  {
    if ((k == K_ESC || k == K_BKSP) && !rep) screen = helpBack;
  }
  else if (screen == Screen::Home)
  {
    switch (k)
    {
      case K_UP:
      case K_LEFT:  if (!rep) homeSel = 0; break;
      case K_DOWN:
      case K_RIGHT: if (!rep) homeSel = 1; break;
      case K_ENTER: if (!rep) openHome(homeSel); break;
      case 'f':                                   // folder picker straight from home
        if (!rep) { openFolderPicker(cfg.dir.length() ? cfg.dir : String("/")); screen = Screen::FolderPicker; }
        break;
      case 'w':
        if (rep) break;
        wifiBack = Screen::Home;
        screen = Screen::WifiList;
        startWifiScan();
        break;
    }
  }
  else if (screen == Screen::FolderPicker)
  {
    int n = pickRowCount();
    switch (k)
    {
      case K_UP:    if (n) { if (pickSel > 0) --pickSel; else if (!rep) pickSel = n - 1; } break;
      case K_DOWN:  if (n) { if (pickSel < n - 1) ++pickSel; else if (!rep) pickSel = 0; } break;
      case K_LEFT:  pickSel = max(0, pickSel - kRows); break;
      case K_RIGHT: pickSel = max(0, min(n - 1, pickSel + kRows)); break;
      case K_ENTER: if (!rep) pickActivate(); break;
      case 'r':     if (!rep) { openFolderPicker(pickDir); toast("重新读取存储卡"); } break;
      case K_BKSP:  if (!rep) pickGoUp(); break;
      case K_ESC:   if (!rep) screen = pickerBack; break;
    }
  }
  else if (screen == Screen::NowPlaying)
  {
    switch (k)
    {
      case K_UP:    changeVolume(+5); break;
      case K_DOWN:  changeVolume(-5); break;
      case K_LEFT:  player.seekBy(-10); break;
      case K_RIGHT: player.seekBy(+10); break;
      case K_ENTER: if (!rep) { player.togglePause(); saveResume(); } break;
      case 'f':                                   // pick another folder to play
        if (!rep) { openFolderPicker(cfg.dir.length() ? cfg.dir : String("/")); screen = Screen::FolderPicker; }
        break;
      case K_BKSP:
      case K_ESC:   if (!rep) screen = Screen::Home; break;
    }
  }
  else if (screen == Screen::SleepTimer)
  {
    const int n = 1 + (int)(sizeof(kSleepOpts) / sizeof(kSleepOpts[0]));
    switch (k)
    {
      case K_UP:   if (sleepSel > 0)     --sleepSel; break;
      case K_DOWN: if (sleepSel < n - 1) ++sleepSel; break;
      case K_ENTER:
        if (rep) break;
        if (sleepSel == 0)
        {
          if (sleepMins) { setSleepTimer(0); toast("已取消定时"); }
        }
        else
        {
          setSleepTimer(kSleepOpts[sleepSel - 1]);
          toast(String("定时 ") + kSleepOpts[sleepSel - 1] + " 分钟 · 到期睡眠关机", -1, 1600);
        }
        screen = timerBack;
        break;
      case K_BKSP:
      case K_ESC:  if (!rep) screen = timerBack; break;
    }
  }
  else if (screen == Screen::Equalizer)
  {
    switch (k)
    {
      case K_LEFT:  if (eqSel > 0) --eqSel; break;
      case K_RIGHT: if (eqSel < 2) ++eqSel; break;
      case K_UP:
      case K_DOWN:
        cfg.eq[eqSel] = constrain(cfg.eq[eqSel] + (k == K_UP ? 1 : -1), -ThreeBandEQ::kMaxDb, ThreeBandEQ::kMaxDb);
        applyEq();
        markDirty();
        break;
      case '0':
        if (rep) break;
        cfg.eq[eqSel] = 0;
        applyEq();
        markDirty();
        break;
      case 'f':
        if (rep) break;
        cfg.eq[0] = cfg.eq[1] = cfg.eq[2] = 0;
        applyEq();
        markDirty();
        toast("均衡器已平直");
        break;
      case K_ENTER:
      case K_BKSP:
      case K_ESC:   if (!rep) backFromEq(); break;
    }
  }
  else if (screen == Screen::RadioMenu)
  {
    int n = menuCount();
    switch (k)
    {
      case K_UP:    if (menuSel > 0) --menuSel; else if (!rep) menuSel = n - 1; break;
      case K_DOWN:  if (menuSel < n - 1) ++menuSel; else if (!rep) menuSel = 0; break;
      case K_LEFT:  menuSel = max(0, menuSel - kRows); break;
      case K_RIGHT: menuSel = min(n - 1, menuSel + kRows); break;
      case K_ENTER:
        if (rep) break;
        if (menuSel == 0)
        {
          if (!histCount) { toast("还没有最近收听"); break; }
          openStationList(-2);
        }
        else if (menuSel == 1)
        {
          resetSearchInput();
          screen = Screen::RadioSearch;
        }
        else if (menuSel == 2) openStationList(-1);
        else                   openStationList(menuSel - 3);
        break;
      case K_BKSP:
      case K_ESC:   if (!rep) screen = Screen::Home; break;
      case 'w':
        if (rep) break;
        wifiBack = Screen::RadioMenu;
        screen = Screen::WifiList;
        startWifiScan();
        break;
    }
  }
  else if (screen == Screen::RadioStations)
  {
    int n = radioItems.size();
    switch (k)
    {
      case K_UP:    if (n) { if (stationSel > 0) --stationSel; else if (!rep) stationSel = n - 1; } break;
      case K_DOWN:  if (n) { if (stationSel < n - 1) ++stationSel; else if (!rep) stationSel = 0; } break;
      case K_LEFT:  stationSel = max(0, stationSel - kRows); break;
      case K_RIGHT: stationSel = max(0, min(n - 1, stationSel + kRows)); break;
      case K_ENTER: if (!rep) playSelectedStation(); break;
      case K_BKSP:
      case K_ESC:
        if (rep) break;
        if (radioGroup == -3) screen = Screen::RadioSearch;   // 回搜索页，词还在
        else                  screen = Screen::RadioMenu;
        break;
      case 'w':
        if (rep) break;
        wifiBack = Screen::RadioStations;
        screen = Screen::WifiList;
        startWifiScan();
        break;
    }
  }
  else if (screen == Screen::WifiList)
  {
    switch (k)
    {
      case K_UP:    if (apCount) { if (apSel > 0) --apSel; else if (!rep) apSel = apCount - 1; } break;
      case K_DOWN:  if (apCount) { if (apSel < apCount - 1) ++apSel; else if (!rep) apSel = 0; } break;
      case K_ENTER: if (!rep) beginConnect(apSel); break;
      case 'r':     if (!rep) startWifiScan(); break;
      case 'x':
        if (rep || !apCount) break;
        if (net::isSaved(aps[apSel].ssid))
        {
          String s = aps[apSel].ssid;
          net::forgetSsid(s);
          toast(String("已忘记 ") + s);
        }
        else toast("该网络不在已存列表里");
        break;
      case K_BKSP:
      case K_ESC:   if (!rep) screen = wifiBack; break;
    }
  }
}

// Password entry reads the raw characters: ';' ',' '/' are legitimate in a
// password even though the same keys are the arrow pad everywhere else.
static void pollPassInput()
{
  auto& ks = M5Cardputer.Keyboard.keysState();

  std::bitset<128> now;
  bool rawEsc = false;
  for (char c : ks.word)
  {
    uint8_t u = (uint8_t)c;
    if (u < 32 || u >= 128) continue;
    // The Esc key arrives as '`' or '~' (see normKey); treat it as ESC here
    // too — otherwise it does nothing on this page and even gets typed into
    // the password.
    if (u == '`' || u == '~') { rawEsc = true; continue; }
    now.set(u);
  }
  for (int c = 32; c < 128; ++c)
    if (now[c] && !prevRawChars[c] && passBuf.length() < 63) passBuf += (char)c;
  prevRawChars = now;

  bool enter = ks.enter, bksp = ks.backspace || ks.del, esc = ks.esc || rawEsc;
  bool any = false;
  if (enter && !prevEnter) { any = true; handlePassKey(K_ENTER, false); }
  if (bksp && !prevBksp)   { any = true; handlePassKey(K_BKSP, false); }
  if (esc && !prevEsc)     { any = true; handlePassKey(K_ESC, false); }
  prevEnter = enter;
  prevBksp = bksp;
  prevEsc = esc;

  if (now.any() || any)
  {
    lastInput = millis();
    if (!screenOn) setScreen(true);
  }
}

// 搜索输入与密码输入同理：读原始字符，方向键位在搜索里就是普通字符。
static void pollSearchInput()
{
  auto& ks = M5Cardputer.Keyboard.keysState();

  std::bitset<128> now;
  bool rawEsc = false;
  for (char c : ks.word)
  {
    uint8_t u = (uint8_t)c;
    if (u < 32 || u >= 128) continue;
    if (u == '`' || u == '~') { rawEsc = true; continue; }
    now.set(u);
  }
  bool changed = false;
  for (int c = 32; c < 128; ++c)
    if (now[c] && !prevSearchChars[c] && searchQ.length() < 31)
    {
      searchQ += (char)c;
      changed = true;
    }
  prevSearchChars = now;

  bool enter = ks.enter, bksp = ks.backspace || ks.del, esc = ks.esc || rawEsc;
  if (bksp && !prevSearchBksp)
  {
    if (searchQ.length()) { searchQ.remove(searchQ.length() - 1); changed = true; }
    else screen = Screen::RadioMenu;            // 空词退格 = 离开搜索
  }
  if (esc && !prevSearchEsc) screen = Screen::RadioMenu;
  if (enter && !prevSearchEnter && searchHits.size()) openStationList(-3);
  if (changed) runSearch();
  prevSearchEnter = enter;
  prevSearchBksp  = bksp;
  prevSearchEsc   = esc;

  if (now.any() || enter || bksp || esc)
  {
    lastInput = millis();
    if (!screenOn) setScreen(true);
  }
}

static std::bitset<256> prevKeys;
static uint32_t pressedAt[256];
static uint32_t lastFire[256];

static void pollInput()
{
  if (screen == Screen::WifiPass)
  {
    pollPassInput();
    // The page is left on a key edge (ENT / ESC): re-sample the pad so that same
    // still-held key cannot fire again on the screen we just landed on.
    if (screen != Screen::WifiPass) readKeys(prevKeys);
    return;
  }
  if (screen == Screen::RadioSearch)
  {
    pollSearchInput();
    if (screen != Screen::RadioSearch) readKeys(prevKeys);
    return;
  }

  std::bitset<256> now;
  readKeys(now);
  uint32_t t = millis();
  bool any = false;
  for (int k = 0; k < 256; ++k)
  {
    if (!now[k]) continue;
    if (!prevKeys[k])
    {
      pressedAt[k] = lastFire[k] = t;
      any = true;
      if (screenOn) onKey(k, false);
    }
    else if (screenOn && repeatable(k) && t - pressedAt[k] > 400 && t - lastFire[k] >= 75)
    {
      lastFire[k] = t;
      onKey(k, true);
    }
  }
  if (any)
  {
    lastInput = t;
    if (!screenOn) setScreen(true);   // first key only wakes the screen
  }
  prevKeys = now;
}

// ---------------------------------------------------------------------------
// SD card
// ---------------------------------------------------------------------------
static bool mountSD()
{
  int sck  = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  int miso = M5.getPin(m5::pin_name_t::sd_spi_miso);
  int mosi = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  int cs   = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (sck < 0 || sck > 48)   sck = 40;    // Cardputer / ADV wiring
  if (miso < 0 || miso > 48) miso = 39;
  if (mosi < 0 || mosi > 48) mosi = 14;
  if (cs < 0 || cs > 48)     cs = 12;
  SPI.begin(sck, miso, mosi, cs);
  if (SD.begin(cs, SPI, 25000000)) return true;
  SD.end();
  return SD.begin(cs, SPI, 10000000);
}

#include <esp_system.h>

/// Crash class of the previous boot, or nullptr for a clean power-on.
static const char* lastResetReason()
{
  switch (esp_reset_reason())
  {
    case ESP_RST_PANIC:    return "上次启动：程序异常崩溃";      // abort / exception
    case ESP_RST_TASK_WDT: return "上次启动：任务看门狗超时";
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:      return "上次启动：硬件看门狗超时";
    case ESP_RST_BROWNOUT: return "上次启动：供电电压不足";
    default:               return nullptr;
  }
}

static void splash(const char* a, const char* b)
{
  cv.fillScreen(col::bg);
  cv.setFont(&fonts::FreeSansBold9pt7b);
  cv.setTextDatum(middle_center);
  cv.setTextColor(col::accent);
  cv.drawString("CardRadio v1.1", W / 2, 34);
  drawCentered(a, b, 70);
  cv.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Arduino
// ---------------------------------------------------------------------------
void setup()
{
  auto m5cfg = M5.config();
  M5Cardputer.begin(m5cfg, true);
  M5Cardputer.Display.setRotation(1);

  // Put the PSRAM into the allocatable heap before anything big is allocated.
  // Without this the chip may well have PSRAM (the heap can even report its
  // size) but every heap_caps_malloc(MALLOC_CAP_SPIRAM) falls back to
  // internal RAM — which is where the 64 KB frame buffer and the 29 KB MP3
  // decoder would rather not be. Returns false when there is no PSRAM, and
  // then everything simply stays in internal RAM as before.
  psramReady = psramAddToHeap();

  // The frame buffer is the single biggest RAM consumer: 240x135 at 16 bpp is
  // 64.8 KB, and this board has no PSRAM to put it in.
  //
  // 8 bpp was tried (rgb332, which needs no palette) and it saved the 31.5 KB,
  // but the colours came out green on the panel — so it stays at 16 bpp.
  cv.setColorDepth(16);
  if (!cv.createSprite(W, H)) { cv.setColorDepth(8); cv.createSprite(W, H); }

  loadSettings();
  setScreen(true);

  M5.Speaker.begin();
  applyVolume();

  // Crash diagnostics first: if the previous boot died, say so and hold the
  // screen long enough to read. Splash otherwise stays up ~1.2 s.
  const char* rst = lastResetReason();
  splash("正在挂载存储卡...", rst);
  delay(rst ? 2500 : 1200);
  while (!mountSD())
  {
    splash("没有 microSD 卡", "请插入 FAT32 / exFAT 的卡");
    delay(1000);
  }

  if (!player.begin(&SD))
  {
    splash("音频启动失败", "内存不足？");
    for (;;) delay(1000);
  }
  player.setShuffle(cfg.shuffle);
  player.setRepeat((RepeatMode)cfg.repeat);
  applyEq();

  net::begin();          // saved network, connects in the background

  batteryLevel = M5.Power.getBatteryLevel();
  lastInput = millis();
}

void loop()
{
  M5Cardputer.update();

  if (M5Cardputer.BtnA.wasPressed())   // G0 button: play / pause, screen on or off
  {
    lastInput = millis();
    player.togglePause();
    saveResume();
  }
  pollInput();

  net::loop();

  if (scanning)
  {
    int n = net::scanPoll(aps, 12);
    if (n >= 0)
    {
      apCount = n;
      scanning = false;
      if (apSel >= apCount) apSel = 0;
    }
  }

  static bool lastWifiUp = false;
  bool wifiUp = net::connected();
  if (wifiUp && !lastWifiUp && screen == Screen::WifiList)
  {
    toast("WiFi 已连接");
    screen = wifiBack;
  }
  lastWifiUp = wifiUp;

  uint32_t v = player.infoVersion();
  if (v != infoVer) { infoVer = v; player.getInfo(info); }

  uint32_t now = millis();
  if (screenOn && player.state() == PlayState::Playing && now - lastInput > kAutoOffMs) setScreen(false);

  // Remember the spot: when leaving the playing page, and now and then while
  // playing, so a power-off never loses more than ~20 s.
  static Screen lastScreen = Screen::Home;
  static uint32_t resumeDue = 0;
  if (screen != lastScreen) { if (lastScreen == Screen::NowPlaying) saveResume(); lastScreen = screen; }
  if ((int32_t)(now - resumeDue) >= 0) { resumeDue = now + 20000; saveResume(); }

  static uint32_t lastBatt = 0;
  if (now - lastBatt > 5000) { lastBatt = now; batteryLevel = M5.Power.getBatteryLevel(); }

  if (saveDue && (int32_t)(now - saveDue) >= 0) { saveDue = 0; saveSettings(); }

  // Sleep timer fired: stop playback, drop WiFi, deep sleep. Never returns.
  if (sleepDue && (int32_t)(now - sleepDue) >= 0) doSleepShutdown();

  static uint32_t lastDraw = 0;
  if (screenOn && now - lastDraw >= 40) { lastDraw = now; render(); }

  delay(5);
}
