#include "velora/host/protected_head_provider.hpp"

#include <algorithm>
#include <cstring>

#include "velora/host/library_resolver.hpp"
#include "velora/host/secure_memory.hpp"

namespace velora::host {

namespace fmt = velora::format;
namespace ports = velora::ports;

namespace {

// The loader's view of a protected reader: decrypts and decodes on the loading thread.
class ProtectedSource final : public InstrumentSource {
 public:
  explicit ProtectedSource(const fmt::ProtectedBundleReader& r) : r_(r) {}
  std::span<const fmt::InstrumentInfo> instruments() const override { return r_.instruments(); }
  std::span<const fmt::SampleInfo> samples() const override { return r_.samples(); }
  fmt::Error read_frames(std::size_t sample, std::uint64_t first, std::uint64_t count, std::span<std::int32_t> out) const override {
    return r_.read_frames(sample, first, count, out);
  }
  bool protected_source() const override { return true; }

 private:
  const fmt::ProtectedBundleReader& r_;
};

constexpr std::uint64_t kMaxWrappedKeyFile = 4096;  // a wrapped_key_v1 blob is 105 bytes; anything much larger is not one

}  // namespace

bool is_integrity_error(fmt::Error e) {
  return e == fmt::Error::kBlockHashMismatch || e == fmt::Error::kCorruptBlock || e == fmt::Error::kPcmHashMismatch;
}

ProtectedHeadProvider::ProtectedHeadProvider(std::shared_ptr<FsPort> fs, ports::KeyProvider& keys, DamageRegistry* damage, SampleLoadOptions options)
    : RamHeadProvider(std::move(options)), fs_(std::move(fs)), keys_(keys), damage_(damage) {}

LoadStatus ProtectedHeadProvider::load(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out) {
  if (!resolved.ok() || !resolved.is_protected()) return RamHeadProvider::load(resolved, out);
  if (load_options().head.enabled) return load_options().stream.enabled ? LoadStatus::kStreamProtectedUnsupported : LoadStatus::kHeadProtectedUnsupported;  // Stories 1.16 / 1.17: heads and streaming are for unprotected bundles only (deferred)
  return load_protected(resolved, out, nullptr);
}

LoadStatus ProtectedHeadProvider::load_controlled(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out, const LoadHooks& hooks) {
  if (!resolved.ok() || !resolved.is_protected()) return RamHeadProvider::load_controlled(resolved, out, hooks);
  if (load_options().head.enabled) return load_options().stream.enabled ? LoadStatus::kStreamProtectedUnsupported : LoadStatus::kHeadProtectedUnsupported;
  return load_protected(resolved, out, &hooks);
}

LoadStatus ProtectedHeadProvider::load_protected(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out, const LoadHooks* hooks) {
  last_key_status_ = ports::KeyStatus::kOk;
  // 1. The wrapped key delivered with the library (a public-key-wrapped blob: it is not secret, only the device that owns the matching private key can open it).
  std::vector<std::uint8_t> blob;
  {
    int fd = -1;
    std::uint64_t size = 0;
    if (fs_->open_read(resolved.bundle_dir + "/" + kWrappedKeyFileName, fd, size) != FsStatus::kOk) {
      last_key_status_ = ports::KeyStatus::kUnavailable;
      return LoadStatus::kKeyUnavailable;
    }
    const bool good = size != 0 && size <= kMaxWrappedKeyFile;
    if (good) {
      blob.resize(static_cast<std::size_t>(size));
      if (fs_->pread(fd, 0, blob.data(), blob.size()) != FsStatus::kOk) blob.clear();
    }
    fs_->close(fd);
    if (blob.empty()) {
      last_key_status_ = ports::KeyStatus::kMalformedBlob;
      return LoadStatus::kKeyUnavailable;
    }
  }
  return load_with_blob(resolved.protected_view, resolved.library_id, resolved.instrument, blob, out, hooks);
}

LoadStatus ProtectedHeadProvider::load_with_blob(const std::shared_ptr<const fmt::ProtectedBundleView>& view, const std::string& library_id, std::size_t instrument,
                                                 const std::vector<std::uint8_t>& blob, std::shared_ptr<const BundleInstrument>& out, const LoadHooks* hooks) {
  last_key_status_ = ports::KeyStatus::kOk;
  if (!view || instrument >= view->instruments().size()) return LoadStatus::kInstrumentNotFound;
  // 2. The content key, through the port. The handle's memory is the only place the key exists.
  ports::KeyResult kr = keys_.unwrap_content_key(library_id, blob);
  if (!kr.ok()) {
    last_key_status_ = kr.status == ports::KeyStatus::kOk ? ports::KeyStatus::kInternal : kr.status;
    return LoadStatus::kKeyUnavailable;
  }
  // 3. The reader over the verified view, borrowing the key; its key check refuses a key that is not this bundle's.
  auto opened = fmt::ProtectedBundleReader::open_view(view, kr.key->bytes(), kr.key);
  if (!opened) return load_status_from(opened.error());  // kWrongContentKey: the key check of the manifest failed
  auto reader = std::make_shared<fmt::ProtectedBundleReader>(opened.take());
  // 4. Resident: hash-check, AES-CTR decrypt and decode of every block, on this (non-real-time) thread.
  const ProtectedSource source(*reader);
  LoadResult r = load_instrument_by_index(source, instrument, hooks);
  if (!r.ok()) return r.status;  // typed: content_integrity, cancelled, ...
  if (r.instrument->failed_samples() != 0) {
    return load_status_from(r.instrument->first_error());  // an I/O error is kBundleInconsistent: the instrument is not resident
  }
  auto ctx = std::make_shared<ProtectedInstrumentContext>();
  ctx->key = std::move(kr.key);
  ctx->reader = std::move(reader);
  ctx->library_id = library_id;
  ctx->instrument_key = view->instruments()[instrument].key;
  ctx->first_sample = view->instruments()[instrument].first_sample;
  ctx->sample_count = view->instruments()[instrument].sample_count;
  ctx->damage = damage_;
  r.instrument->attach_context(ctx);
  if (damage_) damage_->clear_library(library_id);  // a library that loaded cleanly again (reloaded, reinstalled) is no longer reported damaged
  // The lease on bundle.vlrp is the reader's shared view (it keeps the file open); it lives in the context.
  std::shared_ptr<const BundleInstrument> inst(r.instrument.release());
  {
    std::lock_guard<std::mutex> lock(mu_);
    loaded_.erase(std::remove_if(loaded_.begin(), loaded_.end(), [](const std::weak_ptr<const BundleInstrument>& w) { return w.expired(); }), loaded_.end());
    loaded_.push_back(inst);
  }
  out = std::move(inst);
  return LoadStatus::kOk;
}

std::vector<std::weak_ptr<const BundleInstrument>> ProtectedHeadProvider::live_weak() const {
  std::vector<std::weak_ptr<const BundleInstrument>> v;
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& w : loaded_)
    if (!w.expired()) v.push_back(w);
  return v;
}

