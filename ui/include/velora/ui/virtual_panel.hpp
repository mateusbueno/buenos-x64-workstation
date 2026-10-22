#pragma once

#include <vector>
#include <functional>
#include "velora/ui/panel_event.hpp"

namespace velora::ui {

class VirtualPanel {
 public:
  static VirtualPanel& instance() {
    static VirtualPanel s_instance;
    return s_instance;
  }

  void add_listener(PanelEventListener listener) {
    listeners_.push_back(std::move(listener));
  }

  void clear_listeners() {
    listeners_.clear();
  }

  void dispatch(const PanelEvent& ev) {
    for (auto& l : listeners_) {
      if (l) l(ev);
    }
  }

  // Translates keycode and pressed state to PanelEvent. Returns true if key was handled.
  bool handle_key(int32_t keycode, bool pressed);

 private:
  VirtualPanel() = default;
  std::vector<PanelEventListener> listeners_;
};

}  // namespace velora::ui
