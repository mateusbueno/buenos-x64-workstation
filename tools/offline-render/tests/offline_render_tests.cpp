#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "nulltest.hpp"
#include "render.hpp"
#include "smf.hpp"
#include "wav.hpp"

using namespace velora::offline;

static int g_fail = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      ++g_fail;                                                        \
    }                                                                  \
  } while (0)

using Bytes = std::vector<std::uint8_t>;

static void be32(Bytes& v, std::uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i))); }
static void be16(Bytes& v, std::uint32_t x) { v.push_back(static_cast<std::uint8_t>(x >> 8)); v.push_back(static_cast<std::uint8_t>(x)); }
static void vlq(Bytes& v, std::uint32_t n) {
  Bytes t{static_cast<std::uint8_t>(n & 0x7F)};
  for (n >>= 7; n; n >>= 7) t.push_back(static_cast<std::uint8_t>((n & 0x7F) | 0x80));
  v.insert(v.end(), t.rbegin(), t.rend());
}
static Bytes header(int format, int ntrks, int ppq) {
  Bytes v{'M', 'T', 'h', 'd'};
  be32(v, 6); be16(v, format); be16(v, ntrks); be16(v, ppq);
  return v;
}
static void track(Bytes& file, const Bytes& body, bool eot = true) {
  Bytes b = body;
  if (eot) { vlq(b, 0); b.push_back(0xFF); b.push_back(0x2F); b.push_back(0); }
  file.insert(file.end(), {'M', 'T', 'r', 'k'});
  be32(file, static_cast<std::uint32_t>(b.size()));
  file.insert(file.end(), b.begin(), b.end());
}
static void tempo(Bytes& b, std::uint32_t delta, std::uint32_t us) {
  vlq(b, delta);
  b.insert(b.end(), {0xFF, 0x51, 3, static_cast<std::uint8_t>(us >> 16), static_cast<std::uint8_t>(us >> 8), static_cast<std::uint8_t>(us)});
}

static void test_smf_basic() {
  Bytes f = header(0, 1, 480), b;
  vlq(b, 0); b.insert(b.end(), {0x90, 60, 100});
  vlq(b, 480); b.insert(b.end(), {0x80, 60, 0});
  track(f, b);
  SmfResult r = parse_smf(f);
  CHECK(r.ok && r.events.size() == 2);
  CHECK(r.events[0].on && r.events[0].frame == 0 && r.events[0].note == 60);
  CHECK(!r.events[1].on && r.events[1].frame == 24000);  // 1 beat at 120 bpm = 0.5 s
  CHECK(r.events[0].velocity16 == (100u * 65535u + 63u) / 127u);
}

static void test_smf_running_status_vel0_long_delta() {
  Bytes f = header(0, 1, 96), b;
  vlq(b, 0); b.insert(b.end(), {0x91, 60, 100});
  vlq(b, 0); b.insert(b.end(), {64, 100});                   // running status
  vlq(b, 300); b.insert(b.end(), {60, 0});                   // vel 0 => off, 2-byte delta
  vlq(b, 20000); b.insert(b.end(), {64, 0});                 // 3-byte delta
  vlq(b, 0); b.insert(b.end(), {0xFF, 0x01, 2, 'h', 'i'});   // text meta ignored
  vlq(b, 0); b.insert(b.end(), {0xF0, 2, 0x7D, 0xF7});       // sysex ignored, clears running status
  vlq(b, 0); b.insert(b.end(), {0xC1, 5});                   // program change ignored
  track(f, b);
  SmfResult r = parse_smf(f);
  CHECK(r.ok && r.events.size() == 4);
  if (r.events.size() == 4) {
    CHECK(r.events[1].on && r.events[1].note == 64 && r.events[1].channel == 1);
    CHECK(!r.events[2].on && r.events[2].note == 60);
    CHECK(r.events[2].tick == 300 && r.events[3].tick == 20300);
    CHECK(r.events[2].frame == 300ull * 500000 * 48000 / (96ull * 1000000));
  }
}

static void test_smf_tempo_map() {
  Bytes f = header(1, 2, 480), t0, t1;
  tempo(t0, 0, 500000);
  tempo(t0, 480, 250000);  // 240 bpm from tick 480
  track(f, t0);
  vlq(t1, 0); t1.insert(t1.end(), {0x90, 60, 100});
  vlq(t1, 480); t1.insert(t1.end(), {0x80, 60, 0});
  vlq(t1, 480); t1.insert(t1.end(), {0x90, 62, 100});
  track(f, t1);
  SmfResult r = parse_smf(f);
  CHECK(r.ok && r.events.size() == 3);
  if (r.events.size() == 3) {
    CHECK(r.events[1].frame == 24000);
    CHECK(r.events[2].frame == 24000 + 12000);  // second beat is half as long
  }
}

