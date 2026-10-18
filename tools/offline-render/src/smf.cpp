#include "smf.hpp"

#include <algorithm>
#include <fstream>
#include <cstdint>
#include <iterator>
#include <utility>

namespace velora::offline {
namespace {

struct Reader {
  const std::uint8_t* p;
  std::size_t n;
  std::size_t pos = 0;
  bool fail = false;

  std::size_t left() const { return n - pos; }
  std::uint8_t u8() {
    if (pos >= n) { fail = true; return 0; }
    return p[pos++];
  }
  std::uint32_t u16() { std::uint32_t a = u8(); return (a << 8) | u8(); }
  std::uint32_t u32() { std::uint32_t a = u16(); return (a << 16) | u16(); }
  std::uint32_t vlq() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      std::uint8_t b = u8();
      if (fail) return 0;
      v = (v << 7) | (b & 0x7F);
      if (!(b & 0x80)) return v;
    }
    fail = true;  // longer than 4 bytes
    return 0;
  }
  void skip(std::size_t k) {
    if (k > left()) { fail = true; pos = n; return; }
    pos += k;
  }
};

struct Raw {
  std::uint64_t tick;
  std::uint32_t track;
  std::uint32_t seq;
  NoteEvent ev;
};

SmfResult fail(std::string msg) {
  SmfResult r;
  r.error = std::move(msg);
  return r;
}

}  // namespace