std::vector<std::shared_ptr<const BundleInstrument>> ProtectedHeadProvider::live_instruments() const {
  std::vector<std::shared_ptr<const BundleInstrument>> v;
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& w : loaded_)
    if (auto p = w.lock()) v.push_back(std::move(p));
  return v;
}

// ---- EdgeBlockReader ----------------------------------------------------------------------------------------------------------------------------------

void EdgeBlockReader::init() {
  if (auto inst = weak_.lock()) valid_ = inst->has_damage_maps() && inst->context() != nullptr;
}
EdgeBlockReader::EdgeBlockReader(const std::shared_ptr<const BundleInstrument>& instrument) : weak_(instrument) { init(); }
EdgeBlockReader::EdgeBlockReader(std::weak_ptr<const BundleInstrument> instrument) : weak_(std::move(instrument)) { init(); }
EdgeBlockReader::~EdgeBlockReader() = default;

std::size_t EdgeBlockReader::sample_count() const {
  auto inst = weak_.lock();
  if (!inst || !valid_) return 0;
  return static_cast<const ProtectedInstrumentContext*>(inst->context().get())->sample_count;
}

fmt::Error EdgeBlockReader::read_block_impl(std::size_t sample, std::uint64_t block, std::vector<float>* out, std::uint32_t* frames) {
  if (!valid_) return fmt::Error::kInvalidArgument;
  // One strong reference for this block only: it keeps the instrument, the reader and the key alive while the block is decoded, and not a moment longer.
  const std::shared_ptr<const BundleInstrument> instrument = weak_.lock();
  if (!instrument) {
    gone_ = true;
    return fmt::Error::kIo;
  }
  const ProtectedInstrumentContext* ctx = static_cast<const ProtectedInstrumentContext*>(instrument->context().get());
  if (sample >= ctx->sample_count) return fmt::Error::kOutOfRange;
  const std::size_t flat = ctx->first_sample + sample;
  const fmt::SampleInfo& si = ctx->reader->samples()[flat];
  if (block >= si.block_count) return fmt::Error::kOutOfRange;
  const std::size_t ch = si.format.channels;
  // The decoded int32 copy of the block is wiped on EVERY return path (and the vector is released), not kept as a scratch buffer.
  std::vector<std::int32_t> tmp(std::size_t(fmt::kBlockFrames) * ch, 0);
  struct Wipe {
    std::vector<std::int32_t>& v;
    ~Wipe() { secure_wipe(v.data(), v.size() * sizeof(std::int32_t)); }
  } wipe_guard{tmp};
  auto mark_and_report = [&] {
    if (instrument->mark_block_damaged(sample, block) && ctx->damage)  // only the first detection of a block is reported
      ctx->damage->report(DamageEvent{ctx->library_id, ctx->instrument_key, sample, block, "content_integrity"});
  };
  auto r = ctx->reader->read_block(flat, block, tmp, dec_);  // hash check, AES-CTR decrypt, decode: this thread, never the audio thread
  if (!r) {
    const fmt::Error e = r.error();
    if (is_integrity_error(e)) mark_and_report();
    return e;
  }
  if (out) {
    if (!pcm_to_float(si.format, std::span<const std::int32_t>(tmp).first(std::size_t(r.value()) * ch), r.value(), *out)) {
      mark_and_report();  // decoded to something that is not finite audio: not the block that was signed
      return fmt::Error::kCorruptBlock;
    }
    if (frames) *frames = r.value();
  }
  return fmt::Error::kNone;
}

