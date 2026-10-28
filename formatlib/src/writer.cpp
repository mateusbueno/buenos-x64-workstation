#include <unistd.h>

#include <cstdio>

#include "codec.hpp"
#include "layout.hpp"

namespace velora::format {
using namespace layout;

namespace {
void put_str(std::vector<std::uint8_t>& v, const std::string& s) {
  put_u32(v, static_cast<std::uint32_t>(s.size()));
  v.insert(v.end(), s.begin(), s.end());
}
void put_f32(std::vector<std::uint8_t>& v, float f) { put_u32(v, static_cast<std::uint32_t>(float_to_bits(f))); }

void put_region(std::vector<std::uint8_t>& v, const Region& r) {
  put_u32(v, r.sample);
  put_u8(v, r.key_lo); put_u8(v, r.key_hi); put_u8(v, r.key_center);
  put_u8(v, r.vel_lo); put_u8(v, r.vel_hi);
  put_u8(v, static_cast<std::uint8_t>(r.loop_mode));
  put_u8(v, r.trigger == TriggerMode::kRelease ? 1 : 0);  // first/legato/release_key: carried by the format 2.2 table
  put_u8(v, r.has_loop_points ? 1 : 0);
  put_u32(v, static_cast<std::uint32_t>(r.transpose));
  put_f32(v, r.tune_cents); put_f32(v, r.volume_db); put_f32(v, r.pan);
  put_u64(v, r.offset);
  put_u32(v, r.seq_length); put_u32(v, r.seq_position);
  put_f32(v, r.lorand); put_f32(v, r.hirand);
  put_u64(v, r.loop_start); put_u64(v, r.loop_end);
  put_f32(v, r.ampeg.delay); put_f32(v, r.ampeg.start); put_f32(v, r.ampeg.attack);
  put_f32(v, r.ampeg.hold); put_f32(v, r.ampeg.decay); put_f32(v, r.ampeg.sustain);
  put_f32(v, r.ampeg.release);
}

void put_ext(std::vector<std::uint8_t>& v, std::uint32_t index, const Region& r) {
  const Keyswitch& k = r.sw;
  const std::int16_t notes[7] = {k.lokey, k.hikey, k.last, k.down, k.up, k.previous, k.default_key};
  std::uint8_t flags = 0;
  for (int i = 0; i < 7; ++i)
    if (notes[i] >= 0) flags = static_cast<std::uint8_t>(flags | (1u << i));
  put_u32(v, index);
  put_u8(v, flags);
  for (int i = 0; i < 7; ++i) put_u8(v, notes[i] >= 0 ? static_cast<std::uint8_t>(notes[i]) : 0);
  put_u8(v, r.xf.in_count); put_u8(v, r.xf.out_count);
  put_u8(v, static_cast<std::uint8_t>(r.xf.curve));
  put_u8(v, static_cast<std::uint8_t>(r.filter.type));
  for (const CcRange* set : {r.xf.in, r.xf.out})
    for (unsigned i = 0; i < kMaxCcCrossfades; ++i) {
      put_u8(v, set[i].cc); put_u8(v, set[i].lo); put_u8(v, set[i].hi); put_u8(v, 0);
    }
  put_f32(v, r.filter.cutoff_hz);
  put_f32(v, r.filter.resonance_db);
}

void put_trig(std::vector<std::uint8_t>& v, std::uint32_t index, const Region& r) {
  put_u32(v, index);
  put_u8(v, static_cast<std::uint8_t>(r.trigger));
  put_u8(v, r.has_end ? 1 : 0);
  put_u8(v, 0); put_u8(v, 0);
  put_u64(v, r.end);
}

void put_amp(std::vector<std::uint8_t>& v, std::uint32_t index, const Region& r) {
  const AmpMap& a = r.amp;
  put_u32(v, index);
  put_f32(v, a.veltrack); put_f32(v, a.amplitude); put_f32(v, a.keytrack);
  put_u8(v, a.keycenter); put_u8(v, a.curve_count); put_u8(v, 0); put_u8(v, 0);
  put_f32(v, a.vel2attack); put_f32(v, a.vel2decay); put_f32(v, a.vel2sustain); put_f32(v, a.vel2release);
  for (unsigned i = 0; i < a.curve_count; ++i) {
    put_u8(v, a.curve[i].vel); put_u8(v, 0); put_u8(v, 0); put_u8(v, 0);
    put_f32(v, a.curve[i].gain);
  }
}

void put_cc(std::vector<std::uint8_t>& v, std::uint32_t index, const Region& r) {
  const CcMap& m = r.cc;
  put_u32(v, index);
  put_u8(v, m.gate_count); put_u8(v, m.mod_count); put_u8(v, 0); put_u8(v, 0);
  for (unsigned i = 0; i < m.gate_count; ++i) {
    put_u8(v, m.gates[i].cc); put_u8(v, 0); put_u8(v, 0); put_u8(v, 0);
    put_f32(v, m.gates[i].lo); put_f32(v, m.gates[i].hi);
  }
  for (unsigned i = 0; i < m.mod_count; ++i) {
    put_u8(v, static_cast<std::uint8_t>(m.mods[i].target)); put_u8(v, m.mods[i].cc); put_u8(v, m.mods[i].curve); put_u8(v, 0);
    put_f32(v, m.mods[i].depth);
  }
}

void put_vg(std::vector<std::uint8_t>& v, std::uint32_t index, const Region& r) {
  const VoiceGroup& g = r.vg;
  put_u32(v, index);
  put_u32(v, static_cast<std::uint32_t>(g.group));
  put_u32(v, static_cast<std::uint32_t>(g.off_by));
  put_u8(v, static_cast<std::uint8_t>((g.has_off_by ? 1u : 0u) | (g.note_selfmask ? 0u : 2u)));
  put_u8(v, static_cast<std::uint8_t>(g.off_mode));
  put_u8(v, 0); put_u8(v, 0);
  put_f32(v, g.off_time);
  for (std::uint16_t lim : {g.polyphony, g.group_polyphony, g.note_polyphony, std::uint16_t{0}}) {
    put_u8(v, static_cast<std::uint8_t>(lim & 0xFF)); put_u8(v, static_cast<std::uint8_t>(lim >> 8));
  }
}

void put_bt(std::vector<std::uint8_t>& v, std::uint32_t index, const Region& r) {
  const PitchBend& b = r.bend;
  const CcTriggers& t = r.trig;
  put_u32(v, index);
  put_u8(v, static_cast<std::uint8_t>((b.enabled ? 1u : 0u) | (t.no_note ? 2u : 0u)));
  put_u8(v, t.count);
  put_u8(v, static_cast<std::uint8_t>(b.smooth_ms & 0xFF)); put_u8(v, static_cast<std::uint8_t>(b.smooth_ms >> 8));
  put_f32(v, b.up);
  put_f32(v, b.down);
  put_f32(v, b.step);
  for (unsigned i = 0; i < t.count; ++i) {
    put_u8(v, t.t[i].cc); put_u8(v, 0); put_u8(v, 0); put_u8(v, 0);
    put_f32(v, t.t[i].lo);
    put_f32(v, t.t[i].hi);
  }
}

void put_tune(std::vector<std::uint8_t>& v, const FilterTune& t) {
  put_f32(v, t.keytrack); put_f32(v, t.veltrack);
  put_u8(v, t.keycenter); put_u8(v, t.cutoff_count); put_u8(v, t.res_count); put_u8(v, 0);
  for (unsigned i = 0; i < t.cutoff_count; ++i) { put_u8(v, t.cutoff[i].cc); put_u8(v, t.cutoff[i].curve); put_u8(v, 0); put_u8(v, 0); put_f32(v, t.cutoff[i].depth); }
  for (unsigned i = 0; i < t.res_count; ++i) { put_u8(v, t.res[i].cc); put_u8(v, t.res[i].curve); put_u8(v, 0); put_u8(v, 0); put_f32(v, t.res[i].depth); }
}

void put_fl(std::vector<std::uint8_t>& v, std::uint32_t index, const Region& r) {
  const FilterMap& m = r.fmap;
  const bool second = m.f2.type != FilterType::kNone;
  put_u32(v, index);
  put_u8(v, second ? 1 : 0); put_u8(v, 0); put_u8(v, 0); put_u8(v, 0);
  put_tune(v, m.f1);
  if (second) {
    put_u8(v, static_cast<std::uint8_t>(m.f2.type)); put_u8(v, 0); put_u8(v, 0); put_u8(v, 0);
    put_f32(v, m.f2.cutoff_hz); put_f32(v, m.f2.resonance_db);
    put_tune(v, m.f2.tune);
  }
}

void put_md(std::vector<std::uint8_t>& v, std::uint32_t index, const Region& r) {
  const ModMap& m = r.mod;
  put_u32(v, index);
  put_u8(v, static_cast<std::uint8_t>((m.amp.on ? 1u : 0u) | (m.pitch.on ? 2u : 0u) | (m.fil.on ? 4u : 0u) | (m.pitch_eg.on ? 8u : 0u) | (m.fil_eg.on ? 16u : 0u)));
  put_u8(v, m.eq_count); put_u8(v, 0); put_u8(v, 0);
  for (const LfoSpec* l : {&m.amp, &m.pitch, &m.fil}) {
    if (!l->on) continue;
    put_u8(v, static_cast<std::uint8_t>(l->wave)); put_u8(v, 0); put_u8(v, 0); put_u8(v, 0);
    put_f32(v, l->depth); put_f32(v, l->freq); put_f32(v, l->delay); put_f32(v, l->fade);
  }
  for (const EgSpec* e : {&m.pitch_eg, &m.fil_eg}) {
    if (!e->on) continue;
    for (float f : {e->delay, e->attack, e->hold, e->decay, e->release, e->start, e->sustain, e->depth, e->vel2depth}) put_f32(v, f);
  }
  for (unsigned b = 0; b < m.eq_count; ++b) {
    const EqBand& q = m.eq[b];
    put_u8(v, static_cast<std::uint8_t>(q.type)); put_u8(v, q.freq_count); put_u8(v, q.bw_count); put_u8(v, q.gain_count);
    for (float f : {q.freq, q.bw, q.gain, q.vel2freq, q.vel2gain}) put_f32(v, f);
    for (int w = 0; w < 3; ++w) {
      const FilterMod* e = w == 0 ? q.freq_mods : w == 1 ? q.bw_mods : q.gain_mods;
      const unsigned n = w == 0 ? q.freq_count : w == 1 ? q.bw_count : q.gain_count;
      for (unsigned i = 0; i < n; ++i) { put_u8(v, e[i].cc); put_u8(v, e[i].curve); put_u8(v, 0); put_u8(v, 0); put_f32(v, e[i].depth); }
    }
  }
}

void put_cc_setup(std::vector<std::uint8_t>& v, const InstrumentCcSetup& s) {
  put_u32(v, static_cast<std::uint32_t>(s.defaults.size()));
  for (const auto& d : s.defaults) {
    put_u8(v, d.cc); put_u8(v, 0); put_u8(v, 0); put_u8(v, 0);
    put_f32(v, d.value);
  }
  put_u32(v, static_cast<std::uint32_t>(s.curves.size()));
  for (const auto& c : s.curves) {
    put_u8(v, c.index); put_u8(v, 0);
    put_u8(v, static_cast<std::uint8_t>(c.points.size() & 0xFF)); put_u8(v, static_cast<std::uint8_t>(c.points.size() >> 8));
    for (const auto& p : c.points) {
      put_u8(v, p.x); put_u8(v, 0); put_u8(v, 0); put_u8(v, 0);
      put_f32(v, p.y);
    }
  }
}
}  // namespace

BundleWriter::BundleWriter(WriterOptions opt) : opt_(opt) {}

void BundleWriter::set_library_id(const Uuid& id) {
  library_id_ = id;
  has_library_id_ = true;
}

Error BundleWriter::set_source_id(std::string_view source_id) {
  if (source_id.empty()) return Error::kInvalidArgument;
  source_id_ = std::string(source_id);
  has_source_id_ = true;
  return Error::kNone;
}

Error BundleWriter::set_instrument_cc_setup(std::size_t instrument, const InstrumentCcSetup& setup) {
  if (instrument >= instruments_.size()) return Error::kInvalidArgument;
  if (validate_cc_setup(setup) != Error::kNone) return Error::kInvalidArgument;
  instruments_[instrument].cc_setup = setup;
  return Error::kNone;
}

Error BundleWriter::add_region(std::size_t instrument, const Region& region) {
  if (instrument >= instruments_.size()) return Error::kInvalidArgument;
  auto& ins = instruments_[instrument];
  if (region.sample >= ins.samples.size()) return Error::kInvalidArgument;
  if (ins.regions.size() >= (1u << 24)) return Error::kInvalidArgument;
  if (validate_region(region, ins.samples[region.sample].frames) != Error::kNone)
    return Error::kInvalidArgument;
  ins.regions.push_back(region);
  return Error::kNone;
}

Result<std::size_t> BundleWriter::add_instrument(std::string_view name,
                                                 std::string_view relative_path) {
  if (name.size() > kMaxStringLen) return Error::kInvalidArgument;
  auto key = normalize_instrument_key(relative_path);
  if (!key) return key.error();
  for (const auto& i : instruments_)
    if (i.key == key.value()) return Error::kDuplicateKey;
  InstrumentRec r;
  r.name = std::string(name);
  r.key = key.take();
  instruments_.push_back(std::move(r));
  return instruments_.size() - 1;
}

Error BundleWriter::discard_last_instrument() {
  if (instruments_.empty()) return Error::kInvalidArgument;
  const InstrumentRec& ins = instruments_.back();
  if (!ins.samples.empty()) {
    const std::uint64_t first = ins.samples.front().first_block;
    std::uint64_t count = 0;
    for (const SampleRec& s : ins.samples) count += s.block_count;
    if (first + count != blocks_.size()) return Error::kInvalidArgument;  // another instrument added blocks after this one
    const std::size_t data_before = first < blocks_.size() ? static_cast<std::size_t>(blocks_[first].data_offset) : block_data_.size();
    blocks_.resize(static_cast<std::size_t>(first));
    block_data_.resize(data_before);
  }
  instruments_.pop_back();
  return Error::kNone;
}

Error BundleWriter::add_sample(std::size_t instrument, const SampleInput& in) {
  if (instrument >= instruments_.size()) return Error::kInvalidArgument;
  if (in.name.size() > kMaxStringLen) return Error::kInvalidArgument;
  if (!is_supported(in.format)) return Error::kUnsupportedFormat;
  const std::size_t ch = in.format.channels;
  if (in.frame_count > (std::uint64_t(1) << 40)) return Error::kInvalidArgument;
  if (in.data.size() != in.frame_count * ch) return Error::kInvalidArgument;
  if (!in.format.is_float) {
    const std::int32_t lim = std::int32_t(1) << (in.format.bits_per_sample - 1);
    for (std::int32_t v : in.data)
      if (v < -lim || v >= lim) return Error::kInvalidArgument;
  }

  const std::size_t blocks_before = blocks_.size();
  const std::size_t data_before = block_data_.size();
  SampleRec rec;
  rec.name = in.name;
  rec.format = in.format;
  rec.frames = in.frame_count;
  rec.first_block = blocks_before;
  rec.block_count = block_count_for(in.frame_count);

  Sha256 pcm;
  pcm_hash_update(pcm, in.format, in.data.data(), in.data.size());
  rec.pcm_sha256 = pcm.finish();

  for (std::uint32_t b = 0; b < rec.block_count; ++b) {
    const std::uint64_t start = std::uint64_t(b) * kBlockFrames;
    const std::uint32_t frames =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(kBlockFrames, in.frame_count - start));
    const std::int32_t* src = in.data.data() + start * ch;
    BlockRec br;
    br.data_offset = block_data_.size();
    br.frames = frames;
    if (in.format.is_float) {
      for (std::size_t i = 0; i < std::size_t(frames) * ch; ++i)
        put_u32(block_data_, static_cast<std::uint32_t>(src[i]));
    } else {
      const Error e = flac_encode_block(in.format, src, frames, block_data_);
      if (e != Error::kNone) {
        blocks_.resize(blocks_before);
        block_data_.resize(data_before);
        return e;
      }
    }
    br.size = static_cast<std::uint32_t>(block_data_.size() - br.data_offset);
    br.sha256 = Sha256::hash({block_data_.data() + br.data_offset, br.size});
    blocks_.push_back(br);
  }
  instruments_[instrument].samples.push_back(std::move(rec));
  return Error::kNone;
}

