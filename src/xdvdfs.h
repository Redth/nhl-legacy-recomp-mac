// Minimal XDVDFS (Xbox / Xbox 360 XGD) reader.
//
// Xbox 360 discs are not ISO9660/UDF, so Finder, hdiutil and 7-Zip cannot open
// them: the filesystem is XDVDFS - a volume descriptor at sector 32 of the game
// partition, then directory tables holding a binary search tree of entries.
//
// This is the C++ counterpart of tools/macos/xdvdfs_extract.py, so the in-app
// setup flow does not depend on Python or on the repo layout being present.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nhl::xdvdfs {

struct Entry {
  std::string path;  // '/'-separated, relative to the disc root
  uint32_t sector = 0;
  uint32_t size = 0;
  bool is_dir = false;
};

// Reads the directory tree. Returns false and fills `error` if the file is not
// an Xbox/Xbox 360 disc image.
bool ListImage(const std::filesystem::path& iso, std::vector<Entry>& entries_out,
               std::string& error);

// Extracts every file to `out_dir`, skipping any top-level directory in
// `skip_dirs` and any file already present at the right size (so an interrupted
// run resumes cheaply).
//
// `progress(done_bytes, total_bytes)` is called as data lands; return false from
// it to cancel, which makes this return false with an empty `error`.
bool ExtractImage(const std::filesystem::path& iso, const std::filesystem::path& out_dir,
                  const std::vector<std::string>& skip_dirs,
                  const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error);

}  // namespace nhl::xdvdfs
