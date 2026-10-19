// Deterministic fuzz of the streaming decoder. Default budget 50 MB of stream; set VELORA_FUZZ_BYTES for more.
// One hour of 3 Mbaud traffic is 3e6 * 3600 / 8 = 1.35e9 bytes:
//   VELORA_FUZZ_BYTES=1350000000 ctest --test-dir build -R mcu_fuzz_test --output-on-failure
//
// The stream mixes: valid frames, random garbage (biased towards 0xA5), mutated frames (CRC broken), truncated frames,
// false sync headers, and intact frames the decoder must reject (unknown major, wrong length for the type, unknown
// type). Checked against a deliberately simple reference parser written in this file (greedy, left to right: at a sync
// byte, take the frame if the length field is sane, enough bytes exist and the CRC matches, else skip one byte):
//   1. the decoder delivers exactly the frames the reference accepts, in order, byte for byte (so it never delivers a
//      corrupt frame and never loses an intact one), whatever the chunking;
//   2. every injected valid frame is delivered unless a frame formed across garbage (about 2^-16 per false header, an
//      "accidental" frame) overlaps it; an injected frame that must be rejected is never delivered;
//   3. every counter (CRC errors, framing errors, resyncs, dropped bytes, version/length/type rejections, seq gaps,
//      lost frames, reorders) equals the reference's prediction, nothing overflows and nothing crashes; counters are consistent (bytes_in, frames_ok, no buffer overflow) and nothing crashes (the docker job runs
//      this under ASan/UBSan where available).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "mcu/mcu_frame.h"

namespace {

struct Rng {
  std::uint64_t s;
  std::uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>((next() >> 20) % n); }
  std::uint8_t byte() { return below(4) == 0 ? 0xA5 : static_cast<std::uint8_t>(next() >> 33); }
};

using Bytes = std::vector<std::uint8_t>;

enum class Kind { kValid, kRejected };
struct Item {
  Kind kind;
  std::uint64_t start, end;
};
// Counters the reference parser predicts for the decoder.
struct RefCounters {
  std::uint32_t crc = 0, framing = 0, resyncs = 0, dropped = 0, version = 0, bad_len = 0, unknown = 0, gaps = 0,
                reorders = 0, frames_ok = 0;
  std::uint32_t lost = 0;
  bool have_seq = false;
  std::uint16_t last_seq = 0;
};
struct RefSpan {
  std::uint64_t start, end;
  bool delivered;
};

Bytes encode_raw(const mcu_frame& f) {
  std::uint8_t buf[MCU_MAX_FRAME];
  const int n = mcu_encode_unchecked(&f, buf, sizeof buf);
  return n > 0 ? Bytes(buf, buf + n) : Bytes{};
}

mcu_frame random_valid(Rng& r, std::uint16_t seq) {
  static const std::uint8_t types[] = {MCU_TYPE_HELLO,   MCU_TYPE_KEY_CONTACT, MCU_TYPE_AFTERTOUCH,
                                       MCU_TYPE_CONTROL, MCU_TYPE_BUTTON,      MCU_TYPE_LED_SET,
                                       MCU_TYPE_TIME_SYNC, MCU_TYPE_ENTER_BOOTLOADER};
  mcu_frame f;
  mcu_frame_init(&f, types[r.below(8)], seq, static_cast<std::uint32_t>(r.next()));
  f.length = static_cast<std::uint8_t>(mcu_payload_length(f.type));
  for (unsigned i = 0; i < f.length; ++i) f.payload[i] = r.byte();
  return f;
}

// Independent check (does not use the decoder): does any slice of `b` form a complete valid frame?
bool contains_valid(const Bytes& b) {
  for (std::size_t i = 0; i + MCU_HEADER_SIZE + MCU_CRC_SIZE <= b.size(); ++i) {
    if (b[i] != MCU_SYNC) continue;
    const std::size_t len = b[i + 3];
    if (len > MCU_MAX_PAYLOAD || i + MCU_HEADER_SIZE + len + MCU_CRC_SIZE > b.size()) continue;
    const std::uint16_t crc = mcu_crc16(b.data() + i + 1, MCU_HEADER_SIZE - 1 + len);
    if (b[i + MCU_HEADER_SIZE + len] == (crc & 0xFF) && b[i + MCU_HEADER_SIZE + len + 1] == (crc >> 8)) return true;
  }
  return false;
}

