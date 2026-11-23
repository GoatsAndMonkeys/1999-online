// mail_chime.h — the "You've got mail" chime.
//
// An original four-note bell arpeggio (G5 C6 E6 G6), synthesized here: no
// audio asset, no audio library. play_mail_chime() writes it once per run as a
// 16-bit mono WAV in the temp folder and hands it to the first system player
// found on PATH (afplay on macOS; aplay, pw-play or paplay on Linux), spawned
// with posix_spawnp: no shell. Fire and forget; the child is reaped on the
// next call.
//
// Silent (returns false) when files are disabled (--shots / --icon, see
// store::enabled()), when the "mail.sound" setting is false, when no player
// is installed, or within kMinGapMs of the previous chime.
//
// LVGL-free. UI thread only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mm {

constexpr int kMailChimeRate = 22050;
constexpr uint32_t kMailChimeMinGapMs = 1500;

// The chime as mono 16-bit PCM at `rate` Hz (about 1.1 s).
std::vector<int16_t> mail_chime_pcm(int rate = kMailChimeRate);
// RIFF/WAVE bytes of mail_chime_pcm().
std::string mail_chime_wav(int rate = kMailChimeRate);
// true if a player was started.
bool play_mail_chime();

} // namespace mm