static void test_smf_malformed() {
  Bytes good = header(0, 1, 480), b;
  vlq(b, 0); b.insert(b.end(), {0x90, 60, 100});
  track(good, b);
  CHECK(parse_smf(good).ok);
  Bytes bad = good; bad[0] = 'X';
  CHECK(!parse_smf(bad).ok);
  Bytes trunc(good.begin(), good.end() - 3);
  CHECK(!parse_smf(trunc).ok);
  CHECK(!parse_smf(Bytes{}).ok);
  Bytes smpte = header(0, 1, 0xE728); track(smpte, b);
  CHECK(!parse_smf(smpte).ok);
  Bytes fmt2 = header(2, 1, 480); track(fmt2, b);
  CHECK(!parse_smf(fmt2).ok);
  Bytes nodata = header(0, 1, 480), b2; vlq(b2, 0); b2.insert(b2.end(), {60, 100});
  track(nodata, b2);  // data byte with no running status
  CHECK(!parse_smf(nodata).ok);
  Bytes cut = header(0, 1, 480), b3; vlq(b3, 0); b3.insert(b3.end(), {0x90, 60});
  track(cut, b3, false);  // channel message truncated inside a complete chunk
  CHECK(!parse_smf(cut).ok);
}

static WavData noise(std::size_t frames, std::uint32_t seed = 1) {
  WavData w;
  w.samples.resize(frames * 2);
  std::uint32_t s = seed;
  for (auto& x : w.samples) {
    s = s * 1664525u + 1013904223u;
    x = (static_cast<float>(s >> 8) / 8388608.0f - 1.0f) * 0.3f;
  }
  return w;
}
static WavData delayed(const WavData& w, std::size_t k) {
  WavData d = w;
  d.samples.insert(d.samples.begin(), k * 2, 0.0f);
  return d;
}

static void test_wav_roundtrip() {
  WavData w = noise(1000);
  Bytes e = encode_wav(w);
  CHECK(e.size() == 44 + 1000 * 8);
  WavData r;
  std::string err;
  CHECK(decode_wav(e, r, err));
  CHECK(r.samples == w.samples && r.channels == 2 && r.sample_rate == 48000);
  e[20] = 1;  // PCM tag: unsupported
  CHECK(!decode_wav(e, r, err));
}

static void test_null_identical_and_offset() {
  NullParams p; p.tolerance_db = -90; p.max_offset_frames = 16;
  WavData a = noise(20000);
  NullResult r = null_test(a, a, p);
  CHECK(r.pass && r.residual_db <= -299.0 && r.offset_frames == 0);
  // delayed within max
  r = null_test(a, delayed(a, 11), p);
  CHECK(r.pass && r.offset_frames == 11);
  CHECK(r.length_diff_frames == 11);
  // candidate advanced (negative offset)
  WavData adv = a; adv.samples.erase(adv.samples.begin(), adv.samples.begin() + 2 * 5);
  r = null_test(a, adv, p);
  CHECK(r.pass && r.offset_frames == -5);
  // beyond max
  r = null_test(a, delayed(a, 17), p);
  CHECK(!r.pass && r.comparable);
  // different content
  r = null_test(a, noise(20000, 7), p);
  CHECK(!r.pass && r.residual_db > -10.0);
}

static void test_null_mismatch_and_silence() {
  NullParams p; p.tolerance_db = -90;
  WavData a = noise(1000), b = a;
  b.sample_rate = 44100;
  NullResult r = null_test(a, b, p);
  CHECK(!r.pass && !r.comparable && !r.message.empty());
  b = a; b.channels = 1;
  r = null_test(a, b, p);
  CHECK(!r.pass && !r.comparable);
  // length difference: compare overlap, report it
  b = a; b.samples.resize(1600);
  r = null_test(a, b, p);
  CHECK(r.pass && r.overlap_frames == 800 && r.length_diff_frames == -200);
  // silent reference
  WavData s; s.samples.assign(2000, 0.0f);
  r = null_test(s, s, p);
  CHECK(r.pass && !r.message.empty());
  r = null_test(s, a, p);
  CHECK(!r.pass);
}

