// VP6 host-decode bridge (env NHL_VP6_BRIDGE=1).
//
// The recompiled VP6 decoder has an unfixed arithmetic bug in its VMX128
// float IDCT (docs/vp6-fork-investigation.md): high-AC blocks decode with
// columns crushed to zero (green/black striping). Rather than patch the
// generated math, this bridge replaces the DECODED PIXELS at the movie
// player's publish seam with frames decoded on the host by ffmpeg:
//
//   input : the SDK's NtCreateFile exports the last opened *.vp6 guest path
//           in the NHL_VP6_LAST_OPEN process env var; the movie-setup hook
//           (sub_8277ABB8) maps it to a host file under --game_data_root.
//   decode: ffmpeg.exe (PATH or NHL_VP6_FFMPEG) is spawned per movie with
//           "-f rawvideo -pix_fmt yuv420p pipe:1"; a reader thread buffers
//           the raw frames (bounded).
//   output: the plane-publish hook (sub_8277CC98, see diag_hooks.cpp) calls
//           Vp6BridgePublish once per published frame; we overwrite the ring
//           slot's Y/U/V planes with the next ffmpeg frame. Rows are written
//           bottom-up (the ring renders vertically flipped - verified with a
//           gradient test pattern). On stream EOF the process is respawned
//           from frame 0 so looping menu movies keep playing.
//
// Audio is untouched (the guest EA-audio path has always been correct); sync
// is inherently guest-paced because we only swap pixels at publish time.

#include "vp6_bridge.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

bool BridgeOn() {
  static const bool on = [] {
    const char* e = std::getenv("NHL_VP6_BRIDGE");
    return e && *e && *e != '0';
  }();
  return on;
}

void BridgeLog(const char* fmt, ...) {
  FILE* f = std::fopen("vp6_bridge.txt", "a");
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fputc('\n', f);
  std::fclose(f);
}

// ---- movie decode state ------------------------------------------------------

struct BridgeState {
  std::mutex m;
  std::string host_path;   // current movie file on host
  HANDLE proc = nullptr;   // ffmpeg child
  HANDLE pipe_rd = nullptr;
  std::thread reader;
  std::deque<uint8_t> buf;  // raw yuv420p byte stream from ffmpeg
  std::atomic<bool> eof{false};
  std::atomic<bool> reader_stop{false};
  uint64_t frames_served = 0;
};

BridgeState& S() {
  static BridgeState s;
  return s;
}

constexpr size_t kMaxBuffered = 64ull << 20;  // 64MB ~ 46 frames of 720p

void ReaderThread(HANDLE pipe_rd) {
  std::vector<uint8_t> chunk(1 << 20);
  for (;;) {
    if (S().reader_stop.load()) break;
    {
      std::lock_guard<std::mutex> lk(S().m);
      if (S().buf.size() > kMaxBuffered) {
        // Backpressure: publish side drains at movie fps.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
    }
    DWORD got = 0;
    if (!ReadFile(pipe_rd, chunk.data(), DWORD(chunk.size()), &got, nullptr) ||
        got == 0) {
      S().eof.store(true);
      break;
    }
    std::lock_guard<std::mutex> lk(S().m);
    S().buf.insert(S().buf.end(), chunk.data(), chunk.data() + got);
  }
}

void StopMovie() {
  BridgeState& s = S();
  s.reader_stop.store(true);
  if (s.pipe_rd) CancelIoEx(s.pipe_rd, nullptr);
  if (s.reader.joinable()) s.reader.join();
  if (s.pipe_rd) CloseHandle(s.pipe_rd);
  if (s.proc) {
    TerminateProcess(s.proc, 0);
    CloseHandle(s.proc);
  }
  s.pipe_rd = nullptr;
  s.proc = nullptr;
  s.reader_stop.store(false);
  s.eof.store(false);
  std::lock_guard<std::mutex> lk(s.m);
  s.buf.clear();
}

std::string FfmpegPath() {
  if (const char* e = std::getenv("NHL_VP6_FFMPEG"); e && *e) return e;
  return "ffmpeg.exe";  // PATH
}

bool StartMovie(const std::string& host_path) {
  BridgeState& s = S();
  StopMovie();
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 8 << 20)) return false;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  std::string cmd = "\"" + FfmpegPath() +
                    "\" -v error -i \"" + host_path +
                    "\" -map 0:v:0 -f rawvideo -pix_fmt yuv420p pipe:1";
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  std::vector<char> cmdbuf(cmd.begin(), cmd.end());
  cmdbuf.push_back(0);
  BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    BridgeLog("spawn FAILED err=%lu cmd=%s", GetLastError(), cmd.c_str());
    return false;
  }
  CloseHandle(pi.hThread);
  s.proc = pi.hProcess;
  s.pipe_rd = rd;
  s.host_path = host_path;
  s.frames_served = 0;
  s.reader = std::thread(ReaderThread, rd);
  BridgeLog("spawned ffmpeg for %s", host_path.c_str());
  return true;
}