Bytes mutate(Rng& r, const Bytes& src) {
  for (;;) {
    Bytes m = src;
    switch (r.below(4)) {
      case 0:  // bit flips
        for (unsigned i = 0, n = 1 + r.below(3); i < n; ++i) m[r.below(static_cast<std::uint32_t>(m.size()))] ^= static_cast<std::uint8_t>(1u << r.below(8));
        break;
      case 1:  // overwrite a byte
        m[r.below(static_cast<std::uint32_t>(m.size()))] = r.byte();
        break;
      case 2:  // insert a byte
        m.insert(m.begin() + r.below(static_cast<std::uint32_t>(m.size())), r.byte());
        break;
      default:  // drop a byte
        m.erase(m.begin() + r.below(static_cast<std::uint32_t>(m.size())));
        break;
    }
    if (!contains_valid(m)) return m;
  }
}

}  // namespace

int main() {
  std::uint64_t budget = 50'000'000ull;
  if (const char* e = std::getenv("VELORA_FUZZ_BYTES")) {
    const std::uint64_t v = std::strtoull(e, nullptr, 10);
    if (v > 0) budget = v;
  }
  std::printf("mcu_fuzz_test: %llu bytes\n", static_cast<unsigned long long>(budget));

  Rng r{0x9E3779B97F4A7C15ull};
  mcu_decoder dec;
  mcu_decoder_init(&dec, MCU_PROTOCOL_VERSION);

  Bytes win;                  // recent stream bytes
  std::uint64_t win_base = 0;
  std::uint64_t total = 0;    // absolute stream length fed so far
  std::deque<Item> items;
  std::deque<RefSpan> ref_spans;      // frames the reference consumed (delivered or rejected), for item resolution
  std::deque<Bytes> ref_delivered;    // wire bytes of reference deliveries not yet matched by the decoder
  std::deque<Bytes> dec_delivered;    // wire bytes of decoder deliveries not yet matched by the reference
  std::uint64_t rp = 0;               // reference parse position
  std::uint16_t seq = 0;
  std::uint64_t n_valid = 0, n_rejected = 0, n_delivered = 0, n_valid_delivered = 0, n_overlapped = 0, fails = 0;
  std::uint64_t next_report = 100'000'000ull;

  RefCounters rc;
  auto at = [&](std::uint64_t abs) { return win[static_cast<std::size_t>(abs - win_base)]; };

  // Reference parser over the window, up to the bytes fed so far.
  auto ref_advance = [&]() {
    while (rp < total) {
      if (at(rp) != MCU_SYNC) { ++rp; ++rc.dropped; continue; }
      if (rp + 4 > total) break;
      const std::size_t len = at(rp + 3);
      if (len > MCU_MAX_PAYLOAD) { ++rc.framing; ++rc.resyncs; ++rc.dropped; ++rp; continue; }
      const std::size_t tot = MCU_HEADER_SIZE + len + MCU_CRC_SIZE;
      if (rp + tot > total) break;
      Bytes w(tot);
      for (std::size_t i = 0; i < tot; ++i) w[i] = at(rp + i);
      const std::uint16_t crc = mcu_crc16(w.data() + 1, MCU_HEADER_SIZE - 1 + len);
      if (w[tot - 2] != (crc & 0xFF) || w[tot - 1] != (crc >> 8)) { ++rc.crc; ++rc.resyncs; ++rc.dropped; ++rp; continue; }
      const int want = mcu_payload_length(w[2]);
      const bool ver_ok = mcu_version_accepted(w[1], MCU_PROTOCOL_VERSION);
      const bool ok = ver_ok && want >= 0 && static_cast<std::size_t>(want) == len;
      if (!ver_ok) ++rc.version;
      else if (want < 0) ++rc.unknown;
      else if (!ok) ++rc.bad_len;
      // Sequence accounting: every CRC-valid frame consumes a seq; a delivered HELLO resets the expectation.
      const std::uint16_t seq_in = static_cast<std::uint16_t>(w[4] | (w[5] << 8));
      if ((ok && w[2] == MCU_TYPE_HELLO) || !rc.have_seq) {
        rc.last_seq = seq_in;
        rc.have_seq = true;
      } else {
        const std::uint16_t delta = static_cast<std::uint16_t>(seq_in - static_cast<std::uint16_t>(rc.last_seq + 1));
        if (delta == 0) rc.last_seq = seq_in;
        else if (delta > 0x8000) ++rc.reorders;
        else { ++rc.gaps; rc.lost += delta; rc.last_seq = seq_in; }
      }
      if (ok) ++rc.frames_ok;
      ref_spans.push_back({rp, rp + tot, ok});
      if (ok) ref_delivered.push_back(w);
      rp += tot;
    }
  };

  auto compare = [&]() {
    while (!ref_delivered.empty() && !dec_delivered.empty()) {
      if (ref_delivered.front() != dec_delivered.front()) {
        std::printf("FAIL: decoder delivered a frame the reference did not expect (or out of order)\n");
        ++fails;
      }
      ref_delivered.pop_front();
      dec_delivered.pop_front();
      ++n_delivered;
    }
  };

  auto resolve = [&](std::uint64_t horizon) {
    while (!items.empty() && items.front().end + 400 <= horizon) {
      const Item it = items.front();
      items.pop_front();
      bool equal_delivered = false, equal_rejected = false, overlapped = false;
      for (const RefSpan& sp : ref_spans) {
        if (sp.start == it.start && sp.end == it.end) { (sp.delivered ? equal_delivered : equal_rejected) = true; }
        else if (sp.start < it.end && it.start < sp.end) overlapped = true;
      }
      if (it.kind == Kind::kValid) {
        if (equal_delivered) ++n_valid_delivered;
        if (!equal_delivered) {
          if (overlapped) ++n_overlapped;
          else { std::printf("FAIL: valid frame at stream offset %llu was lost\n", static_cast<unsigned long long>(it.start)); ++fails; }
        }
      } else if (equal_delivered) {
        std::printf("FAIL: frame at %llu must be rejected but the reference delivered it\n", static_cast<unsigned long long>(it.start));
        ++fails;
      }
      (void)equal_rejected;
    }
    while (!ref_spans.empty() && ref_spans.front().end + 2000 <= horizon) {
      ref_spans.pop_front();
    }
  };

  while (total < budget && fails == 0) {
    // Build one chunk (about 64 KiB) of stream, recording what was injected.
    Bytes chunk;
    std::vector<Item> chunk_items;  // offsets relative to chunk start
    while (chunk.size() < 65536) {
      const std::uint32_t pick = r.below(100);
      const std::uint64_t start = chunk.size();
      if (pick < 50) {  // valid frame (seq gaps now and then)
        if (r.below(20) == 0) seq = static_cast<std::uint16_t>(seq + 1 + r.below(5));
        const Bytes w = encode_raw(random_valid(r, seq++));
        chunk.insert(chunk.end(), w.begin(), w.end());
        chunk_items.push_back({Kind::kValid, start, chunk.size()});
        ++n_valid;
      } else if (pick < 65) {  // garbage run
        for (unsigned i = 0, n = 1 + r.below(60); i < n; ++i) chunk.push_back(r.byte());
      } else if (pick < 77) {  // mutated frame
        const Bytes m = mutate(r, encode_raw(random_valid(r, seq)));
        chunk.insert(chunk.end(), m.begin(), m.end());
      } else if (pick < 86) {  // truncated frame
        const Bytes w = encode_raw(random_valid(r, seq));
        chunk.insert(chunk.end(), w.begin(), w.begin() + 1 + r.below(static_cast<std::uint32_t>(w.size() - 1)));
      } else if (pick < 92) {  // false header with a random length
        chunk.push_back(MCU_SYNC);
        chunk.push_back(static_cast<std::uint8_t>(r.next() >> 40));
        chunk.push_back(static_cast<std::uint8_t>(r.next() >> 40));
        chunk.push_back(static_cast<std::uint8_t>(r.below(MCU_MAX_PAYLOAD + 8)));
      } else {  // intact but must be rejected
        mcu_frame f = random_valid(r, seq);
        switch (r.below(3)) {
          case 0: f.version = static_cast<std::uint8_t>((2 + r.below(14)) << 4 | r.below(16)); break;  // major 2..15
          case 1: f.length = static_cast<std::uint8_t>(f.length + 1 + r.below(8)); for (unsigned i = 0; i < f.length; ++i) f.payload[i] = r.byte(); break;
          default: f.type = static_cast<std::uint8_t>(9 + r.below(200)); break;
        }
        const Bytes w = encode_raw(f);
        chunk.insert(chunk.end(), w.begin(), w.end());
        chunk_items.push_back({Kind::kRejected, start, chunk.size()});
        ++n_rejected;
      }
    }
    const std::uint64_t chunk_base = total;
    win.insert(win.end(), chunk.begin(), chunk.end());
    for (Item it : chunk_items) {
      it.start += chunk_base;
      it.end += chunk_base;
      items.push_back(it);
    }
    // Feed in random-sized slices (one byte to a few hundred); the reference follows the same bytes.
    std::size_t pos = 0;
    while (pos < chunk.size()) {
      std::size_t n = (r.below(4) == 0) ? 1 : 1 + r.below(300);
      if (n > chunk.size() - pos) n = chunk.size() - pos;
      for (std::size_t i = 0; i < n; ++i) {
        mcu_frame f;
        mcu_event ev = mcu_decoder_feed(&dec, chunk[pos + i], &f);
        while (ev != MCU_EV_NONE) {
          if (ev == MCU_EV_FRAME) dec_delivered.push_back(encode_raw(f));
          ev = mcu_decoder_poll(&dec, &f);
        }
      }
      pos += n;
      total += n;
      ref_advance();
      compare();
    }
    resolve(total);
    const std::uint64_t keep_from = std::min(rp, total > 4096 ? total - 4096 : 0);
    if (keep_from > win_base + 4096) {
      const std::size_t drop = static_cast<std::size_t>(keep_from - win_base);
      win.erase(win.begin(), win.begin() + static_cast<std::ptrdiff_t>(drop));
      win_base += drop;
    }
    if (total >= next_report) {
      std::printf("  ... %llu bytes\n", static_cast<unsigned long long>(total));
      std::fflush(stdout);
      next_report += 100'000'000ull;
    }
  }
  resolve(total + 100000);  // flush remaining items (bytes still pending in the decoder are pending in the reference too)
  if (!ref_delivered.empty() || !dec_delivered.empty()) {
    std::printf("FAIL: unmatched deliveries at the end (reference %zu, decoder %zu)\n", ref_delivered.size(), dec_delivered.size());
    ++fails;
  }

  const mcu_decoder_counters& c = dec.counters;
  // The decoder and the reference must agree on every counter, not only on the delivered frames.
  auto same_counter = [&](const char* name, std::uint32_t a, std::uint32_t b) {
    if (a != b) { std::printf("FAIL: counter %s decoder=%u reference=%u\n", name, a, b); ++fails; }
  };
  same_counter("crc_errors", c.crc_errors, rc.crc);
  same_counter("framing_errors", c.framing_errors, rc.framing);
  same_counter("resyncs", c.resyncs, rc.resyncs);
  same_counter("dropped_bytes", c.dropped_bytes, rc.dropped);
  same_counter("version_mismatches", c.version_mismatches, rc.version);
  same_counter("bad_lengths", c.bad_lengths, rc.bad_len);
  same_counter("unknown_types", c.unknown_types, rc.unknown);
  same_counter("seq_gaps", c.seq_gaps, rc.gaps);
  same_counter("seq_lost", c.seq_lost, rc.lost);
  same_counter("seq_reorders", c.seq_reorders, rc.reorders);
  same_counter("frames_ok", c.frames_ok, rc.frames_ok);
  if (c.bytes_in != total) { std::printf("FAIL: bytes_in mismatch\n"); ++fails; }
  if (c.frames_ok != static_cast<std::uint32_t>(n_delivered)) { std::printf("FAIL: frames_ok != delivered\n"); ++fails; }
  if (c.overflow_bytes != 0) { std::printf("FAIL: decoder buffer overflow bytes\n"); ++fails; }
  if (c.crc_errors == 0 || c.dropped_bytes == 0 || c.seq_gaps == 0 || c.seq_reorders == 0 || c.version_mismatches == 0 ||
      c.framing_errors == 0 || c.bad_lengths == 0 || c.unknown_types == 0) {
    std::printf("FAIL: expected every error counter to be exercised\n");
    ++fails;
  }
  std::printf("stream=%llu valid=%llu rejected=%llu delivered=%llu accidental_frames=%llu valid_lost_to_overlap=%llu\n",
              static_cast<unsigned long long>(total), static_cast<unsigned long long>(n_valid),
              static_cast<unsigned long long>(n_rejected), static_cast<unsigned long long>(n_delivered),
              static_cast<unsigned long long>(n_delivered - n_valid_delivered), static_cast<unsigned long long>(n_overlapped));
  std::printf("crc=%u framing=%u resyncs=%u dropped=%u version=%u bad_len=%u unknown=%u gaps=%u lost=%u\n", c.crc_errors,
              c.framing_errors, c.resyncs, c.dropped_bytes, c.version_mismatches, c.bad_lengths, c.unknown_types,
              c.seq_gaps, c.seq_lost);
  if (fails == 0) std::printf("mcu_fuzz_test: ok\n");
  return fails == 0 ? 0 : 1;
}