static void test_worst_window() {
  NullParams p; p.tolerance_db = -90;
  WavData a = noise(48000);
  WavData b = a;
  // corrupt 1000 frames inside the window starting at 4800*5
  for (std::size_t i = 4800 * 5 + 100; i < 4800 * 5 + 1100; ++i) b.samples[i * 2] += 0.1f;
  NullResult r = null_test(a, b, p);
  CHECK(!r.pass && r.worst_window_start == 4800 * 5);
  CHECK(r.worst_window_db > r.residual_db);
  std::string rep = format_report("cand.wav", r, p);
  CHECK(rep.find("residual:") != std::string::npos && rep.find("worst 100 ms window") != std::string::npos);
  CHECK(rep.find("cand.wav") != std::string::npos);
}

static SmfResult note_smf() {
  Bytes f = header(0, 1, 480), b;
  vlq(b, 0); b.insert(b.end(), {0x90, 69, 100});
  vlq(b, 100); b.insert(b.end(), {0x80, 69, 0});
  vlq(b, 33); b.insert(b.end(), {0x90, 72, 90});  // frame not on a block boundary
  vlq(b, 200); b.insert(b.end(), {0x80, 72, 0});
  track(f, b);
  return parse_smf(f);
}

static void test_render_determinism_and_onsets() {
  SmfResult s = note_smf();
  CHECK(s.ok);
  RenderOptions o; o.tail_seconds = 0.1;
  WavData a, b;
  std::string err;
  CHECK(render_events(s.events, o, a, err) && render_events(s.events, o, b, err));
  CHECK(encode_wav(a) == encode_wav(b));  // byte-equal
  // Note 1 onset at frame 0; second note onset at its exact tempo-map frame.
  CHECK(a.samples[0] != 0.0f);
  const std::uint64_t f2 = s.events[2].frame;
  CHECK(f2 % 64 != 0);
  CHECK(a.samples[(f2 - 1) * 2] == 0.0f && a.samples[(f2 - 1) * 2 + 1] == 0.0f);  // note 1 released
  CHECK(a.samples[f2 * 2] != 0.0f && a.samples[f2 * 2 + 1] != 0.0f);              // exact onset
  RenderOptions d = o; d.detune_cents = 50.0f;
  WavData c;
  CHECK(render_events(s.events, d, c, err));
  NullParams p; p.tolerance_db = -60;
  CHECK(!null_test(a, c, p).pass);
  CHECK(null_test(a, b, p).pass);
}

#ifdef OFFLINE_RENDER_EXE
static void test_cli_malformed_no_output() {
  const std::string dir = OFFLINE_TEST_DIR;
  const std::string in = dir + "/malformed.mid", out = dir + "/malformed.wav";
  std::remove(out.c_str());
  { std::ofstream f(in, std::ios::binary); f.write("MThd\0\0\0\6garbage", 14); }
  const std::string cmd = std::string("\"") + OFFLINE_RENDER_EXE + "\" --midi \"" + in + "\" --out \"" + out + "\" 2>/dev/null";
  CHECK(std::system(cmd.c_str()) != 0);
  CHECK(!std::ifstream(out).good());
}
#endif

