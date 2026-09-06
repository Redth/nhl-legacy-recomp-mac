// See game_setup.h.

#include "game_setup.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "xdvdfs.h"

namespace nhl {

namespace {

namespace fs = std::filesystem;

constexpr const char* kTitle = "NHL Legacy";
// Where the chosen location is remembered, so setup runs only once.
constexpr const char* kRememberFile = "nhl_game_path.txt";

bool HasGameData(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(dir / "default.xex", ec) &&
         fs::file_size(dir / "default.xex", ec) > 0;
}

// Tolerate being pointed one level high (e.g. at a folder holding the game
// folder, which is what a lot of dumps unzip into).
fs::path ResolveGameDir(const fs::path& picked) {
  if (HasGameData(picked)) return picked;
  std::error_code ec;
  fs::path found;
  int hits = 0;
  for (const auto& e : fs::directory_iterator(picked, ec)) {
    if (e.is_directory(ec) && HasGameData(e.path())) {
      found = e.path();
      ++hits;
    }
  }
  return hits == 1 ? found : fs::path();
}

fs::path RememberPath() { return rex::filesystem::GetExecutableFolder() / kRememberFile; }

void Remember(const fs::path& dir) {
  std::ofstream f(RememberPath(), std::ios::trunc);
  if (f) f << dir.string();
}

fs::path Recall() {
  std::ifstream f(RememberPath());
  if (!f) return {};
  std::string line;
  std::getline(f, line);
  return line.empty() ? fs::path() : fs::path(line);
}

void Message(SDL_MessageBoxFlags flags, const char* title, const std::string& text) {
  SDL_ShowSimpleMessageBox(flags, title, text.c_str(), nullptr);
}

// Returns the index of the pressed button, or -1.
int Ask(const std::string& text, const std::vector<const char*>& buttons) {
  std::vector<SDL_MessageBoxButtonData> data;
  for (int i = 0; i < int(buttons.size()); ++i) {
    SDL_MessageBoxButtonData b = {};
    // Last button is the escape/cancel default.
    b.flags = (i == int(buttons.size()) - 1) ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT
                                             : (i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0);
    b.buttonID = i;
    b.text = buttons[size_t(i)];
    data.push_back(b);
  }
  SDL_MessageBoxData box = {};
  box.flags = SDL_MESSAGEBOX_INFORMATION;
  box.title = kTitle;
  box.message = text.c_str();
  box.numbuttons = int(data.size());
  box.buttons = data.data();
  int pressed = -1;
  if (!SDL_ShowMessageBox(&box, &pressed)) return -1;
  return pressed;
}

struct PickResult {
  std::atomic<bool> done{false};
  std::string path;
};

void SDLCALL OnPicked(void* userdata, const char* const* filelist, int /*filter*/) {
  auto* result = static_cast<PickResult*>(userdata);
  if (filelist && filelist[0]) result->path = filelist[0];
  result->done.store(true);
}

// Runs a native picker and pumps the event loop until the callback fires.
std::string Pick(bool folder) {
  PickResult result;
  if (folder) {
    SDL_ShowOpenFolderDialog(OnPicked, &result, nullptr, nullptr, false);
  } else {
    static const SDL_DialogFileFilter kFilters[] = {
        {"Disc image (.iso)", "iso"},
        {"Compressed dump (.7z, .zip)", "7z;zip"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(OnPicked, &result, nullptr, kFilters, 3, nullptr, false);
  }
  while (!result.done.load()) {
    SDL_PumpEvents();
    SDL_Delay(10);
  }
  return result.path;
}

// A small window with a progress bar. Unpacking ~6 GB can take minutes on a
// slow drive, so silence is not an option, and the real UI does not exist yet
// at this point in startup.
class ProgressWindow {
 public:
  ProgressWindow() {
    window_ = SDL_CreateWindow(kTitle, 560, 120, 0);
    if (window_) renderer_ = SDL_CreateRenderer(window_, nullptr);
  }
  ~ProgressWindow() {
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
  }

  // Returns false if the user closed the window (cancel).
  bool Update(const std::string& status, double fraction) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
    }
    if (!renderer_) return true;
    if (status != last_status_) {
      last_status_ = status;
      if (window_) SDL_SetWindowTitle(window_, status.c_str());
    }
    SDL_SetRenderDrawColor(renderer_, 24, 26, 32, 255);
    SDL_RenderClear(renderer_);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    const float margin = 24.0f, bar_h = 26.0f;
    SDL_FRect track{margin, h * 0.5f - bar_h * 0.5f, float(w) - margin * 2.0f, bar_h};
    SDL_SetRenderDrawColor(renderer_, 52, 56, 66, 255);
    SDL_RenderFillRect(renderer_, &track);
    SDL_FRect fill = track;
    fill.w = float(double(track.w) * (fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction)));
    SDL_SetRenderDrawColor(renderer_, 90, 170, 255, 255);
    SDL_RenderFillRect(renderer_, &fill);
    SDL_RenderPresent(renderer_);
    return true;
  }

 private:
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  std::string last_status_;
};

std::string HumanBytes(uint64_t n) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = double(n);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
  return buf;
}

