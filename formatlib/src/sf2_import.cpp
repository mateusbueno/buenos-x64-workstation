#include "velora/format/sf2_import.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

// Clean-room reader written from the public SoundFont 2.04 specification (RIFF layout, section 7
// "SoundFont 2 Data Structure" for the record layouts, section 8 for generators and modulators).
// Nothing here is derived from any SF2 library.

namespace velora::format {
namespace {

// ---------------------------------------------------------------------------------------------
// Byte sources

class MemSource final : public ByteSource {
 public:
  explicit MemSource(std::span<const std::uint8_t> b) : b_(b) {}
  std::uint64_t size() const override { return b_.size(); }
  const std::uint8_t* direct(std::uint64_t off, std::size_t n) const override {
    return (off <= b_.size() && n <= b_.size() - off) ? b_.data() + off : nullptr;
  }
  Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    const auto* p = direct(off, n);
    if (!p) return Error::kTruncated;
    if (n) std::memcpy(dst, p, n);
    return Error::kNone;
  }

 private:
  std::span<const std::uint8_t> b_;
};

class FileSrc final : public ByteSource {
 public:
  FileSrc(int fd, std::uint64_t size) : fd_(fd), size_(size) {}
  ~FileSrc() override { ::close(fd_); }
  std::uint64_t size() const override { return size_; }
  const std::uint8_t* direct(std::uint64_t, std::size_t) const override { return nullptr; }
  Error read_at(std::uint64_t off, void* dst, std::size_t n) const override {
    if (off > size_ || n > size_ - off) return Error::kTruncated;
    auto* p = static_cast<std::uint8_t*>(dst);
    while (n > 0) {
      const ssize_t r = ::pread(fd_, p, n, static_cast<off_t>(off));
      if (r < 0) {
        if (errno == EINTR) continue;
        return Error::kIo;
      }
      if (r == 0) return Error::kTruncated;
      p += r;
      off += static_cast<std::uint64_t>(r);
      n -= static_cast<std::size_t>(r);
    }
    return Error::kNone;
  }