// Story 2.3: control changes are parsed, mapped ones reach the organ's parameters, unmapped ones are dropped.
static void test_smf_controllers_and_organ_options() {
  Bytes f = header(0, 1, 480), b;
  vlq(b, 0); b.insert(b.end(), {0xB0, 80, 127});    // percussion on (part 0)
  vlq(b, 0); b.insert(b.end(), {0xB0, 7, 100});     // channel volume: not mapped
  vlq(b, 0); b.insert(b.end(), {0x90, 60, 100});
  vlq(b, 480); b.insert(b.end(), {0x80, 60, 0});
  vlq(b, 4800); b.insert(b.end(), {0xB0, 64, 127}); // pedal at the very end: neither maps nor extends the render
  track(f, b);
  SmfResult s = parse_smf(f);
  CHECK(s.ok && s.events.size() == 5);
  CHECK(s.events[0].cc && s.events[0].note == 80 && s.events[0].value == 127 && !s.events[0].on);
  CHECK(!s.events[2].cc && s.events[2].on);
  RenderOptions o; o.engine = EngineKind::kOrgan; o.tail_seconds = 0.0; o.drawbars = "000000000"; o.key_click = 0.0f;
  WavData a, c;
  std::string err;
  CHECK(render_events(s.events, o, a, err));
  CHECK(a.frames() <= 24000 + 64 + 64);  // the unmapped trailing controller did not lengthen the render
  // Percussion from the file (CC 80, third, soft default) is audible with every drawbar off; with the controller
  // removed and the option off there is nothing.
  double e = 0;
  for (float x : a.samples) e += static_cast<double>(x) * x;
  CHECK(e > 1e-4);
  std::vector<NoteEvent> notes;
  for (const NoteEvent& ev : s.events) if (!ev.cc) notes.push_back(ev);
  CHECK(render_events(notes, o, c, err));
  double ec = 0;  // only the 5 ms ramp down from the default 8' drawbar remains
  for (float x : c.samples) ec += static_cast<double>(x) * x;
  CHECK(ec < 0.1 * e);
  // The renderer option does what the controller does.
  RenderOptions p = o; p.percussion_on = true;
  WavData d;
  CHECK(render_events(notes, p, d, err));
  CHECK(d.samples == a.samples);
  // The option also bounds: second/fast/normal differs from third/fast/soft.
  p.percussion_harmonic = 0; p.percussion_volume = 1;
  WavData g;
  CHECK(render_events(notes, p, g, err));
  CHECK(g.samples != d.samples);
}

static void test_controllers_other_engines_and_limits() {
  // Non-organ renders ignore controllers entirely: same samples and length with or without them.
  Bytes f = header(0, 1, 480), b;
  vlq(b, 0); b.insert(b.end(), {0xB0, 80, 127});
  vlq(b, 0); b.insert(b.end(), {0xB0, 72, 20});
  vlq(b, 0); b.insert(b.end(), {0x90, 60, 100});
  vlq(b, 480); b.insert(b.end(), {0x80, 60, 0});
  vlq(b, 9600); b.insert(b.end(), {0xB0, 80, 0});
  track(f, b);
  SmfResult with = parse_smf(f);
  CHECK(with.ok);
  std::vector<NoteEvent> notes;
  for (const NoteEvent& e : with.events) if (!e.cc) notes.push_back(e);
  RenderOptions o; o.tail_seconds = 0.1;
  WavData x, y;
  std::string err;
  CHECK(render_events(with.events, o, x, err) && render_events(notes, o, y, err));
  CHECK(x.samples == y.samples);
  // Controllers on channels that are not organ parts are ignored by the organ render too.
  Bytes g = header(0, 1, 480), c;
  vlq(c, 0); c.insert(c.end(), {0x90, 60, 100});
  vlq(c, 480); c.insert(c.end(), {0x80, 60, 0});
  vlq(c, 9600); c.insert(c.end(), {0xB9, 80, 127});
  track(g, c);
  SmfResult hi = parse_smf(g);
  RenderOptions org; org.engine = EngineKind::kOrgan; org.tail_seconds = 0.1;
  WavData h, k;
  std::vector<NoteEvent> hn;
  for (const NoteEvent& e : hi.events) if (!e.cc) hn.push_back(e);
  CHECK(hi.ok && render_events(hi.events, org, h, err) && render_events(hn, org, k, err));
  CHECK(h.samples == k.samples);
  // Too many events for the first block together with the frame-0 organ setup: a clear error, not silently dropped ones.
  Bytes m = header(0, 1, 480), d;
  for (int i = 0; i < 300; ++i) { vlq(d, 0); d.insert(d.end(), {0xB0, static_cast<std::uint8_t>(70 + i % 9), static_cast<std::uint8_t>(i % 9)}); }
  vlq(d, 0); d.insert(d.end(), {0x90, 60, 100});
  track(m, d);
  SmfResult many = parse_smf(m);
  WavData z;
  CHECK(many.ok && !render_events(many.events, org, z, err) && err.find("overflow") != std::string::npos);
  // The setup alone (8 parts x 14 events) plus a normal first block renders fine.
  CHECK(note_smf().ok && render_events(note_smf().events, org, z, err));
  // Bad controller data: a data byte with the high bit set, or a truncated message, is an error like for notes.
  Bytes bad = header(0, 1, 480), e1;
  vlq(e1, 0); e1.insert(e1.end(), {0xB0, 80, 0x80});
  track(bad, e1);
  CHECK(!parse_smf(bad).ok);
  Bytes cut = header(0, 1, 480), e2;
  vlq(e2, 0); e2.insert(e2.end(), {0xB0, 80});
  track(cut, e2, false);
  CHECK(!parse_smf(cut).ok);
}