// Unpacks `iso` into `dest`, showing progress. Returns false on error/cancel.
bool ExtractWithProgress(const fs::path& iso, const fs::path& dest, std::string& error) {
  ProgressWindow progress;
  progress.Update("Preparing...", 0.0);

  bool cancelled = false;
  Uint64 last_draw = 0;
  const bool ok = nhl::xdvdfs::ExtractImage(
      iso, dest, {"$SystemUpdate"},
      [&](uint64_t done, uint64_t total) {
        // Redraw at most ~30x/second; the callback fires every 8 MB.
        const Uint64 now = SDL_GetTicks();
        if (now - last_draw < 33 && done < total) return true;
        last_draw = now;
        const double frac = total ? double(done) / double(total) : 0.0;
        char status[160];
        std::snprintf(status, sizeof(status), "Unpacking game data - %.0f%%  (%s of %s)",
                      frac * 100.0, HumanBytes(done).c_str(), HumanBytes(total).c_str());
        if (!progress.Update(status, frac)) {
          cancelled = true;
          return false;
        }
        return true;
      },
      error);
  if (!ok && cancelled) error.clear();
  return ok;
}

}  // namespace

namespace {
fs::path g_active_game_data;
}  // namespace

void SetActiveGameDataPath(const fs::path& dir) { g_active_game_data = dir; }
fs::path ActiveGameDataPath() { return g_active_game_data; }

bool ForgetGameDataPath() {
  std::error_code ec;
  return fs::remove(RememberPath(), ec);
}

fs::path EnsureGameData(const fs::path& candidate) {
  if (HasGameData(candidate)) return candidate;

  // A previous run may have put the data somewhere else.
  const fs::path remembered = Recall();
  if (HasGameData(remembered)) {
    REXLOG_INFO("[nhl-setup] using remembered game data at {}", remembered.string());
    return remembered;
  }

  // Dialogs need the video subsystem; SDL init is refcounted so the graphics
  // system initialising it again later is fine.
  const bool video_ready = SDL_WasInit(SDL_INIT_VIDEO) || SDL_Init(SDL_INIT_VIDEO);
  if (!video_ready) {
    REXLOG_ERROR("[nhl-setup] no game data and SDL video is unavailable: {}", SDL_GetError());
    return {};
  }

  for (;;) {
    const int choice =
        Ask("NHL Legacy needs the game data from your own disc.\n\n"
            "Choose your disc image (.iso), or a folder you have already unpacked.\n\n"
            "Xbox 360 discs cannot be opened by Finder, so this app will unpack\n"
            "the image for you. It needs about 6 GB of free space.",
            {"Choose disc image...", "Choose unpacked folder...", "Quit"});
    if (choice != 0 && choice != 1) return {};

    const std::string picked = Pick(/*folder=*/choice == 1);
    if (picked.empty()) continue;  // cancelled the picker - back to the prompt

    if (choice == 1) {
      const fs::path dir = ResolveGameDir(picked);
      if (dir.empty()) {
        Message(SDL_MESSAGEBOX_WARNING, kTitle,
                "That folder does not contain default.xex.\n\n"
                "Pick the folder holding the unpacked disc files.");
        continue;
      }
      Remember(dir);
      REXLOG_INFO("[nhl-setup] using unpacked game data at {}", dir.string());
      return dir;
    }

    const fs::path iso(picked);
    const std::string ext = [&] {
      std::string e = iso.extension().string();
      for (char& c : e) c = char(std::tolower(c));
      return e;
    }();
    if (ext == ".7z" || ext == ".zip") {
      Message(SDL_MESSAGEBOX_WARNING, kTitle,
              "That is a compressed archive, not a disc image.\n\n"
              "Extract it first (double-click it in Finder for .zip), then pick\n"
              "the .iso inside.");
      continue;
    }

    // Validate before writing anything, so the wrong file fails immediately
    // rather than after gigabytes.
    std::vector<nhl::xdvdfs::Entry> entries;
    std::string error;
    if (!nhl::xdvdfs::ListImage(iso, entries, error)) {
      Message(SDL_MESSAGEBOX_ERROR, kTitle, "Could not read that disc image.\n\n" + error);
      continue;
    }
    bool has_xex = false;
    uint64_t total = 0;
    for (const auto& e : entries) {
      if (e.is_dir) continue;
      total += e.size;
      const size_t slash = e.path.find_last_of('/');
      std::string name = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
      for (char& c : name) c = char(std::tolower(c));
      if (name == "default.xex") has_xex = true;
    }
    if (!has_xex) {
      Message(SDL_MESSAGEBOX_ERROR, kTitle,
              "That disc image does not contain default.xex, so it is not an\n"
              "NHL Legacy Edition disc.");
      continue;
    }

    const fs::path dest = rex::filesystem::GetExecutableFolder() / "game";
    std::error_code ec;
    const auto space = fs::space(dest.parent_path(), ec);
    if (!ec && space.available < total + (total / 20)) {
      Message(SDL_MESSAGEBOX_ERROR, kTitle,
              "Not enough free disk space.\n\nNeed about " + HumanBytes(total + total / 20) +
                  ", but only " + HumanBytes(space.available) + " is available.");
      continue;
    }

    REXLOG_INFO("[nhl-setup] unpacking {} -> {} ({} bytes)", iso.string(), dest.string(), total);
    std::string extract_error;
    if (!ExtractWithProgress(iso, dest, extract_error)) {
      if (!extract_error.empty()) {
        Message(SDL_MESSAGEBOX_ERROR, kTitle, "Unpacking failed.\n\n" + extract_error);
      }
      continue;  // cancelled or failed - offer the choice again
    }
    if (!HasGameData(dest)) {
      Message(SDL_MESSAGEBOX_ERROR, kTitle,
              "Unpacking finished but default.xex is missing. The disc image may\n"
              "be damaged.");
      continue;
    }
    Remember(dest);
    REXLOG_INFO("[nhl-setup] game data ready at {}", dest.string());
    return dest;
  }
}

}  // namespace nhl