 private:
  int fd_;
  std::uint64_t size_;
};

inline std::uint16_t u16le(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
inline std::uint32_t u32le(const std::uint8_t* p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

// ---------------------------------------------------------------------------------------------
// Generator table (section 8.1.2). Ranges are the specification's; defaults are the values a
// generator has when no zone sets it.

enum GenId : unsigned {
  kStartAddrsOffset = 0, kEndAddrsOffset = 1, kStartloopAddrsOffset = 2, kEndloopAddrsOffset = 3,
  kStartAddrsCoarse = 4, kModLfoToPitch = 5, kVibLfoToPitch = 6, kModEnvToPitch = 7,
  kInitialFilterFc = 8, kInitialFilterQ = 9, kModLfoToFilterFc = 10, kModEnvToFilterFc = 11,
  kEndAddrsCoarse = 12, kModLfoToVolume = 13, kChorusSend = 15, kReverbSend = 16, kPan = 17,
  kDelayVolEnv = 33, kAttackVolEnv = 34, kHoldVolEnv = 35, kDecayVolEnv = 36, kSustainVolEnv = 37,
  kReleaseVolEnv = 38, kInstrument = 41, kKeyRange = 43, kVelRange = 44, kStartloopCoarse = 45,
  kKeynum = 46, kVelocity = 47, kInitialAttenuation = 48, kEndloopCoarse = 50, kCoarseTune = 51,
  kFineTune = 52, kSampleID = 53, kSampleModes = 54, kScaleTuning = 56, kExclusiveClass = 57,
  kOverridingRootKey = 58,
};
constexpr unsigned kNumGens = 61;  // operators 0..60 (60 = endOper)

struct GenInfo {
  const char* name;
  int lo, hi, def;
};
constexpr int kI16Min = -32768, kI16Max = 32767;
const GenInfo kGens[kNumGens] = {
    {"startAddrsOffset", kI16Min, kI16Max, 0}, {"endAddrsOffset", kI16Min, kI16Max, 0},
    {"startloopAddrsOffset", kI16Min, kI16Max, 0}, {"endloopAddrsOffset", kI16Min, kI16Max, 0},
    {"startAddrsCoarseOffset", kI16Min, kI16Max, 0}, {"modLfoToPitch", -12000, 12000, 0},
    {"vibLfoToPitch", -12000, 12000, 0}, {"modEnvToPitch", -12000, 12000, 0},
    {"initialFilterFc", 1500, 13500, 13500}, {"initialFilterQ", 0, 960, 0},
    {"modLfoToFilterFc", -12000, 12000, 0}, {"modEnvToFilterFc", -12000, 12000, 0},
    {"endAddrsCoarseOffset", kI16Min, kI16Max, 0}, {"modLfoToVolume", -960, 960, 0},
    {"unused1", kI16Min, kI16Max, 0}, {"chorusEffectsSend", 0, 1000, 0},
    {"reverbEffectsSend", 0, 1000, 0}, {"pan", -500, 500, 0},
    {"unused2", kI16Min, kI16Max, 0}, {"unused3", kI16Min, kI16Max, 0},
    {"unused4", kI16Min, kI16Max, 0}, {"delayModLFO", -12000, 5000, -12000},
    {"freqModLFO", -16000, 4500, 0}, {"delayVibLFO", -12000, 5000, -12000},
    {"freqVibLFO", -16000, 4500, 0}, {"delayModEnv", -12000, 5000, -12000},
    {"attackModEnv", -12000, 8000, -12000}, {"holdModEnv", -12000, 5000, -12000},
    {"decayModEnv", -12000, 8000, -12000}, {"sustainModEnv", 0, 1000, 0},
    {"releaseModEnv", -12000, 8000, -12000}, {"keynumToModEnvHold", -1200, 1200, 0},
    {"keynumToModEnvDecay", -1200, 1200, 0}, {"delayVolEnv", -12000, 5000, -12000},
    {"attackVolEnv", -12000, 8000, -12000}, {"holdVolEnv", -12000, 5000, -12000},
    {"decayVolEnv", -12000, 8000, -12000}, {"sustainVolEnv", 0, 1440, 0},
    {"releaseVolEnv", -12000, 8000, -12000}, {"keynumToVolEnvHold", -1200, 1200, 0},
    {"keynumToVolEnvDecay", -1200, 1200, 0}, {"instrument", 0, 65535, 0},
    {"reserved1", kI16Min, kI16Max, 0}, {"keyRange", 0, 65535, 0}, {"velRange", 0, 65535, 0},
    {"startloopAddrsCoarseOffset", kI16Min, kI16Max, 0}, {"keynum", 0, 127, -1},
    {"velocity", 1, 127, -1}, {"initialAttenuation", 0, 1440, 0}, {"reserved2", kI16Min, kI16Max, 0},
    {"endloopAddrsCoarseOffset", kI16Min, kI16Max, 0}, {"coarseTune", -120, 120, 0},
    {"fineTune", -99, 99, 0}, {"sampleID", 0, 65535, 0}, {"sampleModes", 0, 3, 0},
    {"reserved3", kI16Min, kI16Max, 0}, {"scaleTuning", 0, 1200, 100},
    {"exclusiveClass", 0, 127, 0}, {"overridingRootKey", 0, 127, -1},
    {"unused5", kI16Min, kI16Max, 0}, {"endOper", kI16Min, kI16Max, 0},
};

// Generators that exist only at instrument level (section 8.1.2: "not valid at preset level").
bool inst_only(unsigned g) {
  switch (g) {
    case 0: case 1: case 2: case 3: case 4: case 12: case 45: case 46: case 47: case 50: case 54:
    case 57: case 58:
      return true;
    default: return false;
  }
}
// Generators handled by the mapping.
bool mapped(unsigned g) {
  switch (g) {
    case kStartAddrsOffset: case kStartloopAddrsOffset: case kEndloopAddrsOffset: case kStartAddrsCoarse:
    case kInitialFilterFc: case kInitialFilterQ: case kPan: case kDelayVolEnv: case kAttackVolEnv:
    case kHoldVolEnv: case kDecayVolEnv: case kSustainVolEnv: case kReleaseVolEnv:
    case kStartloopCoarse: case kInitialAttenuation: case kEndloopCoarse: case kCoarseTune:
    case kFineTune: case kSampleModes: case kOverridingRootKey:
      return true;
    default: return false;
  }
}
// Why a generator the model lacks is ignored (nullptr = it is not one of the known ignored ones).
const char* ignore_reason(unsigned g) {
  switch (g) {
    case 5: case 6: case 10: case 13: case 21: case 22: case 23: case 24: return "LFOs are not in the instrument model";
    case 7: case 11: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32:
      return "the modulation envelope is not in the instrument model";
    case 39: case 40: return "key number scaling of the volume envelope is not in the instrument model";
    case 15: case 16: return "effects sends are not in the instrument model";
    case 46: case 47: return "key number / velocity overrides are not in the instrument model";
    case 56: return "scaleTuning is not in the instrument model (pitch follows 100 cents per key)";
    case 57: return "exclusive classes are not in the instrument model";
    case 1: case 12: return "the instrument model cannot cut a sample short";
    default: return nullptr;
  }
}

// ---------------------------------------------------------------------------------------------
// Records

struct PhdrRec { std::string name; std::uint16_t preset = 0, bank = 0, bag = 0; };
struct BagRec { std::uint16_t gen = 0, mod = 0; };
struct ModRec { std::uint16_t src = 0, dest = 0; std::int16_t amount = 0; std::uint16_t amt_src = 0, trans = 0; };
struct GenRec { std::uint16_t oper = 0, amount = 0; };
struct InstRec { std::string name; std::uint16_t bag = 0; };
struct ShdrRec {
  std::string name;
  std::uint32_t start = 0, end = 0, startloop = 0, endloop = 0, rate = 0;
  std::uint8_t pitch = 0;
  std::int8_t correction = 0;
  std::uint16_t link = 0, type = 0;
};

// The ten default modulators of section 8.4 (identity + amount). A pmod/imod entry equal to one of
// these is the default behaviour, not a custom modulator.
struct DefMod { std::uint16_t src, dest; std::int16_t amount; std::uint16_t amt_src, trans; };
constexpr DefMod kDefaultMods[] = {
    {0x0502, 48, 960, 0x0000, 0},    // velocity -> initial attenuation
    {0x0102, 8, -2400, 0x0000, 0},   // velocity -> initial filter cutoff
    {0x000D, 6, 50, 0x0000, 0},      // channel pressure -> vibrato LFO pitch depth
    {0x0081, 6, 50, 0x0000, 0},      // CC1 -> vibrato LFO pitch depth
    {0x0587, 48, 960, 0x0000, 0},    // CC7 -> initial attenuation
    {0x028A, 17, 1000, 0x0000, 0},   // CC10 -> pan
    {0x058B, 48, 960, 0x0000, 0},    // CC11 -> initial attenuation
    {0x00DB, 16, 200, 0x0000, 0},    // CC91 -> reverb send
    {0x00DD, 15, 200, 0x0000, 0},    // CC93 -> chorus send
    {0x020E, 52, 12700, 0x0010, 0},  // pitch wheel -> initial pitch, scaled by the bend sensitivity
};
bool same_identity(const ModRec& a, const ModRec& b) {
  return a.src == b.src && a.dest == b.dest && a.amt_src == b.amt_src && a.trans == b.trans;
}
bool is_default_mod(const ModRec& m) {
  for (const DefMod& d : kDefaultMods)
    if (m.src == d.src && m.dest == d.dest && m.amt_src == d.amt_src && m.trans == d.trans && m.amount == d.amount)
      return true;
  return false;
}

// ---------------------------------------------------------------------------------------------
// Text

std::size_t utf8_len(const std::string& s, std::size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) return 1;
  std::size_t n = 0;
  std::uint32_t cp = 0, min = 0;
  if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; min = 0x80; }
  else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; min = 0x800; }
  else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; min = 0x10000; }
  else return 0;
  if (i + n > s.size()) return 0;
  for (std::size_t k = 1; k < n; ++k) {
    const unsigned char d = static_cast<unsigned char>(s[i + k]);
    if ((d & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (d & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return n;
}

// A fixed-size SF2 name (at most 20 bytes, NUL terminated or full) as valid UTF-8: valid UTF-8
// sequences are kept, any other byte is read as Latin-1, control characters become blanks, and
// trailing blanks are dropped.
std::string name_utf8(const std::uint8_t* p, std::size_t n) {
  std::size_t len = 0;
  while (len < n && p[len] != 0) ++len;
  const std::string raw(reinterpret_cast<const char*>(p), len);
  std::string out;
  for (std::size_t i = 0; i < raw.size();) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    const std::size_t k = utf8_len(raw, i);
    if (k == 1) {
      out.push_back((c < 0x20 || c == 0x7f) ? ' ' : static_cast<char>(c));
      ++i;
    } else if (k > 1) {
      out.append(raw, i, k);
      i += k;
    } else {  // not UTF-8: a Latin-1 byte
      if (c >= 0xA0) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
      } else {
        out.push_back('?');
      }
      ++i;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// ---------------------------------------------------------------------------------------------
// The samples handle

class SamplesImpl final : public Sf2Samples {
 public:
  SamplesImpl(std::shared_ptr<const ByteSource> src, std::uint64_t smpl_off, std::uint64_t frames,
              std::uint64_t sm24_off, bool has24)
      : src_(std::move(src)), smpl_off_(smpl_off), frames_(frames), sm24_off_(sm24_off), has24_(has24) {}

  Error read(const Sf2SampleRef& ref, std::vector<std::int32_t>& out) const override {
    out.clear();
    if (ref.first_frame > frames_ || ref.frames > frames_ - ref.first_frame) return Error::kOutOfRange;
    if (ref.format.channels != 1 || ref.format.is_float ||
        ref.format.bits_per_sample != (has24_ ? 24 : 16))
      return Error::kInvalidArgument;
    out.resize(ref.frames);
    constexpr std::uint64_t kMaxChunk = 1u << 15;
    const std::size_t kChunk = static_cast<std::size_t>(std::min<std::uint64_t>(kMaxChunk, std::max<std::uint64_t>(ref.frames, 1)));
    std::vector<std::uint8_t> w(kChunk * 2), l(has24_ ? kChunk : 0);
    for (std::uint64_t done = 0; done < ref.frames;) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, ref.frames - done));
      const std::uint64_t at = ref.first_frame + done;
      if (Error e = src_->read_at(smpl_off_ + at * 2, w.data(), n * 2); e != Error::kNone) { out.clear(); return e; }
      if (has24_) {
        if (Error e = src_->read_at(sm24_off_ + at, l.data(), n); e != Error::kNone) { out.clear(); return e; }
      }
      for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t hi = static_cast<std::int16_t>(u16le(&w[i * 2]));
        out[done + i] = has24_ ? (hi * 256 + l[i]) : hi;
      }
      done += n;
    }
    return Error::kNone;
  }

 private:
  std::shared_ptr<const ByteSource> src_;
  std::uint64_t smpl_off_, frames_, sm24_off_;
  bool has24_;
};

