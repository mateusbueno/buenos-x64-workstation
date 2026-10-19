#pragma once
// Test-only event list input (Story 2.5): a small deterministic text format that can carry what a Standard MIDI File cannot,
// the per-note contact_k events of a multi-contact keybed (engine/core/include/velora/core/contact_events.hpp). Used through
// `velora-offline-render --events file` (organ engine only); conventionally named *.contacts.
//
//   # comment (also after a line's last field), blank lines ignored
//   contacts 2|3                              contacts per key of the keybed (default 3); at most once
//   <time> on    <channel> <note> [<velocity 1..127>]     Note On  (default velocity 100)
//   <time> off   <channel> <note>                         Note Off
//   <time> close <channel> <note> <k>                     contact k closed  (k 0..255 is carried as written: the organ
//   <time> open  <channel> <note> <k>                     contact k open     refuses what is out of range)
//   <time> cc    <channel> <controller> <value 0..127>    control change (same mapping as in a MIDI file)
//
// <time> is an absolute position: `N` or `Nf` = N frames at 48 kHz, `Nms` or `N.FFFms` = milliseconds with up to three decimals
// (frames = floor(ms * 48), integer arithmetic only, so the result is identical on every platform). Lines need not be sorted;
// events are ordered by frame, ties in file order. Channel 0..15, note 0..127.
#include <string>
#include <vector>

#include "smf.hpp"

namespace velora::offline {

struct EventsResult {
  bool ok = false;
  std::string error;              // "line N: ..." on failure
  std::vector<NoteEvent> events;  // ordered by frame, ties in file order
  unsigned contact_count = 0;     // from the `contacts` directive; 0 = not given
};

EventsResult parse_events(const std::string& text);
EventsResult load_events_file(const std::string& path);

// Stable merge of two frame-ordered lists (ties: `a` first).
std::vector<NoteEvent> merge_events(const std::vector<NoteEvent>& a, const std::vector<NoteEvent>& b);

}  // namespace velora::offline
