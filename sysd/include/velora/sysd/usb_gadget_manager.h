#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace velora::sysd {

enum class GadgetStatus {
  kUnbound = 0,
  kBound = 1,
  kError = 2,
};

struct UsbGadgetConfig {
  std::uint16_t id_vendor{0x1209};
  std::uint16_t id_product{0x5673};
  std::string manufacturer{"Velora"};
  std::string product{"Velora 73"};
  std::string serial_number{"VELORA73-DEV001"};
  bool uac2_enabled{true};
  bool midi_enabled{true};
  bool ncm_enabled{true};
};

class UsbGadgetManager {
 public:
  explicit UsbGadgetManager(std::string gadget_path = "/sys/kernel/config/usb_gadget/velora");

  const std::string& gadget_path() const noexcept { return gadget_path_; }

  // Inspects configured functions in configs/c.1 and returns true only if
  // exactly the permitted functions (uac2, midi, ncm) are present.
  bool validate_composition() const;

  // Invariant (NFR24, AR38): Check whether any mass storage or ACM serial functions exist.
  bool has_forbidden_functions() const;

  // Check whether mass storage is exposed (strictly must be false).
  bool is_mass_storage_exposed() const;

  // Check whether serial/ACM console is exposed (strictly must be false).
  bool is_serial_console_exposed() const;

  // Query active bound UDC driver. Returns empty string if unbound.
  std::string get_bound_udc() const;

  GadgetStatus get_status() const;

  std::vector<std::string> get_active_functions() const;

  const UsbGadgetConfig& config() const noexcept { return config_; }

 private:
  std::string gadget_path_;
  UsbGadgetConfig config_;
};

}  // namespace velora::sysd
