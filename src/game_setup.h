// First-run game data setup.
//
// The port needs an unpacked copy of the user's own disc. Rather than making
// people discover a command-line tool, run a guided flow on launch when no game
// data is found: explain what is needed, open a native file picker, unpack the
// disc image with a progress window, and remember the location for next time.
//
// Xbox 360 discs are XDVDFS, so Finder cannot open them and the user cannot
// simply drag the files out themselves - see xdvdfs.h.

#pragma once

#include <filesystem>

namespace nhl {

// Returns a directory containing default.xex, or an empty path if the user
// cancelled or setup failed (in which case the app should exit).
//
// `candidate` is where the caller expects game data to live; if it already has
// default.xex it is returned untouched and nothing is shown.
std::filesystem::path EnsureGameData(const std::filesystem::path& candidate);

// The location actually in use this run, for display in the overlay. Set once
// during startup.
void SetActiveGameDataPath(const std::filesystem::path& dir);
std::filesystem::path ActiveGameDataPath();

// Clears the remembered location so the next launch runs the picker again.
// Returns false if there was nothing to forget.
bool ForgetGameDataPath();

}  // namespace nhl