static void test_smf_more() {
  // meta event cancels running status: following bare data bytes are invalid
  Bytes f = header(0, 1, 480), b;
  vlq(b, 0); b.insert(b.end(), {0x90, 60, 100});
  vlq(b, 0); b.insert(b.end(), {0xFF, 0x01, 1, 'x'});
  vlq(b, 0); b.insert(b.end(), {60, 0});
  track(f, b);
  CHECK(!parse_smf(f).ok);
  Bytes g = header(0, 1, 480), c;
  vlq(c, 0); c.insert(c.end(), {0x90, 60, 100});
  vlq(c, 0); c.insert(c.end(), {0xF0, 1, 0xF7});
  vlq(c, 0); c.insert(c.end(), {60, 0});
  track(g, c);
  CHECK(!parse_smf(g).ok);
  Bytes z = header(1, 0, 480);
  CHECK(!parse_smf(z).ok);
  // 64-bit overflow of tick*tempo*rate is an error, not a wrapped frame number
  Bytes o = header(0, 1, 1), ob;
  tempo(ob, 0, 0xFFFFFF);
  for (int i = 0; i < 1000; ++i) { vlq(ob, 0x0FFFFFFF); ob.insert(ob.end(), {0x90, 60, 100}); }
  track(o, ob);
  SmfResult r = parse_smf(o);
  CHECK(!r.ok && r.error.find("overflow") != std::string::npos);
}

static void test_null_robustness() {
  NullParams p; p.tolerance_db = -90; p.max_offset_frames = 200;
  WavData a = noise(5000);
  WavData bad = a;
  bad.samples[100] = std::nanf("");
  NullResult r = null_test(a, bad, p);
  CHECK(!r.pass && !r.comparable && r.message.find("non-finite") != std::string::npos);
  r = null_test(bad, a, p);
  CHECK(!r.pass && !r.comparable);
  bad.samples[100] = INFINITY;
  CHECK(!null_test(a, bad, p).pass);
  // sound outside the overlap must not pass a silent reference
  WavData sil; sil.samples.assign(2000, 0.0f);
  WavData loud = sil; loud.samples.resize(2000 + 2000, 0.0f);
  for (std::size_t i = 2000; i < loud.samples.size(); ++i) loud.samples[i] = 0.2f;
  CHECK(!null_test(sil, loud, p).pass);
  // reference with a short burst then silence; silent candidate must fail even though some
  // offsets overlap only silence
  WavData burst = noise(100);
  burst.samples.resize(5100 * 2, 0.0f);
  WavData zeros; zeros.samples.assign(5100 * 2, 0.0f);
  r = null_test(burst, zeros, p);
  CHECK(!r.pass && r.residual_db > -1.0);
  // delay beyond max on material with silent edges still fails
  WavData edge; edge.samples.assign(3000 * 2, 0.0f);
  WavData mid = noise(1000, 3);
  edge.samples.insert(edge.samples.end(), mid.samples.begin(), mid.samples.end());
  edge.samples.resize(edge.samples.size() + 3000 * 2, 0.0f);
  NullParams q = p; q.max_offset_frames = 16;
  CHECK(null_test(edge, delayed(edge, 10), q).pass);
  CHECK(!null_test(edge, delayed(edge, 200), q).pass);
}

static void test_null_length_limit() {
  NullParams p; p.tolerance_db = -90; p.max_length_diff_frames = 0;
  WavData a = noise(1000), b = a;
  CHECK(null_test(a, b, p).pass);
  b.samples.resize(1600);
  NullResult r = null_test(a, b, p);
  CHECK(!r.pass && r.comparable && r.length_diff_frames == -200);
  p.max_length_diff_frames = 200;
  CHECK(null_test(a, b, p).pass);
  p.window_frames = 9600;
  CHECK(format_report("x", null_test(a, b, p), p).find("200 ms") != std::string::npos);
}

static void test_render_frame_count() {
  Bytes f = header(0, 1, 480), b;
  vlq(b, 0); b.insert(b.end(), {0x90, 60, 100});
  vlq(b, 480); b.insert(b.end(), {0x80, 60, 0});
  track(f, b);
  SmfResult s = parse_smf(f);
  RenderOptions o; o.tail_seconds = 0.1;
  WavData w;
  std::string err;
  CHECK(s.ok && render_events(s.events, o, w, err));
  // last event at frame 24000 + 4800 tail = 28800 -> (28800 + 64) / 64 = 451 blocks
  CHECK(w.frames() == 451 * 64);
}