Result<std::vector<std::uint8_t>> BundleWriter::finalize() const { return finalize_impl(nullptr); }

Result<std::vector<std::uint8_t>> BundleWriter::finalize_protected(const ProtectParams& p) const {
  if (!p.encrypt || !p.sign) return Error::kInvalidArgument;
  if (std::all_of(p.nonce.begin(), p.nonce.end(), [](std::uint8_t b) { return b == 0; })) return Error::kInvalidArgument;  // never zero
  return finalize_impl(&p);
}

Result<Nonce> BundleWriter::derive_nonce() const {
  auto plain = finalize();
  if (!plain) return plain.error();
  return nonce_from_plain_digest(Sha256::hash(plain.value()));
}

Nonce BundleWriter::nonce_from_plain_digest(const Digest256& content) {
  static constexpr char kLabel[] = "velora/protected-nonce/v1";
  Sha256 h;
  h.update(kLabel, sizeof kLabel);  // includes the terminating 0x00
  h.update(content.data(), 32);
  const Digest256 d = h.finish();
  Nonce n;
  std::copy(d.begin(), d.begin() + 8, n.begin());
  return n;
}

Result<std::vector<std::uint8_t>> BundleWriter::finalize_impl(const ProtectParams* prot) const {
  const bool protect = prot != nullptr;
  if (protect) {
    if (opt_.format_version != kFormatVersion) return Error::kInvalidArgument;  // no test hooks on the protected path
  } else if (opt_.format_version < kMinReadableVersion || opt_.format_version > kFormatVersion + 1) {
    return Error::kInvalidArgument;  // only the documented test hook range is producible
  }

  // Fingerprint of the source content (used for library_id unless vendor-assigned).
  if (opt_.format_version < 2)
    for (const auto& ins : instruments_)
      if (!ins.regions.empty()) return Error::kInvalidArgument;  // v1 has no region table
  // Minor 1 only for bundles that need the extension table (everything else stays byte-identical
  // to the Epic 1 output).
  bool needs_ext = false;
  for (const auto& ins : instruments_)
    for (const auto& r : ins.regions) needs_ext = needs_ext || has_extension(r);
  bool needs_trig = false;
  for (const auto& ins : instruments_)
    for (const auto& r : ins.regions) needs_trig = needs_trig || has_trigger_extension(r);
  bool needs_amp = false;
  for (const auto& ins : instruments_)
    for (const auto& r : ins.regions) needs_amp = needs_amp || has_amp_extension(r);
  bool needs_cc = false;
  for (const auto& ins : instruments_) {
    needs_cc = needs_cc || ins.cc_setup.any();
    for (const auto& r : ins.regions) needs_cc = needs_cc || has_cc_extension(r);
  }
  bool needs_vg = false;
  for (const auto& ins : instruments_)
    for (const auto& r : ins.regions) needs_vg = needs_vg || has_vg_extension(r);
  bool needs_bt = false;
  for (const auto& ins : instruments_)
    for (const auto& r : ins.regions) needs_bt = needs_bt || has_bend_trigger_extension(r);
  bool needs_fl = false;
  for (const auto& ins : instruments_)
    for (const auto& r : ins.regions) needs_fl = needs_fl || has_filter_extension(r);
  bool needs_md = false;
  for (const auto& ins : instruments_)
    for (const auto& r : ins.regions) needs_md = needs_md || has_mod_extension(r);
  if (needs_md && opt_.format_minor < 8) return Error::kInvalidArgument;
  if (needs_fl && opt_.format_minor < 7) return Error::kInvalidArgument;
  if (needs_bt && opt_.format_minor < 6) return Error::kInvalidArgument;
  if (needs_vg && opt_.format_minor < 5) return Error::kInvalidArgument;
  if (needs_cc && opt_.format_minor < 4) return Error::kInvalidArgument;
  if (needs_amp && opt_.format_minor < 3) return Error::kInvalidArgument;
  if (needs_trig && opt_.format_minor < 2) return Error::kInvalidArgument;
  if (needs_ext && opt_.format_minor < 1) return Error::kInvalidArgument;
  if (opt_.format_minor > kFormatMinor) return Error::kInvalidArgument;
  // a minor N bundle carries the tables of every lower minor too (possibly empty)
  const std::uint32_t minor = needs_md ? 8u : needs_fl ? 7u : needs_bt ? 6u : needs_vg ? 5u : needs_cc ? 4u : needs_amp ? 3u : needs_trig ? 2u : needs_ext ? 1u : 0u;
  const std::uint32_t version_word =
      (protect ? kProtectedFormatVersion : opt_.format_version) | (minor << kMinorShift);
  Uuid library_id = library_id_;
  if (!has_library_id_ && has_source_id_) {
    library_id = library_id_from_source(source_id_);
  } else if (!has_library_id_) {
    std::vector<std::uint8_t> fp;
    put_u32(fp, static_cast<std::uint32_t>(instruments_.size()));
    for (const auto& ins : instruments_) {
      put_str(fp, ins.key);
      put_u32(fp, static_cast<std::uint32_t>(ins.samples.size()));
      for (const auto& s : ins.samples) {
        put_u8(fp, s.format.channels);
        put_u8(fp, s.format.bits_per_sample);
        put_u8(fp, s.format.is_float ? 1 : 0);
        put_u32(fp, s.format.sample_rate);
        put_u64(fp, s.frames);
        fp.insert(fp.end(), s.pcm_sha256.begin(), s.pcm_sha256.end());
      }
    }
    const Digest256 d = Sha256::hash(fp);
    library_id = library_id_from_fingerprint(d);
  }

  // Flatten blocks in instrument-major sample order.
  std::vector<BlockRec> ordered;
  std::vector<std::uint8_t> data;
  ordered.reserve(blocks_.size());
  data.reserve(block_data_.size());
  std::vector<std::uint64_t> first_block;  // per flat sample
  for (const auto& ins : instruments_)
    for (const auto& s : ins.samples) {
      first_block.push_back(ordered.size());
      for (std::uint32_t b = 0; b < s.block_count; ++b) {
        BlockRec br = blocks_[s.first_block + b];
        const std::uint64_t off = data.size();
        data.insert(data.end(), block_data_.begin() + br.data_offset,
                    block_data_.begin() + br.data_offset + br.size);
        br.data_offset = off;
        ordered.push_back(br);
      }
    }

  Digest256 list_root{};
  if (protect) {
    if (ordered.size() >= kMaxProtectedBlocks) return Error::kInvalidArgument;
    std::vector<Digest256> hashes;
    hashes.reserve(ordered.size());
    for (std::size_t b = 0; b < ordered.size(); ++b) {
      BlockRec& br = ordered[b];
      if (br.size > kMaxProtectedBlockBytes) return Error::kInvalidArgument;
      const Error e = prot->encrypt(static_cast<std::uint32_t>(b), {data.data() + br.data_offset, br.size});
      if (e != Error::kNone) return e;
      br.sha256 = Sha256::hash({data.data() + br.data_offset, br.size});  // index hash is over the ciphertext
      hashes.push_back(br.sha256);
    }
    list_root = hash_list_root(hashes);
  }

  std::vector<std::uint8_t> manifest;
  put_u32(manifest, version_word);
  manifest.insert(manifest.end(), library_id.begin(), library_id.end());
  std::uint32_t total_samples = 0;
  for (const auto& ins : instruments_) total_samples += static_cast<std::uint32_t>(ins.samples.size());
  put_u32(manifest, static_cast<std::uint32_t>(instruments_.size()));
  put_u32(manifest, total_samples);
  put_u64(manifest, ordered.size());
  const std::size_t index_hash_pos = manifest.size();
  manifest.insert(manifest.end(), 32, 0);  // patched below
  if (protect) {
    put_u8(manifest, kCipherAes256Ctr);
    put_u8(manifest, kHashListMerkleSha256);
    put_u8(manifest, 0);
    put_u8(manifest, 0);
    manifest.insert(manifest.end(), prot->nonce.begin(), prot->nonce.end());
    manifest.insert(manifest.end(), prot->key_check.begin(), prot->key_check.end());
    manifest.insert(manifest.end(), list_root.begin(), list_root.end());
  }
  std::size_t flat = 0;
  for (const auto& ins : instruments_) {
    put_str(manifest, ins.key);
    put_str(manifest, ins.name);
    put_u32(manifest, static_cast<std::uint32_t>(ins.samples.size()));
    for (const auto& s : ins.samples) {
      put_str(manifest, s.name);
      put_u8(manifest, s.format.channels);
      put_u8(manifest, s.format.bits_per_sample);
      put_u8(manifest, s.format.is_float ? 1 : 0);
      put_u8(manifest, 0);
      put_u32(manifest, s.format.sample_rate);
      put_u64(manifest, s.frames);
      put_u32(manifest, kBlockFrames);
      put_u32(manifest, s.block_count);
      put_u64(manifest, first_block[flat++]);
      manifest.insert(manifest.end(), s.pcm_sha256.begin(), s.pcm_sha256.end());
    }
    if (protect || opt_.format_version >= 2) {
      put_u32(manifest, static_cast<std::uint32_t>(ins.regions.size()));
      for (const auto& r : ins.regions) put_region(manifest, r);
      if (minor >= 1) {
        std::uint32_t n_ext = 0;
        for (const auto& r : ins.regions) n_ext += has_extension(r) ? 1u : 0u;
        put_u32(manifest, n_ext);
        for (std::size_t k = 0; k < ins.regions.size(); ++k)
          if (has_extension(ins.regions[k])) put_ext(manifest, static_cast<std::uint32_t>(k), ins.regions[k]);
      }
      if (minor >= 2) {
        std::uint32_t n_trig = 0;
        for (const auto& r : ins.regions) n_trig += has_trigger_extension(r) ? 1u : 0u;
        put_u32(manifest, n_trig);
        for (std::size_t k = 0; k < ins.regions.size(); ++k)
          if (has_trigger_extension(ins.regions[k])) put_trig(manifest, static_cast<std::uint32_t>(k), ins.regions[k]);
      }
      if (minor >= 3) {
        std::uint32_t n_amp = 0;
        for (const auto& r : ins.regions) n_amp += has_amp_extension(r) ? 1u : 0u;
        put_u32(manifest, n_amp);
        for (std::size_t k = 0; k < ins.regions.size(); ++k)
          if (has_amp_extension(ins.regions[k])) put_amp(manifest, static_cast<std::uint32_t>(k), ins.regions[k]);
      }
      if (minor >= 4) {
        std::uint32_t n_cc = 0;
        for (const auto& r : ins.regions) n_cc += has_cc_extension(r) ? 1u : 0u;
        put_u32(manifest, n_cc);
        for (std::size_t k = 0; k < ins.regions.size(); ++k)
          if (has_cc_extension(ins.regions[k])) put_cc(manifest, static_cast<std::uint32_t>(k), ins.regions[k]);
        put_cc_setup(manifest, ins.cc_setup);
      }
      if (minor >= 5) {
        std::uint32_t n_vg = 0;
        for (const auto& r : ins.regions) n_vg += has_vg_extension(r) ? 1u : 0u;
        put_u32(manifest, n_vg);
        for (std::size_t k = 0; k < ins.regions.size(); ++k)
          if (has_vg_extension(ins.regions[k])) put_vg(manifest, static_cast<std::uint32_t>(k), ins.regions[k]);
      }
      if (minor >= 6) {
        std::uint32_t n_bt = 0;
        for (const auto& r : ins.regions) n_bt += has_bend_trigger_extension(r) ? 1u : 0u;
        put_u32(manifest, n_bt);
        for (std::size_t k = 0; k < ins.regions.size(); ++k)
          if (has_bend_trigger_extension(ins.regions[k])) put_bt(manifest, static_cast<std::uint32_t>(k), ins.regions[k]);
      }
      if (minor >= 7) {
        std::uint32_t n_fl = 0;
        for (const auto& r : ins.regions) n_fl += has_filter_extension(r) ? 1u : 0u;
        put_u32(manifest, n_fl);
        for (std::size_t k = 0; k < ins.regions.size(); ++k)
          if (has_filter_extension(ins.regions[k])) put_fl(manifest, static_cast<std::uint32_t>(k), ins.regions[k]);
      }
      if (minor >= 8) {
        std::uint32_t n_md = 0;
        for (const auto& r : ins.regions) n_md += has_mod_extension(r) ? 1u : 0u;
        put_u32(manifest, n_md);
        for (std::size_t k = 0; k < ins.regions.size(); ++k)
          if (has_mod_extension(ins.regions[k])) put_md(manifest, static_cast<std::uint32_t>(k), ins.regions[k]);
      }
    }
  }

  const std::uint64_t manifest_off = kHeaderSize;
  const std::uint64_t index_off = manifest_off + manifest.size();
  const std::uint64_t index_size = ordered.size() * kIndexEntrySize;
  const std::uint64_t data_off = index_off + index_size + (protect ? kSealSize : 0);
  std::vector<std::uint8_t> index;
  index.reserve(index_size);
  for (const auto& b : ordered) {
    put_u64(index, data_off + b.data_offset);
    put_u32(index, b.size);
    put_u32(index, b.frames);
    index.insert(index.end(), b.sha256.begin(), b.sha256.end());
  }
  const Digest256 index_hash = Sha256::hash(index);
  std::copy(index_hash.begin(), index_hash.end(), manifest.begin() + index_hash_pos);
  const Digest256 manifest_hash = Sha256::hash(manifest);

  std::vector<std::uint8_t> out;
  out.reserve(data_off + data.size());
  out.insert(out.end(), kMagic, kMagic + 4);
  put_u32(out, version_word);
  put_u64(out, manifest_off);
  put_u64(out, manifest.size());
  put_u64(out, index_off);
  put_u64(out, index_size);
  put_u64(out, data_off);
  put_u64(out, data.size());
  put_u64(out, data_off + data.size());
  out.insert(out.end(), manifest_hash.begin(), manifest_hash.end());
  out.insert(out.end(), index_hash.begin(), index_hash.end());
  out.insert(out.end(), manifest.begin(), manifest.end());
  out.insert(out.end(), index.begin(), index.end());
  if (protect) {
    ManifestStatement st;
    st.version_word = version_word;
    st.library_id = library_id;
    st.bundle_id = manifest_hash;
    st.index_sha256 = index_hash;
    st.hash_list_root = list_root;
    st.block_count = ordered.size();
    st.nonce = prot->nonce;
    st.signer_key_id = prot->signer_key_id;
    Signature sig{};
    if (const Error e = prot->sign(statement_bytes(st), sig); e != Error::kNone) return e;
    out.insert(out.end(), kSealMagic, kSealMagic + 4);
    put_u8(out, kSealAlgEd25519);
    out.insert(out.end(), 3, 0);
    out.insert(out.end(), prot->signer_key_id.begin(), prot->signer_key_id.end());
    out.insert(out.end(), sig.begin(), sig.end());
    out.insert(out.end(), kSealSize - 80, 0);
  }
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

Error BundleWriter::finalize_to_file(const std::string& path) const {
  auto bytes = finalize();
  if (!bytes) return bytes.error();
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return Error::kIo;
  bool ok = std::fwrite(bytes.value().data(), 1, bytes.value().size(), f) == bytes.value().size();
  ok = std::fflush(f) == 0 && ok;
  ok = ::fsync(::fileno(f)) == 0 && ok;
  ok = std::fclose(f) == 0 && ok;
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return Error::kIo;
  }
  return Error::kNone;
}

}  // namespace velora::format