fmt::Error EdgeBlockReader::read_block_pcm(std::size_t sample, std::uint64_t block, std::vector<float>& out, std::uint32_t& frames) {
  return read_block_impl(sample, block, &out, &frames);
}

fmt::Error EdgeBlockReader::check_block(std::size_t sample, std::uint64_t block) { return read_block_impl(sample, block, nullptr, nullptr); }

ScrubResult EdgeBlockReader::scrub(const std::atomic<bool>* stop) {
  ScrubResult res;
  if (!valid_) return res;
  const std::size_t samples = sample_count();
  for (std::size_t s = 0; s < samples; ++s) {
    std::uint32_t blocks = 0;
    std::string library_id;
    DamageRegistry* damage = nullptr;
    if (auto inst = weak_.lock()) {  // the block count of this sample, from a momentary reference
      const auto* ctx = static_cast<const ProtectedInstrumentContext*>(inst->context().get());
      blocks = ctx->reader->samples()[ctx->first_sample + s].block_count;
      library_id = ctx->library_id;
      damage = ctx->damage;
    } else {
      res.generation_gone = true;
      return res;
    }
    for (std::uint64_t b = 0; b < blocks; ++b) {
      if (stop && stop->load(std::memory_order_acquire)) {
        res.stopped = true;
        return res;
      }
      const fmt::Error e = check_block(s, b);
      if (gone_) {
        res.generation_gone = true;
        return res;
      }
      ++res.blocks_checked;
      if (e == fmt::Error::kNone) continue;
      if (is_integrity_error(e)) {
        ++res.damaged;
      } else {
        ++res.io_errors;  // says nothing about the block: not marked, but counted and shown (State.content.io_errors)
        if (damage) damage->report_io_error(library_id);
      }
    }
  }
  return res;
}

}  // namespace velora::host
