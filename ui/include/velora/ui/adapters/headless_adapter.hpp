#pragma once

#include <vector>
#include <cstdint>
#include "velora/ui/display_adapter.hpp"

namespace velora::ui {

class HeadlessAdapter : public IDisplayAdapter {
 public:
  HeadlessAdapter() = default;
  ~HeadlessAdapter() override { shutdown(); }

  bool initialize(int width, int height) override;
  void update() override;
  void shutdown() override;
  bool should_close() const override { return should_close_; }
  lv_display_t* get_display() override { return display_; }
  AdapterType type() const noexcept override { return AdapterType::Headless; }

  void request_close() { should_close_ = true; }

 private:
  lv_display_t* display_{nullptr};
  std::vector<uint32_t> buf1_;
  std::vector<uint32_t> buf2_;
  bool should_close_{false};
};

}  // namespace velora::ui