// ---------------------------------------------------------------------------------------------
// One zone's generators (a bag, read up to and including its terminal generator).

struct GenSet {
  std::uint64_t has = 0;           // bit g: generator g was given (not the ranges / the terminal)
  std::int16_t v[kNumGens] = {};   // raw amounts as signed 16-bit
  std::uint8_t kr_lo = 0, kr_hi = 127, vr_lo = 0, vr_hi = 127;
  bool has_kr = false, has_vr = false, has_term = false;
  std::uint16_t term = 0;
  std::uint64_t stray = 0;         // bit g: a terminal generator of the other level (instrument in igen, sampleID in pgen)
  std::uint16_t high[4] = {};      // unknown operator numbers >= 61 (first four)
  std::uint8_t n_high = 0;
  std::uint32_t mod_begin = 0, mod_end = 0;
  bool given(unsigned g) const { return (has >> g) & 1; }
};

struct Aggregate {
  std::string item, message;
  std::size_t zones = 0;
};

class Parser {
 public:
  explicit Parser(std::shared_ptr<const ByteSource> src) : src_(std::move(src)) {}
  Sf2ParseResult run();

 private:
  // --- failure handling
  bool fail(std::string msg) {
    if (!damaged_) damaged_msg_ = std::move(msg);
    damaged_ = true;
    return false;
  }
  bool read(std::uint64_t off, void* dst, std::size_t n, const char* what) {
    const Error e = src_->read_at(off, dst, n);
    if (e == Error::kNone) return true;
    return fail(std::string("cannot read ") + what + " at offset " + std::to_string(off) + " (" + to_string(e) + ")");
  }

  struct ChunkHdr {
    char id[5] = {};
    std::uint64_t pos = 0, data = 0, size = 0;
    bool is(const char* s) const { return std::memcmp(id, s, 4) == 0; }
  };
  // Next chunk of the container [pos, end); false at the end or on damage (damaged_ set).
  bool next_chunk(std::uint64_t& pos, std::uint64_t end, ChunkHdr& h) {
    if (pos >= end) return false;
    if (end - pos < 8) {  // fewer than 8 bytes of padding/slack at the end of a container
      pos = end;
      return false;
    }
    std::uint8_t b[8];
    if (!read(pos, b, 8, "chunk header")) return false;
    std::memcpy(h.id, b, 4);
    h.id[4] = 0;
    for (int i = 0; i < 4; ++i)
      if (b[i] < 0x20 || b[i] > 0x7e) h.id[i] = '?';
    h.pos = pos;
    h.data = pos + 8;
    h.size = u32le(b + 4);
    if (h.size > end - h.data)
      return fail(std::string("chunk '") + h.id + "' at offset " + std::to_string(pos) + " has size " +
                  std::to_string(h.size) + " which exceeds its container (" + std::to_string(end - h.data) + " bytes left)");
    const std::uint64_t padded = h.data + h.size + (h.size & 1);
    pos = padded > end ? end : padded;  // a missing final pad byte is tolerated
    return true;
  }

  bool parse_riff();
  bool parse_info(std::uint64_t b, std::uint64_t e);
  bool parse_sdta(std::uint64_t b, std::uint64_t e);
  bool parse_pdta(std::uint64_t b, std::uint64_t e);
  bool load_tables();
  bool check_structure();
  bool check_samples();
  bool build_presets();

  // zone handling
  void fill_gens(const std::vector<GenRec>& gens, const std::vector<BagRec>& bags, std::size_t bag,
                 bool inst_level, GenSet& out);
  struct PresetAgg {
    std::vector<Aggregate> gen, mod, zone;
    std::map<std::string, std::size_t> gi, mi, zi;
  };

  bool map_pair(const GenSet& pz, const GenSet* pg, const GenSet& iz, const GenSet* ig, Sf2Preset& out,
                std::vector<int>& local, PresetAgg& agg);
  void note_ignored(std::vector<Aggregate>& agg, std::map<std::string, std::size_t>& idx, const std::string& item,
                    const std::string& message);

  std::shared_ptr<const ByteSource> src_;
  bool damaged_ = false;
  std::string damaged_msg_;
  std::vector<Sf2Issue> unsupported_;

  // chunk locations (offset, size); size < 0 style via flags
  struct Loc { std::uint64_t off = 0, size = 0; bool present = false; };
  Loc ifil_, smpl_, sm24_;
  Loc tbl_[9];  // phdr pbag pmod pgen inst ibag imod igen shdr
  bool have_info_ = false, have_sdta_ = false, have_pdta_ = false;
  unsigned ifil_major_ = 0, ifil_minor_ = 0;

  std::vector<PhdrRec> phdr_;
  std::vector<BagRec> pbag_, ibag_;
  std::vector<ModRec> pmod_, imod_;
  std::vector<GenRec> pgen_, igen_;
  std::vector<InstRec> inst_;
  std::vector<ShdrRec> shdr_;
  std::uint64_t smpl_frames_ = 0;
  bool has24_ = false;

  // per sample (shdr) verdict for referenced samples, computed lazily: 0 unknown, 1 ok, 2 unsupported
  std::vector<std::uint8_t> sample_state_;
  std::vector<std::uint32_t> inst_zones_;  // zones with a sampleID per instrument

  struct InstCache {
    bool has_global = false;
    GenSet global;
    std::vector<GenSet> zones;  // zones with a sampleID, in file order
  };
  std::vector<InstCache> icache_;
  std::uint64_t gen_scans_ = 0;

  Sf2ParseResult res_;
};

