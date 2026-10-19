#include "events_file.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>

#include "parse_util.hpp"

namespace velora::offline {

namespace {

constexpr std::uint64_t kMaxFrame = 48000ull * 3600;  // one hour, as the renderer's own limit

bool parse_time(const std::string& tok, std::uint64_t& frames) {
  std::string t = tok;
  bool ms = false;
  if (t.size() > 2 && t.compare(t.size() - 2, 2, "ms") == 0) {
    ms = true;
    t.resize(t.size() - 2);
  } else if (t.size() > 1 && t.back() == 'f') {
    t.pop_back();
  }
  if (!ms) {
    std::uint64_t n = 0;
    if (!parse_uint(t, kMaxFrame, n)) return false;
    frames = n;
    return true;
  }
  // milliseconds: integer part, optional '.' and one to three decimals
  std::string ip = t, fp;
  const std::size_t dot = t.find('.');
  if (dot != std::string::npos) {
    ip = t.substr(0, dot);
    fp = t.substr(dot + 1);
    if (fp.empty() || fp.size() > 3 || fp.find_first_not_of("0123456789") != std::string::npos) return false;
  }
  std::uint64_t whole = 0;
  if (!parse_uint(ip, kMaxFrame / 48, whole)) return false;
  while (fp.size() < 3) fp.push_back('0');
  const std::uint64_t thousandths = whole * 1000 + std::stoull(fp);
  frames = thousandths * 48 / 1000;
  return frames <= kMaxFrame;
}

}  // namespace

EventsResult parse_events(const std::string& text) {
  EventsResult res;
  std::istringstream in(text);
  std::string line;
  unsigned lineno = 0;
  bool contacts_seen = false;
  std::vector<NoteEvent> items;
  auto fail = [&](const std::string& msg) {
    res.ok = false;
    res.error = "line " + std::to_string(lineno) + ": " + msg;
    res.events.clear();
    return res;
  };
  while (std::getline(in, line)) {
    ++lineno;
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    std::istringstream ls(line);
    std::vector<std::string> tok;
    for (std::string w; ls >> w;) tok.push_back(w);
    if (tok.empty()) continue;
    if (tok[0] == "contacts") {
      std::uint64_t n = 0;
      if (tok.size() != 2 || !parse_uint(tok[1], 3, n) || n < 2) return fail("contacts takes 2 or 3");
      if (contacts_seen) return fail("contacts given twice");
      contacts_seen = true;
      res.contact_count = static_cast<unsigned>(n);
      continue;
    }
    std::uint64_t frame = 0;
    if (!parse_time(tok[0], frame)) return fail("invalid time '" + tok[0] + "' (frames, or milliseconds as 12ms / 0.125ms)");
    if (tok.size() < 2) return fail("missing event kind");
    const std::string& kind = tok[1];
    auto num = [&](std::size_t i, std::uint64_t hi, std::uint64_t& out) { return i < tok.size() && parse_uint(tok[i], hi, out); };
    std::uint64_t ch = 0, a = 0, b = 0;
    NoteEvent e;
    e.tick = frame;
    e.frame = frame;
    if (kind == "on" || kind == "off") {
      if (!num(2, 15, ch) || !num(3, 127, a)) return fail("'" + kind + "' needs <channel 0..15> <note 0..127>");
      std::uint64_t vel = 100;
      if (kind == "on") {
        if (tok.size() > 5 || (tok.size() == 5 && (!num(4, 127, vel) || vel < 1))) return fail("velocity must be 1..127");
      } else if (tok.size() != 4) {
        return fail("'off' takes exactly <channel> <note>");
      }
      e.on = kind == "on";
      e.channel = static_cast<std::uint8_t>(ch);
      e.note = static_cast<std::uint8_t>(a);
      e.velocity16 = e.on ? static_cast<std::uint16_t>((vel * 65535 + 63) / 127) : 0;
    } else if (kind == "close" || kind == "open") {
      if (tok.size() != 5 || !num(2, 15, ch) || !num(3, 127, a) || !num(4, 255, b))
        return fail("'" + kind + "' needs <channel 0..15> <note 0..127> <k 0..255>");
      e.contact = true;
      e.closed = kind == "close";
      e.channel = static_cast<std::uint8_t>(ch);
      e.note = static_cast<std::uint8_t>(a);
      e.contact_index = static_cast<std::uint8_t>(b);
    } else if (kind == "cc") {
      if (tok.size() != 5 || !num(2, 15, ch) || !num(3, 127, a) || !num(4, 127, b))
        return fail("'cc' needs <channel 0..15> <controller 0..127> <value 0..127>");
      e.cc = true;
      e.channel = static_cast<std::uint8_t>(ch);
      e.note = static_cast<std::uint8_t>(a);
      e.value = static_cast<std::uint8_t>(b);
    } else {
      return fail("unknown event kind '" + kind + "' (on, off, close, open, cc)");
    }
    items.push_back(e);
  }
  std::stable_sort(items.begin(), items.end(), [](const NoteEvent& x, const NoteEvent& y) { return x.frame < y.frame; });
  res.events = std::move(items);
  res.ok = true;
  return res;
}

EventsResult load_events_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    EventsResult r;
    r.error = "cannot read file";
    return r;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse_events(ss.str());
}

std::vector<NoteEvent> merge_events(const std::vector<NoteEvent>& a, const std::vector<NoteEvent>& b) {
  auto by_frame = [](const NoteEvent& x, const NoteEvent& y) { return x.frame < y.frame; };
  std::vector<NoteEvent> sa = a, sb = b;  // each list is put in frame order first (stable), so the merge never relies on the callers
  std::stable_sort(sa.begin(), sa.end(), by_frame);
  std::stable_sort(sb.begin(), sb.end(), by_frame);
  std::vector<NoteEvent> out;
  out.reserve(a.size() + b.size());
  std::merge(sa.begin(), sa.end(), sb.begin(), sb.end(), std::back_inserter(out),
             [](const NoteEvent& x, const NoteEvent& y) { return x.frame < y.frame; });
  return out;
}

}  // namespace velora::offline
