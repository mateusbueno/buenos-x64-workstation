#pragma once
// Protected libraries in the engine (Story 6.5, FR30/FR31/FR32, AR19/AR20). Target velora_host_protected: the ONLY engine-side code that links the decrypting
// reader (formatlib velora_format_decrypt, OpenSSL). engine/core never sees any of it; velora_host (and with it the librarian) stays free of cipher code.
//
//   ProtectedHeadProvider  a HeadProvider (live_set_loader.hpp). For an UNPROTECTED instrument it is the RamHeadProvider, unchanged. For a protected one
//                          (LibraryResolver verified the signature and found bundle.vlrp) the control / staging thread
//                            1. reads the wrapped_key_v1 blob stored next to the bundle,
//                            2. obtains the content key through the key-provider port (engine/ports): the handle owns mlocked, non-dumpable, wiped-on-release memory,
//                            3. opens a ProtectedBundleReader over the verified view BORROWING that key (no copy of the key anywhere), whose key check refuses a
//                               key that does not belong to the bundle (wrong_content_key),
//                            4. makes the instrument resident through the ordinary loader: every ciphertext block is hash-checked and AES-CTR decrypted HERE, on
//                               the loading thread, and the audio thread later reads decoded float PCM from RAM exactly as for any other library.
//                          A block that fails its hash check refuses the load with the typed LoadStatus::kContentIntegrity; nothing partial stays resident.
//                          The reader (which holds the key) is hung on the BundleInstrument (attach_context), and the instrument is what every generation that plays
//                          it shares (SampleBank::owner): the key is attached to the generation, refcounted with its lifetime, and zeroized when the generation is
//                          freed after its last voice ended.
//   EdgeBlockReader       the same decrypt + decode on an I/O edge thread, block by block: how a streamer (Story 1.17) fills its buffers from a protected
//                          bundle, and how the integrity scrub re-verifies a resident instrument against its signed index. A block that fails is marked damaged in
//                          the instrument's damage map (the voices fade to silence for it, SampleEngine), counted, and the library is reported to the DamageRegistry.
//
// Threading: every class here runs on control, staging or I/O threads, never on the audio thread (they allocate, read files, hash and run AES).
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "velora/format/protected_reader.hpp"
#include "velora/host/damage_registry.hpp"
#include "velora/host/fs_port.hpp"
#include "velora/host/live_set_loader.hpp"
#include "velora/ports/key_provider.hpp"

namespace velora::host {

// Hung on a protected BundleInstrument (BundleInstrument::attach_context).
struct ProtectedInstrumentContext {
  std::shared_ptr<const velora::ports::ContentKeyHandle> key;  // the content key (the reader borrows it); wiped when the last reference goes
  std::shared_ptr<velora::format::ProtectedBundleReader> reader;  // destroyed before `key`
  std::string library_id, instrument_key;
  std::size_t first_sample = 0;  // flat sample index of the instrument's first sample in the bundle
  std::size_t sample_count = 0;
  DamageRegistry* damage = nullptr;  // where a damaged block is reported (may be null)
};

class ProtectedHeadProvider : public RamHeadProvider {
 public:
  // `fs` reads wrapped-key.bin (the same FsPort the resolver uses). `keys` and `damage` must outlive the provider and every instrument it loaded.
  ProtectedHeadProvider(std::shared_ptr<FsPort> fs, velora::ports::KeyProvider& keys, DamageRegistry* damage = nullptr, SampleLoadOptions options = {});

  LoadStatus load(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out) override;
  LoadStatus load_controlled(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out, const LoadHooks& hooks) override;

  // The tail of a protected load for a caller that holds the verified view and the wrapped-key blob itself (tools/offline-render takes both from command-line
  // paths): unwrap through the port, open the reader borrowing the key, make instrument `instrument` of the view resident. Same typed failures as load().
  LoadStatus load_with_blob(const std::shared_ptr<const velora::format::ProtectedBundleView>& view, const std::string& library_id, std::size_t instrument,
                            const std::vector<std::uint8_t>& blob, std::shared_ptr<const BundleInstrument>& out, const LoadHooks* hooks = nullptr);