const char* const kTableNames[9] = {"phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"};
const std::size_t kTableRec[9] = {38, 4, 10, 4, 22, 4, 10, 4, 46};

bool Parser::parse_riff() {
  const std::uint64_t size = src_->size();
  if (size < 12) return fail("file is " + std::to_string(size) + " bytes, shorter than a RIFF header");
  std::uint8_t h[12];
  if (!read(0, h, 12, "RIFF header")) return false;
  if (std::memcmp(h, "RIFF", 4) != 0) return fail("not a RIFF file (no 'RIFF' magic)");
  const std::uint64_t riff = u32le(h + 4);
  if (riff < 4 || riff > size - 8)
    return fail("RIFF size " + std::to_string(riff) + " does not fit the file (" + std::to_string(size) + " bytes)");
  if (std::memcmp(h + 8, "sfbk", 4) != 0) return fail("not a SoundFont 2 file (RIFF form type is not 'sfbk')");
  std::uint64_t pos = 12;
  const std::uint64_t end = 8 + riff;
  ChunkHdr c;
  while (next_chunk(pos, end, c)) {
    if (!c.is("LIST")) continue;  // unknown top-level chunks are skipped
    if (c.size < 4) return fail("LIST chunk at offset " + std::to_string(c.pos) + " is shorter than its type");
    char type[5] = {};
    if (!read(c.data, type, 4, "LIST type")) return false;
    const std::uint64_t b = c.data + 4, e = c.data + c.size;
    if (std::memcmp(type, "INFO", 4) == 0) {
      if (have_info_) return fail("repeated INFO list");
      have_info_ = true;
      if (!parse_info(b, e)) return false;
    } else if (std::memcmp(type, "sdta", 4) == 0) {
      if (have_sdta_) return fail("repeated sdta list");
      have_sdta_ = true;
      if (!parse_sdta(b, e)) return false;
    } else if (std::memcmp(type, "pdta", 4) == 0) {
      if (have_pdta_) return fail("repeated pdta list");
      have_pdta_ = true;
      if (!parse_pdta(b, e)) return false;
    }
  }
  if (damaged_) return false;
  if (!have_info_) return fail("missing INFO list");
  if (!ifil_.present) return fail("missing 'ifil' chunk in INFO");
  if (!have_sdta_) return fail("missing sdta list");
  if (!have_pdta_) return fail("missing pdta list");
  for (int i = 0; i < 9; ++i)
    if (!tbl_[i].present) return fail(std::string("missing '") + kTableNames[i] + "' chunk in pdta");
  return true;
}

bool Parser::parse_info(std::uint64_t b, std::uint64_t e) {
  std::uint64_t pos = b;
  ChunkHdr c;
  while (next_chunk(pos, e, c)) {
    if (!c.is("ifil")) continue;
    if (ifil_.present) return fail("repeated 'ifil' chunk");
    if (c.size != 4) return fail("'ifil' chunk has size " + std::to_string(c.size) + ", expected 4");
    std::uint8_t v[4];
    if (!read(c.data, v, 4, "ifil")) return false;
    ifil_ = {c.data, c.size, true};
    ifil_major_ = u16le(v);
    ifil_minor_ = u16le(v + 2);
  }
  return !damaged_;
}

bool Parser::parse_sdta(std::uint64_t b, std::uint64_t e) {
  std::uint64_t pos = b;
  ChunkHdr c;
  while (next_chunk(pos, e, c)) {
    if (c.is("smpl")) {
      if (smpl_.present) return fail("repeated 'smpl' chunk");
      smpl_ = {c.data, c.size, true};
    } else if (c.is("sm24")) {
      if (sm24_.present) return fail("repeated 'sm24' chunk");
      sm24_ = {c.data, c.size, true};
    }
  }
  return !damaged_;
}

bool Parser::parse_pdta(std::uint64_t b, std::uint64_t e) {
  std::uint64_t pos = b;
  ChunkHdr c;
  while (next_chunk(pos, e, c)) {
    for (int i = 0; i < 9; ++i) {
      if (!c.is(kTableNames[i])) continue;
      if (tbl_[i].present) return fail(std::string("repeated '") + kTableNames[i] + "' chunk");
      tbl_[i] = {c.data, c.size, true};
    }
  }
  return !damaged_;
}

bool Parser::load_tables() {
  // sdta first: smpl frames and the optional sm24.
  if (smpl_.present) {
    smpl_frames_ = smpl_.size / 2;  // an odd byte size loses its last byte
  }
  if (sm24_.present) {
    const std::uint64_t even = smpl_frames_ + (smpl_frames_ & 1);
    if (!smpl_.present) return fail("'sm24' chunk without 'smpl'");
    if (sm24_.size != smpl_frames_ && sm24_.size != even)
      return fail("'sm24' chunk has size " + std::to_string(sm24_.size) + " but 'smpl' holds " +
                  std::to_string(smpl_frames_) + " samples");
    has24_ = true;
  }
  if (ifil_major_ != 2) {
    res_.stats.bits24 = false;
    Sf2Issue is;
    is.kind = Sf2IssueKind::kUnsupported;
    is.item = "ifil";
    is.message = "SoundFont version " + std::to_string(ifil_major_) + "." + std::to_string(ifil_minor_) +
                 " is not supported (only major version 2)";
    unsupported_.push_back(is);
    return false;
  }

  std::vector<std::uint8_t> raw;
  auto table = [&](int i, auto&& decode) -> bool {
    const Loc& t = tbl_[i];
    const std::size_t rec = kTableRec[i];
    if (t.size % rec != 0 || t.size < rec)
      return fail(std::string("'") + kTableNames[i] + "' chunk has size " + std::to_string(t.size) +
                  ", not a positive multiple of " + std::to_string(rec));
    if (t.size > (64u << 20)) return fail(std::string("'") + kTableNames[i] + "' chunk is larger than 64 MiB");
    raw.resize(t.size);
    if (!read(t.off, raw.data(), raw.size(), kTableNames[i])) return false;
    for (std::size_t k = 0; k < t.size / rec; ++k) decode(&raw[k * rec]);
    return true;
  };
  if (!table(0, [&](const std::uint8_t* p) {
        PhdrRec r;
        r.name = name_utf8(p, 20);
        r.preset = u16le(p + 20);
        r.bank = u16le(p + 22);
        r.bag = u16le(p + 24);
        phdr_.push_back(std::move(r));
      })) return false;
  auto bag = [](std::vector<BagRec>& v) {
    return [&v](const std::uint8_t* p) { v.push_back({u16le(p), u16le(p + 2)}); };
  };
  auto mod = [](std::vector<ModRec>& v) {
    return [&v](const std::uint8_t* p) {
      v.push_back({u16le(p), u16le(p + 2), static_cast<std::int16_t>(u16le(p + 4)), u16le(p + 6), u16le(p + 8)});
    };
  };
  auto gen = [](std::vector<GenRec>& v) {
    return [&v](const std::uint8_t* p) { v.push_back({u16le(p), u16le(p + 2)}); };
  };
  if (!table(1, bag(pbag_)) || !table(2, mod(pmod_)) || !table(3, gen(pgen_))) return false;
  if (!table(4, [&](const std::uint8_t* p) {
        InstRec r;
        r.name = name_utf8(p, 20);
        r.bag = u16le(p + 20);
        inst_.push_back(std::move(r));
      })) return false;
  if (!table(5, bag(ibag_)) || !table(6, mod(imod_)) || !table(7, gen(igen_))) return false;
  return table(8, [&](const std::uint8_t* p) {
    ShdrRec r;
    r.name = name_utf8(p, 20);
    r.start = u32le(p + 20);
    r.end = u32le(p + 24);
    r.startloop = u32le(p + 28);
    r.endloop = u32le(p + 32);
    r.rate = u32le(p + 36);
    r.pitch = p[40];
    r.correction = static_cast<std::int8_t>(p[41]);
    r.link = u16le(p + 42);
    r.type = u16le(p + 44);
    shdr_.push_back(std::move(r));
  });
}

