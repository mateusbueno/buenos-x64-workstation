#pragma once
// Tonewheel tables for the organ engine (Story 2.2). Clean-room: every number here comes from public
// physical and musical facts, listed in engine/core/ORGAN.md ("Facts the tables come from"). Nothing is
// derived from another organ implementation.
#include <array>
#include <cstdint>

namespace velora::core::tonewheel {

inline constexpr std::uint32_t kWheelCount = 91;      // wheels 1..91, 12 per octave, 7.5 octaves
inline constexpr std::uint32_t kDrawbarCount = 9;     // 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'
inline constexpr int kLowestKey = 28;                 // E1: lowest key of the 73-key keybed (MIDI note)
inline constexpr int kHighestKey = 100;               // E7
inline constexpr std::uint32_t kKeyCount = 73;
// Wheel 1 is C1 (MIDI note 24) at twelve-tone equal temperament with A4 = 440 Hz. Wheel w sounds MIDI
// note kWheelNoteOffset + w, so wheel 13 (C2, MIDI 36) is the 8' wheel of the key C2.
inline constexpr int kWheelNoteOffset = 23;
inline constexpr double kReferenceA4Hz = 440.0;

// Drawbar footage -> semitones above the 8' pitch of the key, in drawbar order (param ids 100..108).
// 16' is an octave below, 5 1/3' the third harmonic of 16' (a fifth above 8'), 2 2/3' the sixth harmonic,
// 1 3/5' and 1 1/3' the tenth and twelfth, each placed on the nearest equal-tempered wheel.
inline constexpr std::array<int, kDrawbarCount> kDrawbarSemitones{-12, 7, 0, 12, 19, 24, 28, 31, 36};

// Foldback: a footage whose wheel number falls outside 1..91 moves by whole octaves (12 wheels) into
// range: up while below wheel 1, down while above wheel 91. Bounded for any input.
constexpr int fold_wheel(int wheel) noexcept {
  if (wheel < 1) wheel += 12 * ((1 - wheel + 11) / 12);
  if (wheel > static_cast<int>(kWheelCount)) wheel -= 12 * ((wheel - static_cast<int>(kWheelCount) + 11) / 12);
  return wheel;
}

// The 1-based wheel that drawbar `drawbar` (0..8) uses for MIDI note `note`.
constexpr int drawbar_wheel(int note, std::uint32_t drawbar) noexcept {
  return fold_wheel(note - kWheelNoteOffset + kDrawbarSemitones[drawbar % kDrawbarCount]);
}

}  // namespace velora::core::tonewheel