  // Why the last load that failed in the key provider failed (KeyStatus::kOk otherwise). One thread at a time, like the provider itself.
  velora::ports::KeyStatus last_key_status() const { return last_key_status_; }

  // The protected instruments this provider loaded that are still alive (some generation holds them).
  std::vector<std::shared_ptr<const BundleInstrument>> live_instruments() const;
  // The same as weak references: what an edge thread should hold, so that its scrub never keeps a generation (and with it the content key) alive.
  std::vector<std::weak_ptr<const BundleInstrument>> live_weak() const;

 private:
  LoadStatus load_protected(const ResolveResult& resolved, std::shared_ptr<const BundleInstrument>& out, const LoadHooks* hooks);
  std::shared_ptr<FsPort> fs_;
  velora::ports::KeyProvider& keys_;
  DamageRegistry* damage_;
  velora::ports::KeyStatus last_key_status_ = velora::ports::KeyStatus::kOk;
  mutable std::mutex mu_;
  std::vector<std::weak_ptr<const BundleInstrument>> loaded_;
};

struct ScrubResult {
  std::uint64_t blocks_checked = 0;
  std::uint64_t damaged = 0;     // blocks that failed their integrity check in this pass (each marked, counted and reported)
  std::uint64_t io_errors = 0;   // reads that failed for a reason that says nothing about the block (not marked; counted and reported to the DamageRegistry)
  bool stopped = false;          // the stop flag ended the pass early
  bool generation_gone = false;  // the instrument was freed (its generation ended) while the pass ran: the pass stopped at once
};

class EdgeBlockReader {
 public:
  // `instrument` must have been loaded by a ProtectedHeadProvider (valid() says so). The reader holds it WEAKLY and takes a reference only for the duration of one
  // block: a scrub never keeps a generation alive, so the key is wiped when the generation is freed even while a scrub is running (the scrub then ends,
  // generation_gone).
  explicit EdgeBlockReader(const std::shared_ptr<const BundleInstrument>& instrument);
  explicit EdgeBlockReader(std::weak_ptr<const BundleInstrument> instrument);
  ~EdgeBlockReader();
  bool valid() const { return valid_; }
  std::size_t sample_count() const;

  // Verifies the ciphertext hash of block `block` of the instrument's sample `sample`, decrypts it (AES-CTR), decodes it and converts it to planar float
  // (`out`: left then right, frames * channels values; `frames` = frames of the block). On a block that fails its integrity check (hash mismatch, or bytes that
  // do not decode) the block is marked damaged in the instrument's map, reported to the DamageRegistry, `out` is untouched and the error is returned. Any other
  // error (kIo, kOutOfRange) is returned without marking anything. This is the call a streamer makes to fill a ring buffer.
  velora::format::Error read_block_pcm(std::size_t sample, std::uint64_t block, std::vector<float>& out, std::uint32_t& frames);
  // The same without keeping the PCM (the scrub).
  velora::format::Error check_block(std::size_t sample, std::uint64_t block);
  // Checks every block of every sample of the instrument, in order; ends early when `stop` becomes true.
  ScrubResult scrub(const std::atomic<bool>* stop = nullptr);

 private:
  velora::format::Error read_block_impl(std::size_t sample, std::uint64_t block, std::vector<float>* out, std::uint32_t* frames);
  void init();
  std::weak_ptr<const BundleInstrument> weak_;
  bool valid_ = false, gone_ = false;
  velora::format::BlockDecoder dec_;
};

// True for the integrity class of errors (a block that does not match its signed hash, or does not decode): the ones that mark a block damaged.
bool is_integrity_error(velora::format::Error e);

}  // namespace velora::host
