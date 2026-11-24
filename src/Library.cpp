#include "Library.h"
#include "Mp3Info.h"
#include <algorithm>
#include <ctype.h>
#include <errno.h>

namespace Library
{

// Failure detail of the last list() call, shown on the error screen so a
// reproducible "cannot read card" tells us which layer answered wrong.
static Diag s_diag { 0, 0 };

Diag diag() { return s_diag; }

bool isMp3(const String& name)
{
  size_t n = name.length();
  if (n < 5) return false;
  const char* e = name.c_str() + n - 4;
  return e[0] == '.' && tolower(e[1]) == 'm' && tolower(e[2]) == 'p' && e[3] == '3';
}

String join(const String& dir, const String& name)
{
  if (dir.endsWith("/")) return dir + name;
  return dir + "/" + name;
}

String parent(const String& path)
{
  int i = path.lastIndexOf('/');
  if (i <= 0) return "/";
  return path.substring(0, i);
}

String baseName(const String& path)
{
  int i = path.lastIndexOf('/');
  return i < 0 ? path : path.substring(i + 1);
}

/// Case-insensitive compare where digit runs compare by value.
static int naturalCompare(const char* a, const char* b)
{
  while (*a && *b)
  {
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b))
    {
      while (*a == '0') ++a;
      while (*b == '0') ++b;
      const char* sa = a; const char* sb = b;
      while (isdigit((unsigned char)*a)) ++a;
      while (isdigit((unsigned char)*b)) ++b;
      int la = a - sa, lb = b - sb;
      if (la != lb) return la - lb;
      int c = strncmp(sa, sb, la);
      if (c) return c;
      continue;
    }
    int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
    if (ca != cb) return ca - cb;
    ++a; ++b;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

bool list(fs::FS& fs, const String& path, std::vector<LibEntry>& out, bool& truncated)
{
  out.clear();
  truncated = false;
  s_diag = { 0, 0 };
  File dir = fs.open(path);
  if (!dir || !dir.isDirectory())
  {
    s_diag = { 1, errno };
    if (dir) dir.close();
    return false;
  }

  for (;;)
  {
    bool isDir = false;
    String full = dir.getNextFileName(&isDir);   // no per-file open: much faster
    if (full.isEmpty())
    {
      s_diag = { 2, errno };   // end-of-dir — or readdir answering empty too soon
      break;
    }
    String name = baseName(full);
    if (name.isEmpty() || name[0] == '.') continue;
    if (isDir && name.equalsIgnoreCase("System Volume Information")) continue;
    if (!isDir && !isMp3(name)) continue;
    if (out.size() >= kMaxEntries) { truncated = true; break; }
    // Growing the vector doubles its block. With a radio stream holding its
    // own memory that allocation can fail and abort the whole app — so only
    // guard the *growth* step, and in small chunks. Listing entries themselves
    // is a few hundred bytes and must always work, otherwise the library
    // would appear empty whenever a radio stream is active.
    bool needGrow = (out.capacity() == out.size());
    if (needGrow && ESP.getFreeHeap() < 24 * 1024) { truncated = true; break; }
    if (needGrow) out.reserve(out.size() + 64);   // small blocks, fragmentation-friendly
    out.push_back({ name, foldUtf8(name), isDir });
  }
  dir.close();

  std::sort(out.begin(), out.end(), [](const LibEntry& x, const LibEntry& y) {
    if (x.dir != y.dir) return x.dir;
    return naturalCompare(x.label.c_str(), y.label.c_str()) < 0;
  });
  return true;
}

} // namespace Library