// Map a guest VFS path (game:\..., cache:\..., update:\...) to a host file
// under --game_data_root (parsed from the command line).
std::string GameDataRoot() {
  static const std::string root = [] {
    std::string cl = GetCommandLineA();
    const char* key = "--game_data_root";
    size_t p = cl.find(key);
    if (p == std::string::npos) return std::string();
    p += std::strlen(key);
    while (p < cl.size() && (cl[p] == ' ' || cl[p] == '=')) ++p;
    std::string v;
    if (p < cl.size() && cl[p] == '"') {
      size_t q = cl.find('"', p + 1);
      v = cl.substr(p + 1, q - p - 1);
    } else {
      size_t q = cl.find(' ', p);
      v = cl.substr(p, q - p);
    }
    return v;
  }();
  return root;
}

std::string MapGuestPath(std::string g) {
  for (auto& c : g)
    if (c == '/') c = '\\';
  auto strip = [&](const char* pfx) -> bool {
    size_t n = std::strlen(pfx);
    if (g.size() > n && _strnicmp(g.c_str(), pfx, n) == 0) {
      g = g.substr(n);
      return true;
    }
    return false;
  };
  std::string root = GameDataRoot();
  if (root.empty()) return {};
  if (strip("game:\\")) return root + "\\" + g;
  // cache:\X overlays game_data_root\_compiled\X (loose-asset tree).
  if (strip("cache:\\")) return root + "\\_compiled\\" + g;
  if (strip("update:\\")) return root + "\\" + g;
  // Raw device path - try as-is under the root.
  return root + "\\" + g;
}

}  // namespace

// Start (or switch) the decoder to whatever .vp6 the guest opened last. The
// file open happens asynchronously AFTER the movie-setup call, so this runs
// lazily from the publish hook too.
static void MaybeStartFromEnv(const char* who) {
  std::string host;
  char hostbuf[512] = {};
  DWORD hn = GetEnvironmentVariableA("NHL_VP6_LAST_OPEN_HOST", hostbuf,
                                     sizeof(hostbuf));
  if (hn && hn < sizeof(hostbuf)) {
    host = hostbuf;  // loose-tree open: already a host path
  } else {
    char guest[512] = {};
    DWORD n =
        GetEnvironmentVariableA("NHL_VP6_LAST_OPEN", guest, sizeof(guest));
    if (!n || n >= sizeof(guest)) return;
    host = MapGuestPath(guest);
  }
  if (host == S().host_path && S().proc) return;  // already decoding it
  DWORD attrs = GetFileAttributesA(host.c_str());
  BridgeLog("%s: host='%s' exists=%d", who, host.c_str(),
            attrs != INVALID_FILE_ATTRIBUTES);
  if (attrs == INVALID_FILE_ATTRIBUTES) return;
  StartMovie(host);
}

void Vp6BridgeOnMovieSetup() {
  if (!BridgeOn()) return;
  MaybeStartFromEnv("setup");
}

void Vp6BridgePublish(uint8_t* y, uint8_t* u, uint8_t* v, uint32_t w,
                      uint32_t h, uint32_t y_pitch, uint32_t c_pitch) {
  if (!BridgeOn()) return;
  MaybeStartFromEnv("publish");
  BridgeState& s = S();
  if (!s.proc) return;
  const size_t ysz = size_t(w) * h;
  const size_t csz = size_t(w / 2) * (h / 2);
  const size_t fsz = ysz + 2 * csz;
  std::vector<uint8_t> frame;
  {
    std::lock_guard<std::mutex> lk(s.m);
    if (s.buf.size() >= fsz) {
      frame.assign(s.buf.begin(), s.buf.begin() + fsz);
      s.buf.erase(s.buf.begin(), s.buf.begin() + fsz);
    }
  }
  if (frame.empty()) {
    if (s.eof.load()) {
      // Movie looped (menu attract) - restart the decode from frame 0.
      std::string path = s.host_path;
      uint64_t served = s.frames_served;
      BridgeLog("eof after %llu frames - respawning %s",
                (unsigned long long)served, path.c_str());
      StartMovie(path);
    }
    return;  // underrun: keep the guest's own frame this once
  }
  // The ring renders vertically flipped relative to write order: write frame
  // row r into plane row (h-1-r).
  const uint8_t* fy = frame.data();
  const uint8_t* fu = frame.data() + ysz;
  const uint8_t* fv = frame.data() + ysz + csz;
  if (y) {
    for (uint32_t r = 0; r < h; ++r)
      std::memcpy(y + size_t(h - 1 - r) * y_pitch, fy + size_t(r) * w, w);
  }
  const uint32_t cw = w / 2, ch = h / 2;
  if (u) {
    for (uint32_t r = 0; r < ch; ++r)
      std::memcpy(u + size_t(ch - 1 - r) * c_pitch, fu + size_t(r) * cw, cw);
  }
  if (v) {
    for (uint32_t r = 0; r < ch; ++r)
      std::memcpy(v + size_t(ch - 1 - r) * c_pitch, fv + size_t(r) * cw, cw);
  }
  ++s.frames_served;
}
