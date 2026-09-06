// See xdvdfs.h.

#include "xdvdfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace nhl::xdvdfs {

namespace {

constexpr uint64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMagicLen = 20;
constexpr uint8_t kAttrDir = 0x10;

// Game-partition byte offsets by disc generation: original Xbox, XGD3, XGD2,
// XGD1. Probed in order; the volume magic confirms which one is right.
constexpr uint64_t kBases[] = {0x00000000ull, 0x02080000ull, 0x0FD90000ull, 0x18300000ull};

struct File {
  explicit File(const std::filesystem::path& p) : f(std::fopen(p.string().c_str(), "rb")) {}
  ~File() {
    if (f) std::fclose(f);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  bool Seek(uint64_t off) const { return f && std::fseek(f, long(off), SEEK_SET) == 0; }
  size_t Read(void* dst, size_t n) const { return f ? std::fread(dst, 1, n, f) : 0; }
  std::FILE* f = nullptr;
};

bool FindBase(const File& f, uint64_t& base_out) {
  char magic[kMagicLen];
  for (uint64_t base : kBases) {
    if (!f.Seek(base + 32 * kSector)) continue;
    if (f.Read(magic, kMagicLen) != kMagicLen) continue;
    if (std::memcmp(magic, kMagic, kMagicLen) == 0) {
      base_out = base;
      return true;
    }
  }
  return false;
}

template <typename T>
T ReadLE(const uint8_t* p) {
  T v = 0;
  for (size_t i = 0; i < sizeof(T); ++i) v |= T(p[i]) << (8 * i);
  return v;
}

// Walks one directory table. The table is a binary search tree keyed by name;
// entries hold 16-bit left/right offsets in units of 4 bytes, 0xFFFF for "none".
void WalkDir(const File& f, uint64_t base, uint32_t sector, uint32_t size, const std::string& prefix,
             std::vector<Entry>& out, int depth) {
  if (size == 0 || depth > 32) return;
  std::vector<uint8_t> table(size);
  if (!f.Seek(base + uint64_t(sector) * kSector)) return;
  if (f.Read(table.data(), size) != size) return;

  std::vector<uint32_t> stack{0};
  std::unordered_set<uint32_t> seen;
  std::vector<Entry> dirs;
  while (!stack.empty()) {
    const uint32_t off = stack.back();
    stack.pop_back();
    // 0xFFFF terminates a branch; the rest guards malformed or looping tables.
    if (off == 0xFFFF || uint64_t(off) * 4 >= table.size() || !seen.insert(off).second) continue;
    const size_t p = size_t(off) * 4;
    if (p + 14 > table.size()) continue;
    const uint16_t left = ReadLE<uint16_t>(&table[p]);
    const uint16_t right = ReadLE<uint16_t>(&table[p + 2]);
    const uint32_t start = ReadLE<uint32_t>(&table[p + 4]);
    const uint32_t fsize = ReadLE<uint32_t>(&table[p + 8]);
    const uint8_t attrs = table[p + 12];
    const uint8_t nlen = table[p + 13];
    if (p + 14 + nlen > table.size() || nlen == 0) continue;
    std::string name(reinterpret_cast<const char*>(&table[p + 14]), nlen);
    stack.push_back(left);
    stack.push_back(right);
    Entry e;
    e.path = prefix + name;
    e.sector = start;
    e.size = fsize;
    e.is_dir = (attrs & kAttrDir) != 0;
    out.push_back(e);
    if (e.is_dir) dirs.push_back(e);
  }
  // Recurse after the table is parsed so `table` is not held across the calls.
  for (const Entry& d : dirs) {
    WalkDir(f, base, d.sector, d.size, d.path + "/", out, depth + 1);
  }
}

}  // namespace

bool ListImage(const std::filesystem::path& iso, std::vector<Entry>& entries_out,
               std::string& error) {
  File f(iso);
  if (!f.f) {
    error = "cannot open " + iso.string();
    return false;
  }
  uint64_t base = 0;
  if (!FindBase(f, base)) {
    error =
        "not an Xbox/Xbox 360 disc image. Xbox 360 discs use XDVDFS - a plain "
        "ISO9660 rip made in a PC drive will not work.";
    return false;
  }
  uint8_t header[8];
  if (!f.Seek(base + 32 * kSector + kMagicLen) || f.Read(header, 8) != 8) {
    error = "unreadable volume descriptor";
    return false;
  }
  const uint32_t root_sector = ReadLE<uint32_t>(&header[0]);
  const uint32_t root_size = ReadLE<uint32_t>(&header[4]);
  entries_out.clear();
  WalkDir(f, base, root_sector, root_size, "", entries_out, 0);
  if (entries_out.empty()) {
    error = "the disc image contains no files";
    return false;
  }
  return true;
}

bool ExtractImage(const std::filesystem::path& iso, const std::filesystem::path& out_dir,
                  const std::vector<std::string>& skip_dirs,
                  const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error) {
  std::vector<Entry> entries;
  if (!ListImage(iso, entries, error)) return false;

  auto skipped = [&](const std::string& path) {
    const std::string top = path.substr(0, path.find('/'));
    return std::any_of(skip_dirs.begin(), skip_dirs.end(), [&](const std::string& s) {
      return top.size() == s.size() &&
             std::equal(top.begin(), top.end(), s.begin(),
                        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    });
  };

  std::vector<Entry> files;
  uint64_t total = 0;
  for (const Entry& e : entries) {
    if (e.is_dir || skipped(e.path)) continue;
    files.push_back(e);
    total += e.size;
  }
  std::sort(files.begin(), files.end(),
            [](const Entry& a, const Entry& b) { return a.path < b.path; });

  File f(iso);
  if (!f.f) {
    error = "cannot open " + iso.string();
    return false;
  }
  uint64_t base = 0;
  if (!FindBase(f, base)) {
    error = "not an Xbox/Xbox 360 disc image";
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  std::vector<uint8_t> buffer(8u << 20);
  uint64_t done = 0;
  for (const Entry& e : files) {
    std::filesystem::path dest = out_dir;
    for (size_t start = 0, slash; start <= e.path.size(); start = slash + 1) {
      slash = e.path.find('/', start);
      dest /= e.path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
      if (slash == std::string::npos) break;
    }
    std::filesystem::create_directories(dest.parent_path(), ec);

    // Resume: an existing file of the right size is assumed good.
    if (std::filesystem::exists(dest, ec) && std::filesystem::file_size(dest, ec) == e.size) {
      done += e.size;
      if (progress && !progress(done, total)) return false;
      continue;
    }

    if (!f.Seek(base + uint64_t(e.sector) * kSector)) {
      error = "seek failed reading /" + e.path;
      return false;
    }
    std::FILE* o = std::fopen(dest.string().c_str(), "wb");
    if (!o) {
      error = "cannot write " + dest.string();
      return false;
    }
    uint64_t remaining = e.size;
    while (remaining) {
      const size_t want = size_t(std::min<uint64_t>(remaining, buffer.size()));
      const size_t got = f.Read(buffer.data(), want);
      if (got == 0) {
        std::fclose(o);
        error = "unexpected end of image while reading /" + e.path;
        return false;
      }
      if (std::fwrite(buffer.data(), 1, got, o) != got) {
        std::fclose(o);
        error = "write failed (disk full?) at " + dest.string();
        return false;
      }
      remaining -= got;
      done += got;
      if (progress && !progress(done, total)) {
        std::fclose(o);
        return false;
      }
    }
    std::fclose(o);
  }
  return true;
}

}  // namespace nhl::xdvdfs