static Bytes pcm16_wav(const std::vector<std::int16_t>& s, int channels) {
  Bytes v{'R', 'I', 'F', 'F'};
  auto le32 = [&](std::uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i))); };
  auto le16 = [&](std::uint32_t x) { for (int i = 0; i < 2; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i))); };
  le32(static_cast<std::uint32_t>(36 + s.size() * 2));
  v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  le32(16); le16(1); le16(channels); le32(48000); le32(48000 * channels * 2); le16(channels * 2); le16(16);
  v.insert(v.end(), {'d', 'a', 't', 'a'});
  le32(static_cast<std::uint32_t>(s.size() * 2));
  for (std::int16_t x : s) le16(static_cast<std::uint16_t>(x));
  return v;
}

static void test_pcm16_and_sfizz_decode() {
  const std::vector<std::int16_t> s = {-32768, -1, 0, 1, 32767, 1000};
  WavData w;
  std::string err;
  CHECK(decode_wav(pcm16_wav(s, 2), w, err) && w.channels == 2 && w.frames() == 3 && w.from_pcm16);
  for (std::size_t i = 0; i < s.size(); ++i) CHECK(w.samples[i] == static_cast<float>(s[i]) / 32768.0f);
  // sfizz_render's conversion is s = trunc((x + 1) * 32767.5) - 32768; the undo is (s + 1) / 32767.5.
  CHECK(undo_sfizz_s16(w, err));
  for (std::size_t i = 0; i < s.size(); ++i)
    CHECK(std::fabs(w.samples[i] - (static_cast<double>(s[i]) + 1.0) / 32767.5) < 1e-6);
  CHECK(w.samples[1] == 0.0f);  // -1 is silence
  // A float WAV is not sfizz output: decoding it that way is an error.
  WavData f;
  f.channels = 1;
  f.samples = {0.1f, 0.2f};
  WavData g;
  CHECK(decode_wav(encode_wav(f), g, err) && !g.from_pcm16);
  CHECK(!undo_sfizz_s16(g, err) && !err.empty());
  // 8-bit PCM stays unsupported.
  Bytes bad = pcm16_wav(s, 2);
  bad[34] = 8;
  CHECK(!decode_wav(bad, g, err));
}

static void test_null_check_excess() {
  WavData ref, cand;
  ref.channels = cand.channels = 1;
  for (int i = 0; i < 1000; ++i) ref.samples.push_back(0.5f * std::sin(0.05f * static_cast<float>(i)));
  cand = ref;
  for (int i = 0; i < 500; ++i) cand.samples.push_back(0.4f);  // a tail the reference lacks
  NullParams p;
  p.tolerance_db = -60.0;
  const NullResult off = null_test(ref, cand, p);
  CHECK(off.pass && off.residual_db <= -100.0);  // the overlap alone is perfect...
  p.check_excess = true;
  const NullResult on = null_test(ref, cand, p);
  CHECK(!on.pass && on.excess_db > -60.0 && on.message.find("outside") != std::string::npos);  // ...the tail is not
  // The reverse: a candidate that stops early lacks reference signal.
  WavData shortc = ref;
  shortc.samples.resize(500);
  CHECK(null_test(ref, shortc, p).pass == false);
  p.check_excess = false;
  CHECK(null_test(ref, shortc, p).pass);
  // Equal files pass with the check on.
  p.check_excess = true;
  CHECK(null_test(ref, ref, p).pass);
}

int main() {
  test_pcm16_and_sfizz_decode();
  test_null_check_excess();
  test_smf_more();
  test_smf_controllers_and_organ_options();
  test_controllers_other_engines_and_limits();
  test_null_robustness();
  test_null_length_limit();
  test_render_frame_count();
  test_smf_basic();
  test_smf_running_status_vel0_long_delta();
  test_smf_tempo_map();
  test_smf_malformed();
  test_wav_roundtrip();
  test_null_identical_and_offset();
  test_null_mismatch_and_silence();
  test_worst_window();
  test_render_determinism_and_onsets();
#ifdef OFFLINE_RENDER_EXE
  test_cli_malformed_no_output();
#endif
  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("offline_render_tests: all passed\n");
  return 0;
}