bool Parser::check_structure() {
  // The last record of every table is a terminal record; the index fields of every record are
  // non-decreasing and the terminal's index is the position of the next table's terminal.
  const std::size_t n_phdr = phdr_.size(), n_pbag = pbag_.size(), n_pgen = pgen_.size(), n_pmod = pmod_.size();
  const std::size_t n_inst = inst_.size(), n_ibag = ibag_.size(), n_igen = igen_.size(), n_imod = imod_.size();
  auto mono = [&](const char* what, std::size_t count, auto&& key, std::size_t terminal_target, const char* next) {
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t k = key(i);
      if (i + 1 < count && k > key(i + 1))
        return fail(std::string(what) + " record " + std::to_string(i) + " indexes " + std::to_string(k) + ", after its successor " + std::to_string(key(i + 1)));
      if (k > terminal_target)
        return fail(std::string(what) + " record " + std::to_string(i) + " indexes " + std::to_string(k) + " beyond the " + next + " table (" + std::to_string(terminal_target) + " records)");
    }
    if (key(count - 1) != terminal_target)
      return fail(std::string("terminal ") + what + " record indexes " + std::to_string(key(count - 1)) + ", expected " + std::to_string(terminal_target) + " (the terminal of the " + next + " table)");
    return true;
  };
  if (!mono("phdr", n_phdr, [&](std::size_t i) { return std::size_t(phdr_[i].bag); }, n_pbag - 1, "pbag")) return false;
  if (!mono("pbag generator", n_pbag, [&](std::size_t i) { return std::size_t(pbag_[i].gen); }, n_pgen - 1, "pgen")) return false;
  if (!mono("pbag modulator", n_pbag, [&](std::size_t i) { return std::size_t(pbag_[i].mod); }, n_pmod - 1, "pmod")) return false;
  if (!mono("inst", n_inst, [&](std::size_t i) { return std::size_t(inst_[i].bag); }, n_ibag - 1, "ibag")) return false;
  if (!mono("ibag generator", n_ibag, [&](std::size_t i) { return std::size_t(ibag_[i].gen); }, n_igen - 1, "igen")) return false;
  if (!mono("ibag modulator", n_ibag, [&](std::size_t i) { return std::size_t(ibag_[i].mod); }, n_imod - 1, "imod")) return false;
  if (n_phdr < 2) return fail("the file has no presets (phdr holds only its terminal record)");
  return true;
}

bool Parser::check_samples() {
  // Sample headers are examined lazily, only when a zone references them (map_pair).
  sample_state_.assign(shdr_.size() - 1, 0);
  return true;
}

void Parser::fill_gens(const std::vector<GenRec>& gens, const std::vector<BagRec>& bags, std::size_t bag,
                       bool inst_level, GenSet& out) {
  out = GenSet{};
  gen_scans_ += bags[bag + 1].gen - bags[bag].gen;
  const unsigned terminal = inst_level ? kSampleID : kInstrument;
  const unsigned wrong = inst_level ? kInstrument : kSampleID;
  out.mod_begin = bags[bag].mod;
  out.mod_end = bags[bag + 1].mod;
  for (std::size_t i = bags[bag].gen; i < bags[bag + 1].gen; ++i) {
    const unsigned op = gens[i].oper;
    const std::uint16_t amt = gens[i].amount;
    if (op == terminal) {
      out.has_term = true;
      out.term = amt;
      break;  // generators after the terminal one are ignored
    }
    if (op == wrong) { out.stray |= std::uint64_t(1) << op; continue; }
    if (op == kKeyRange) { out.has_kr = true; out.kr_lo = static_cast<std::uint8_t>(amt & 0xff); out.kr_hi = static_cast<std::uint8_t>(amt >> 8); continue; }
    if (op == kVelRange) { out.has_vr = true; out.vr_lo = static_cast<std::uint8_t>(amt & 0xff); out.vr_hi = static_cast<std::uint8_t>(amt >> 8); continue; }
    if (op < kNumGens) {
      out.has |= std::uint64_t(1) << op;
      out.v[op] = static_cast<std::int16_t>(amt);
    } else if (out.n_high < 4) {
      out.high[out.n_high++] = static_cast<std::uint16_t>(op);
    }
  }
}

void Parser::note_ignored(std::vector<Aggregate>& agg, std::map<std::string, std::size_t>& idx,
                          const std::string& item, const std::string& message) {
  auto it = idx.find(item);
  if (it == idx.end()) {
    idx[item] = agg.size();
    agg.push_back({item, message, 1});
  } else {
    ++agg[it->second].zones;
  }
}

// The dB offset of the engine's pan law (see the header).
double timecents_to_s(int tc) {
  if (tc <= -12000) return 0.0;
  return std::min(100.0, std::exp2(double(tc) / 1200.0));
}

