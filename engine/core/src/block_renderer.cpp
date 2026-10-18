#include "velora/core/block_renderer.hpp"

#include <cassert>

namespace velora::core {

BlockRenderer::BlockRenderer(std::uint32_t test_tone_voices)
    : arena_(64 * 1024), tone_(arena_, test_tone_voices) {
  bound_ = registry_.bind(tone_);
  assert(bound_);
  channel_engine_.fill(static_cast<std::uint32_t>(EngineTypeId::kTestTone));
}

void BlockRenderer::record(EngineStatus s) noexcept {
  ++status_counts_[static_cast<std::size_t>(s)];
  last_status_ = s;
}

void BlockRenderer::drain_queue() noexcept {
  TimedUmp ev;
  while (queue_.pop(ev)) {
    if (pending_count_ >= kPendingCapacity) {
      record(EngineStatus::kEventOverflow);  // drop, but keep the queue draining
      continue;
    }
    // Stable insertion: after the last element with target_frame <= ev.target_frame.
    std::size_t pos = pending_count_;
    while (pos > 0 && pending_[pos - 1].target_frame > ev.target_frame) {
      pending_[pos] = pending_[pos - 1];
      --pos;
    }
    pending_[pos] = ev;
    ++pending_count_;
  }
}

void BlockRenderer::apply(const Ump& ump) noexcept {
  if (ump.group() != 0 || (!ump.is_note_on() && !ump.is_note_off())) {
    ++ignored_;
    return;
  }
  Engine* engine = nullptr;
  const EngineStatus rs = registry_.resolve(channel_engine_[ump.channel()], engine);
  if (rs != EngineStatus::kOk) {
    record(rs);  // silence: nothing is rendered for this request
    return;
  }
  if (ump.is_note_on())
    record(engine->start_voice(ump.channel(), ump.note(), ump.velocity16()));
  else
    record(engine->stop_voice(ump.channel(), ump.note()));
}

void BlockRenderer::render_segment(float* l, float* r, std::uint32_t from, std::uint32_t to) noexcept {
  if (to <= from) return;
  for (std::size_t i = 0; i < registry_.size(); ++i) registry_.at(i)->render(l + from, r + from, to - from);
}

void BlockRenderer::process_block(float* left, float* right) noexcept {
  if (!left || !right) return;
  for (std::uint32_t i = 0; i < kBlockFrames; ++i) left[i] = right[i] = 0.0f;
  drain_queue();

  std::uint32_t pos = 0;
  std::size_t consumed = 0;
  while (consumed < pending_count_ && pending_[consumed].target_frame < kBlockFrames) {
    const std::uint32_t f = pending_[consumed].target_frame;
    render_segment(left, right, pos, f);
    pos = f;
    apply(pending_[consumed].ump);
    ++consumed;
  }
  render_segment(left, right, pos, kBlockFrames);

  // Drop applied events, carry the rest into the next block.
  const std::size_t remaining = pending_count_ - consumed;
  for (std::size_t i = 0; i < remaining; ++i) {
    pending_[i] = pending_[consumed + i];
    pending_[i].target_frame -= kBlockFrames;
  }
  pending_count_ = remaining;
}

}  // namespace velora::core