SmfResult parse_smf(const std::uint8_t* data, std::size_t size, std::uint32_t sample_rate) {
  Reader f{data, size};
  if (size < 14 || data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd')
    return fail("not a Standard MIDI File (bad MThd header)");
  f.pos = 4;
  const std::uint32_t hlen = f.u32();
  if (hlen < 6 || hlen > f.left()) return fail("bad MThd length");
  const std::uint32_t format = f.u16();
  const std::uint32_t ntrks = f.u16();
  const std::uint32_t division = f.u16();
  f.skip(hlen - 6);
  if (format > 1) return fail("unsupported SMF format " + std::to_string(format) + " (need 0 or 1)");
  if (division & 0x8000) return fail("SMPTE time division is not supported");
  if (division == 0) return fail("zero ticks per quarter note");
  if (ntrks == 0) return fail("file declares no tracks");
  if (format == 0 && ntrks != 1) return fail("format 0 file must have exactly one track");

  std::vector<Raw> raws;
  std::vector<std::pair<std::uint64_t, std::uint32_t>> tempos;  // tick, us per quarter (file order)
  std::uint32_t tracks_seen = 0;
  std::uint32_t seq = 0;

  while (tracks_seen < ntrks) {
    if (f.left() < 8) return fail("truncated file: missing track chunk");
    const std::uint8_t* id = data + f.pos;
    f.pos += 4;
    const std::uint32_t len = f.u32();
    if (len > f.left()) return fail("truncated track chunk");
    const std::size_t end = f.pos + len;
    if (!(id[0] == 'M' && id[1] == 'T' && id[2] == 'r' && id[3] == 'k')) {
      f.pos = end;  // unknown chunk: skip
      continue;
    }
    const std::uint32_t track = tracks_seen++;
    Reader t{data, end, f.pos};
    std::uint64_t tick = 0;
    int running = -1;
    bool eot = false;
    while (t.pos < end && !eot) {
      tick += t.vlq();
      if (t.fail) return fail("truncated track (delta time)");
      if (t.pos >= end) return fail("truncated track (missing event)");
      std::uint8_t b = data[t.pos];
      int status;
      if (b & 0x80) {
        ++t.pos;
        status = b;
        if (status < 0xF0) running = status; else running = -1;  // meta/sysex cancel running status
      } else {
        if (running < 0) return fail("data byte without running status");
        status = running;
      }
      if (status == 0xFF) {
        const std::uint8_t type = t.u8();
        const std::uint32_t mlen = t.vlq();
        if (t.fail || mlen > t.left()) return fail("truncated meta event");
        if (type == 0x51) {
          if (mlen != 3) return fail("bad tempo meta event length");
          const std::uint32_t us = (static_cast<std::uint32_t>(data[t.pos]) << 16) |
                                   (static_cast<std::uint32_t>(data[t.pos + 1]) << 8) | data[t.pos + 2];
          if (us == 0) return fail("zero tempo");
          tempos.emplace_back(tick, us);
        } else if (type == 0x2F) {
          eot = true;
        }
        t.skip(mlen);
      } else if (status == 0xF0 || status == 0xF7) {
        const std::uint32_t slen = t.vlq();
        if (t.fail || slen > t.left()) return fail("truncated sysex event");
        t.skip(slen);
      } else if (status >= 0xF8) {
        // single byte realtime: nothing to read
      } else if (status >= 0xF1) {
        t.skip(status == 0xF2 ? 2 : (status == 0xF3 || status == 0xF1) ? 1 : 0);
        if (t.fail) return fail("truncated track (system common)");
      } else {
        const int hi = status & 0xF0;
        const std::uint8_t d1 = t.u8();
        std::uint8_t d2 = 0;
        if (hi != 0xC0 && hi != 0xD0) d2 = t.u8();
        if (t.fail) return fail("truncated channel message");
        if ((d1 & 0x80) || (d2 & 0x80)) return fail("invalid data byte in channel message");
        if (hi == 0x80 || hi == 0x90) {
          NoteEvent e;
          e.tick = tick;
          e.channel = static_cast<std::uint8_t>(status & 0x0F);
          e.note = d1;
          e.on = (hi == 0x90 && d2 != 0);
          e.velocity16 = static_cast<std::uint16_t>((d2 * 65535u + 63u) / 127u);
          raws.push_back({tick, track, seq++, e});
        }
      }
    }
    if (t.fail) return fail("truncated track");
    f.pos = end;
  }
  if (tracks_seen != ntrks) return fail("fewer tracks than declared");

  // Tempo map: stable by tick, later entry at same tick wins; default 120 bpm.
  std::stable_sort(tempos.begin(), tempos.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  struct Seg { std::uint64_t tick; std::uint32_t tempo; std::uint64_t units; };
  std::vector<Seg> segs;
  segs.push_back({0, 500000, 0});
  for (const auto& tp : tempos) {
    if (tp.first == segs.back().tick) { segs.back().tempo = tp.second; continue; }
    const Seg& s = segs.back();
    if (tp.first - s.tick > UINT64_MAX / s.tempo) return fail("tempo map overflows 64-bit time");
    const std::uint64_t add = (tp.first - s.tick) * s.tempo;
    if (add > UINT64_MAX - s.units) return fail("tempo map overflows 64-bit time");
    segs.push_back({tp.first, tp.second, s.units + add});
  }

  std::stable_sort(raws.begin(), raws.end(), [](const Raw& a, const Raw& b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    if (a.track != b.track) return a.track < b.track;
    return a.seq < b.seq;
  });

  // units are (us * ppq-ticks); frames = units * rate / (ppq * 1e6). Reduce rate/1e6 by gcd.
  std::uint64_t num = sample_rate, den = 1000000;
  while (true) {
    std::uint64_t a = num, b = den;
    while (b) { std::uint64_t r = a % b; a = b; b = r; }
    if (a == 1) break;
    num /= a; den /= a;
  }
  SmfResult out;
  out.format = static_cast<std::uint16_t>(format);
  out.ppq = static_cast<std::uint16_t>(division);
  std::size_t si = 0;
  for (auto& r : raws) {
    while (si + 1 < segs.size() && segs[si + 1].tick <= r.tick) ++si;
    const Seg& s = segs[si];
    if (r.tick - s.tick > UINT64_MAX / s.tempo) return fail("event time overflows 64-bit time");
    const std::uint64_t add = (r.tick - s.tick) * s.tempo;
    if (add > UINT64_MAX - s.units) return fail("event time overflows 64-bit time");
    const std::uint64_t units = s.units + add;
    if (units > UINT64_MAX / num) return fail("event time overflows 64-bit time");
    r.ev.frame = units * num / (den * division);
    out.events.push_back(r.ev);
  }
  out.ok = true;
  return out;
}

SmfResult parse_smf(const std::vector<std::uint8_t>& bytes, std::uint32_t sample_rate) {
  return parse_smf(bytes.data(), bytes.size(), sample_rate);
}

SmfResult load_smf_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail("cannot open MIDI file: " + path);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse_smf(bytes);
}

}  // namespace velora::offline