bool Parser::map_pair(const GenSet& pz, const GenSet* pg, const GenSet& iz, const GenSet* ig, Sf2Preset& out,
                      std::vector<int>& local, PresetAgg& agg) {
  std::vector<Aggregate>& gen_agg = agg.gen;
  std::vector<Aggregate>& mod_agg = agg.mod;
  std::map<std::string, std::size_t>& gen_idx = agg.gi;
  std::map<std::string, std::size_t>& mod_idx = agg.mi;
  // Ranges: the zone's own, else the global zone's, else the full range; both levels intersect.
  auto range = [](const GenSet& z, const GenSet* g, bool key, int& lo, int& hi) {
    const bool zh = key ? z.has_kr : z.has_vr;
    const GenSet* s = zh ? &z : (g && (key ? g->has_kr : g->has_vr) ? g : nullptr);
    if (!s) { lo = 0; hi = 127; return; }
    lo = key ? s->kr_lo : s->vr_lo;
    hi = key ? s->kr_hi : s->vr_hi;
    if (hi > 127) hi = 127;
  };
  int pkl, pkh, ikl, ikh, pvl, pvh, ivl, ivh;
  range(pz, pg, true, pkl, pkh);
  range(iz, ig, true, ikl, ikh);
  range(pz, pg, false, pvl, pvh);
  range(iz, ig, false, ivl, ivh);
  const int kl = std::max(pkl, ikl), kh = std::min(pkh, ikh), vl = std::max(pvl, ivl), vh = std::min(pvh, ivh);
  if (kl > kh || vl > vh) return true;  // never sounds: dropped silently

  // Effective generator values.
  auto iv = [&](unsigned g) -> int {
    if (iz.given(g)) return iz.v[g];
    if (ig && ig->given(g)) return ig->v[g];
    return kGens[g].def;
  };
  auto pv_given = [&](unsigned g, int& v) -> bool {
    if (pz.given(g)) { v = pz.v[g]; return true; }
    if (pg && pg->given(g)) { v = pg->v[g]; return true; }
    return false;
  };
  auto pv = [&](unsigned g) -> int {
    int v = 0;
    return pv_given(g, v) ? v : 0;
  };
  auto eff = [&](unsigned g) -> int {  // additive generator: instrument value + preset value, clamped
    const long s = long(iv(g)) + long(inst_only(g) ? 0 : pv(g));
    return static_cast<int>(std::clamp<long>(s, kGens[g].lo, kGens[g].hi));
  };

  // The sample.
  const std::uint32_t sid = iz.term;
  const ShdrRec& sh = shdr_[sid];
  if (sample_state_[sid] == 0) {
    sample_state_[sid] = 1;
    if (sh.type & 0x8000u) {
      sample_state_[sid] = 2;
      unsupported_.push_back({Sf2IssueKind::kUnsupported,
                              "sample " + std::to_string(sid) + " '" + sh.name + "' is a ROM sample (sampleType 0x" +
                                  [&] { char b[8]; std::snprintf(b, sizeof b, "%04X", sh.type); return std::string(b); }() + ")",
                              "sample"});
    } else {
      const unsigned t = sh.type;
      if (t != 1 && t != 2 && t != 4 && t != 8)
        return fail("sample " + std::to_string(sid) + " '" + sh.name + "': invalid sampleType " + std::to_string(t));
      if (sh.start > sh.end)
        return fail("sample " + std::to_string(sid) + " '" + sh.name + "': start " + std::to_string(sh.start) + " is after end " + std::to_string(sh.end));
      if (sh.end > smpl_frames_)
        return fail("sample " + std::to_string(sid) + " '" + sh.name + "': end " + std::to_string(sh.end) + " is beyond the sample data (" + std::to_string(smpl_frames_) + " points)");
      if (t != 1 && sh.link >= shdr_.size() - 1)
        return fail("sample " + std::to_string(sid) + " '" + sh.name + "': sampleLink " + std::to_string(sh.link) + " is out of range");
      if (sh.start == sh.end) return fail("sample " + std::to_string(sid) + " '" + sh.name + "' is empty");
      if (sh.rate != 44100 && sh.rate != 48000 && sh.rate != 88200 && sh.rate != 96000) {
        sample_state_[sid] = 2;
        unsupported_.push_back({Sf2IssueKind::kUnsupported,
                                "sample " + std::to_string(sid) + " '" + sh.name + "' has sample rate " + std::to_string(sh.rate) +
                                    " Hz (the importer never resamples; need 44.1/48/88.2/96 kHz)",
                                "sample"});
      }
    }
  }
  if (sample_state_[sid] == 2) return true;  // reported; the file will be skipped as unsupported

  const std::uint64_t frames = sh.end - sh.start;
  Region r;
  r.key_lo = static_cast<std::uint8_t>(kl);
  r.key_hi = static_cast<std::uint8_t>(kh);
  r.vel_lo = static_cast<std::uint8_t>(vl);
  r.vel_hi = static_cast<std::uint8_t>(vh);
  const int root = iv(kOverridingRootKey);
  r.key_center = (root >= 0 && root <= 127) ? static_cast<std::uint8_t>(root)
                                            : (sh.pitch <= 127 ? sh.pitch : std::uint8_t(60));
  r.transpose = eff(kCoarseTune);
  r.tune_cents = static_cast<float>(eff(kFineTune) + int(sh.correction));
  // Level: the engine's pan law compensation, the attenuation, and the specification's gain reduction of
  // half the resonance peak (section 8.1.3 #9) that applies to the whole signal whatever the cutoff.
  const int atten = eff(kInitialAttenuation);
  const int q_cb = eff(kInitialFilterQ);
  r.volume_db = std::max(-144.0f, kSf2PanLawDb - static_cast<float>(atten) / 10.0f - static_cast<float>(q_cb) / 20.0f);
  const int pan = static_cast<int>(std::clamp<long>(long(iv(kPan)) + long(pv(kPan)), -500, 500));
  r.pan = static_cast<float>(pan) / 5.0f;
  // The two halves of a stereo pair carry hard pans (-500 / +500) in their zones. A left or right sample whose
  // zone does not pan at all is played hard panned anyway: old fonts omit the generator, hardware forced it.
  if (pan == 0 && sh.type == 4) r.pan = -100.0f;
  if (pan == 0 && sh.type == 2) r.pan = 100.0f;

  // Offsets and loops.
  auto drop = [&](const std::string& why) {  // a zone the model cannot hold: dropped with a warning, the file goes on
    const std::string item = "zone of sample '" + sh.name + "': " + why;
    note_ignored(agg.zone, agg.zi, item, item + ": the zone is dropped");
    return true;
  };
  const std::int64_t off = std::int64_t(iv(kStartAddrsOffset)) + 32768 * std::int64_t(iv(kStartAddrsCoarse));
  if (off < 0 || off >= std::int64_t(frames))
    return drop("start offset " + std::to_string(off) + " is outside the sample (" + std::to_string(frames) + " frames)");
  r.offset = static_cast<std::uint64_t>(off);
  const unsigned modes = static_cast<unsigned>(iv(kSampleModes)) & 3u;
  if (modes == 1 || modes == 3) {
    const std::int64_t ls = std::int64_t(sh.startloop) - sh.start + iv(kStartloopAddrsOffset) + 32768 * std::int64_t(iv(kStartloopCoarse));
    const std::int64_t le = std::int64_t(sh.endloop) - sh.start + iv(kEndloopAddrsOffset) + 32768 * std::int64_t(iv(kEndloopCoarse));
    if (ls < 0 || le > std::int64_t(frames) || ls >= le)
      return drop("loop points " + std::to_string(ls) + ".." + std::to_string(le) + " are outside the sample (" +
                  std::to_string(frames) + " frames) or empty");
    r.loop_mode = modes == 1 ? LoopMode::kLoopContinuous : LoopMode::kLoopSustain;
    r.has_loop_points = true;
    r.loop_start = static_cast<std::uint64_t>(ls);
    r.loop_end = static_cast<std::uint64_t>(le - 1);
  }

  // Volume envelope.
  AmpEnvelope& a = r.ampeg;
  a.delay = static_cast<float>(timecents_to_s(eff(kDelayVolEnv)));
  a.attack = static_cast<float>(timecents_to_s(eff(kAttackVolEnv)));
  a.hold = static_cast<float>(timecents_to_s(eff(kHoldVolEnv)));
  a.decay = static_cast<float>(std::min(100.0, timecents_to_s(eff(kDecayVolEnv)) * kSf2NineTau));
  a.release = static_cast<float>(std::min(100.0, timecents_to_s(eff(kReleaseVolEnv)) * kSf2NineTau));
  const int sus = eff(kSustainVolEnv);
  a.sustain = sus >= 1000 ? 0.0f : static_cast<float>(100.0 * std::pow(10.0, -double(sus) / 200.0));
  a.start = 0;

  // Filter: initialFilterFc at its maximum (13500 cents) means no filter.
  const int fc = eff(kInitialFilterFc);
  if (fc < 13500) {
    r.filter.type = FilterType::kLpf2p;
    r.filter.cutoff_hz = static_cast<float>(8.175798915643707 * std::exp2(double(fc) / 1200.0));
    // initialFilterQ is the height of the peak over a filter that is not resonant (Butterworth, Q = 0.707): the
    // filter's Q in dB is q/10 - 3.01. The model's Q starts at 1 (0 dB), so a smaller peak is clamped to it.
    r.filter.resonance_db = std::clamp(static_cast<float>(q_cb) / 10.0f - 3.01f, 0.0f, 40.0f);
  }

  if (validate_region(r, frames) != Error::kNone) return drop("the values do not fit the instrument model");

  // The sample's slot in the preset.
  if (local[sid] < 0) {
    local[sid] = static_cast<int>(out.samples.size());
    Sf2SampleRef ref;
    ref.shdr = sid;
    ref.name = sh.name.empty() ? "sample " + std::to_string(sid) : sh.name;
    ref.format.channels = 1;
    ref.format.bits_per_sample = has24_ ? 24 : 16;
    ref.format.is_float = false;
    ref.format.sample_rate = sh.rate;
    ref.first_frame = sh.start;
    ref.frames = frames;
    out.samples.push_back(std::move(ref));
  }
  r.sample = static_cast<std::uint32_t>(local[sid]);
  out.regions.push_back(r);
  ++res_.stats.zones;

  // ---- ignored features, once per region-producing zone pair
  if (r.filter.type != FilterType::kNone)
    note_ignored(mod_agg, mod_idx, "default modulator velocity -> initialFilterFc (-2400 cents)",
                 "the default modulator velocity -> initialFilterFc is dropped: the cutoff is fixed, so this filtered preset "
                 "sounds brighter than SF2 players render it at velocity below 127");
  const char* const scope_pre = " (preset level)";
  for (unsigned g = 0; g < kNumGens; ++g) {
    if (g == kInstrument || g == kSampleID || g == kKeyRange || g == kVelRange) continue;
    // Preset-level generators that only exist at instrument level.
    int pval = 0;
    if (inst_only(g) && pv_given(g, pval) && pval != kGens[g].def) {
      note_ignored(gen_agg, gen_idx, std::string(kGens[g].name) + scope_pre,
                   std::string(kGens[g].name) + " is an instrument-only generator and is ignored at preset level (value " + std::to_string(pval) + ")");
    }
    if (mapped(g)) continue;
    const char* why = ignore_reason(g);
    if (why) {
      const int e = inst_only(g) ? iv(g) : eff(g);
      if (e != kGens[g].def)
        note_ignored(gen_agg, gen_idx, kGens[g].name, std::string(kGens[g].name) + " = " + std::to_string(e) + " is ignored: " + why);
    } else if (g < kNumGens && !inst_only(g) && (g == 14 || g == 18 || g == 19 || g == 20 || g == 42 || g == 49 || g == 55 || g == 59 || g == 60)) {
      const int e = iv(g);
      if (e != 0 || pv(g) != 0)
        note_ignored(gen_agg, gen_idx, kGens[g].name, std::string("reserved generator ") + kGens[g].name + " (" + std::to_string(g) + ") is ignored");
    }
  }
  for (const GenSet* z : {&pz, &iz})
    for (unsigned k = 0; k < z->n_high; ++k)
      note_ignored(gen_agg, gen_idx, "generator #" + std::to_string(z->high[k]),
                   "unknown generator #" + std::to_string(z->high[k]) + " is ignored");
  for (const GenSet* z : {&pz, &iz}) {
    const bool inst = z == &iz;
    for (unsigned g : {unsigned(kInstrument), unsigned(kSampleID)})
      if ((z->stray >> g) & 1)
        note_ignored(gen_agg, gen_idx, std::string(kGens[g].name) + (inst ? " (instrument level)" : scope_pre),
                     std::string(kGens[g].name) + " is not valid at this level and is ignored");
  }

  // Modulators of both levels: the zone's own entries override a global entry of the same identity.
  std::vector<ModRec> mods;
  auto collect = [&](const std::vector<ModRec>& table, const GenSet& z, const GenSet* g) {
    const std::size_t start = mods.size();
    for (std::size_t i = z.mod_begin; i < z.mod_end; ++i) mods.push_back(table[i]);
    if (g)
      for (std::size_t i = g->mod_begin; i < g->mod_end; ++i) {
        bool overridden = false;
        for (std::size_t k = start; k < mods.size(); ++k) overridden = overridden || same_identity(mods[k], table[i]);
        if (!overridden) mods.push_back(table[i]);
      }
  };
  collect(pmod_, pz, pg);
  collect(imod_, iz, ig);
  for (const ModRec& m : mods) {
    if (is_default_mod(m)) continue;
    char buf[160];
    std::string dest;
    if (m.dest & 0x8000u) dest = "link " + std::to_string(m.dest & 0x7fffu);
    else if (m.dest < kNumGens) dest = kGens[m.dest].name;
    else dest = "#" + std::to_string(m.dest);
    std::snprintf(buf, sizeof buf, "modulator src=0x%04X dest=%s amount=%d amtsrc=0x%04X trans=%u", m.src, dest.c_str(),
                  int(m.amount), m.amt_src, unsigned(m.trans));
    note_ignored(mod_agg, mod_idx, buf, std::string(buf) + " is ignored: custom modulators are not in the instrument model");
  }
  return true;
}

bool Parser::build_presets() {
  const std::size_t n_presets = phdr_.size() - 1, n_inst = inst_.size() - 1;
  res_.stats.instruments = n_inst;
  res_.stats.shdr_samples = shdr_.size() - 1;

  // One pass over the instruments: every zone with a sampleID is scanned once and cached (range checked), which
  // also gives the zone counts for the combination bound.
  inst_zones_.assign(n_inst, 0);
  icache_.assign(n_inst, InstCache{});
  std::uint64_t cached = 0;
  GenSet scratch;
  for (std::size_t i = 0; i < n_inst; ++i) {
    InstCache& c = icache_[i];
    for (std::size_t b = inst_[i].bag; b < inst_[i + 1].bag; ++b) {
      fill_gens(igen_, ibag_, b, true, scratch);
      if (!scratch.has_term) {
        if (b == inst_[i].bag) { c.global = scratch; c.has_global = true; }
        continue;
      }
      if (scratch.term >= shdr_.size() - 1)
        return fail("instrument " + std::to_string(i) + " '" + inst_[i].name + "': sampleID " + std::to_string(scratch.term) +
                    " is out of range (" + std::to_string(shdr_.size() - 1) + " samples)");
      if (++cached > kMaxInstrumentZones)
        return fail("more than " + std::to_string(kMaxInstrumentZones) + " instrument zones with a sample in the file");
      c.zones.push_back(scratch);
      ++inst_zones_[i];
    }
    if (gen_scans_ > kMaxGeneratorScans) return fail("too many generator records");
  }

  std::uint64_t combos = 0;
  std::map<std::pair<unsigned, unsigned>, std::size_t> seen;
  std::vector<int> local(shdr_.size() - 1, -1);
  for (std::size_t p = 0; p < n_presets; ++p) {
    Sf2Preset pre;
    pre.bank = phdr_[p].bank;
    pre.program = phdr_[p].preset;
    pre.key = std::to_string(pre.bank) + ":" + std::to_string(pre.program);
    pre.name = phdr_[p].name.empty() ? pre.key : phdr_[p].name;
    if (!seen.emplace(std::make_pair(unsigned(pre.bank), unsigned(pre.program)), p).second)
      return fail("preset " + std::to_string(p) + " '" + phdr_[p].name + "': bank:preset " + pre.key + " appears more than once");

    // Zones of the preset.
    std::vector<std::size_t> zones;  // bags with an instrument
    GenSet pg_store;
    bool has_global = false;
    for (std::size_t b = phdr_[p].bag; b < phdr_[p + 1].bag; ++b) {
      fill_gens(pgen_, pbag_, b, false, scratch);
      if (scratch.has_term) {
        if (scratch.term >= n_inst)
          return fail("preset " + std::to_string(p) + " '" + phdr_[p].name + "' (" + pre.key + "): instrument " +
                      std::to_string(scratch.term) + " is out of range (" + std::to_string(n_inst) + " instruments)");
        zones.push_back(b);
        combos += inst_zones_[scratch.term];
        if (combos > kMaxZoneCombos)
          return fail("more than " + std::to_string(kMaxZoneCombos) + " preset-zone x instrument-zone combinations (preset " + pre.key + ")");
      } else if (b == phdr_[p].bag) {
        pg_store = scratch;
        has_global = true;
      }
    }
    const GenSet* pg = has_global ? &pg_store : nullptr;

    PresetAgg agg;
    GenSet pz;
    for (std::size_t b : zones) {
      fill_gens(pgen_, pbag_, b, false, pz);
      const InstCache& ic = icache_[pz.term];
      for (const GenSet& iz : ic.zones) {
        if (!map_pair(pz, pg, iz, ic.has_global ? &ic.global : nullptr, pre, local, agg)) return false;
        if (res_.stats.zones > kMaxRegions)
          return fail("more than " + std::to_string(kMaxRegions) + " regions in the file");
      }
      if (gen_scans_ > kMaxGeneratorScans) return fail("too many generator records");
    }
    for (const auto& s : pre.samples) local[s.shdr] = -1;

    auto emit = [&](const std::vector<Aggregate>& v, const char* code, std::size_t& counter) {
      for (const Aggregate& a : v) {
        Sf2Warning w;
        w.code = code;
        w.preset = pre.key;
        w.item = a.item;
        w.message = a.message + " (in " + std::to_string(a.zones) + " zone" + (a.zones == 1 ? "" : "s") + ")";
        res_.warnings.push_back(std::move(w));
        ++counter;
      }
    };
    emit(agg.gen, "ignored_sf2_feature", res_.stats.ignored_generators);
    emit(agg.mod, "ignored_sf2_feature", res_.stats.ignored_modulators);
    emit(agg.zone, "ignored_zone", res_.stats.ignored_zones);
    if (pre.regions.empty()) {  // an empty instrument could only fail at engine load: not published
      Sf2Warning w;
      w.code = "ignored_empty_preset";
      w.preset = pre.key;
      w.item = "preset '" + pre.name + "'";
      w.message = "preset " + pre.key + " has no region that can sound and is not imported";
      res_.warnings.push_back(std::move(w));
      ++res_.stats.ignored_empty_presets;
      continue;
    }
    res_.presets.push_back(std::move(pre));
  }
  res_.stats.presets = res_.presets.size();
  res_.stats.generator_scans = gen_scans_;
  return true;
}

Sf2ParseResult Parser::run() {
  auto finish = [&]() -> Sf2ParseResult {
    if (damaged_) {
      res_.issues.clear();
      res_.issues.push_back({Sf2IssueKind::kDamaged, damaged_msg_, {}});
    } else if (!unsupported_.empty()) {
      res_.issues = unsupported_;
    }
    if (!res_.issues.empty()) {
      res_.presets.clear();
      res_.warnings.clear();
      res_.samples.reset();
      res_.stats = {};
    }
    return std::move(res_);
  };
  if (!parse_riff() || !load_tables() || !check_structure() || !check_samples()) return finish();
  res_.stats.bits24 = has24_;
  if (!build_presets()) return finish();
  res_.samples = std::make_shared<SamplesImpl>(src_, smpl_.off, smpl_frames_, sm24_.off, has24_);
  return finish();
}

}  // namespace

bool Sf2ParseResult::damaged() const {
  for (const auto& i : issues)
    if (i.kind == Sf2IssueKind::kDamaged) return true;
  return false;
}

Sf2ParseResult parse_sf2(std::shared_ptr<const ByteSource> src) {
  if (!src) {
    Sf2ParseResult r;
    r.issues.push_back({Sf2IssueKind::kDamaged, "no byte source", {}});
    return r;
  }
  Parser p(std::move(src));
  return p.run();
}

Sf2ParseResult parse_sf2_memory(std::span<const std::uint8_t> bytes) {
  return parse_sf2(std::make_shared<MemSource>(bytes));
}

Sf2ParseResult parse_sf2_file(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  auto bad = [&](const std::string& why) {
    Sf2ParseResult r;
    r.issues.push_back({Sf2IssueKind::kDamaged, why, {}});
    return r;
  };
  if (fd < 0) return bad(path + ": cannot open: " + std::strerror(errno));
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return bad(path + ": not a regular file");
  }
  return parse_sf2(std::make_shared<FileSrc>(fd, static_cast<std::uint64_t>(st.st_size)));
}

const char* sf2_generator_name(unsigned oper) { return oper < kNumGens ? kGens[oper].name : nullptr; }

}  // namespace velora::format
