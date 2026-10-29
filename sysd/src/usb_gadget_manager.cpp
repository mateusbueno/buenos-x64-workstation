#include "velora/sysd/usb_gadget_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace velora::sysd {

namespace fs = std::filesystem;

UsbGadgetManager::UsbGadgetManager(std::string gadget_path)
    : gadget_path_(std::move(gadget_path)) {
  std::error_code ec;
  if (fs::exists(gadget_path_, ec)) {
    // Read vendor / product ID if present
    auto read_hex = [](const fs::path& p, std::uint16_t& out) {
      std::ifstream f(p);
      if (f.is_open()) {
        std::string s;
        if (f >> s) {
          try {
            out = static_cast<std::uint16_t>(std::stoul(s, nullptr, 16));
          } catch (...) {}
        }
      }
    };
    auto read_str = [](const fs::path& p, std::string& out) {
      std::ifstream f(p);
      if (f.is_open()) {
        std::getline(f, out);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
          out.pop_back();
        }
      }
    };

    read_hex(fs::path(gadget_path_) / "idVendor", config_.id_vendor);
    read_hex(fs::path(gadget_path_) / "idProduct", config_.id_product);
    read_str(fs::path(gadget_path_) / "strings/0x409/product", config_.product);
    read_str(fs::path(gadget_path_) / "strings/0x409/manufacturer", config_.manufacturer);
    read_str(fs::path(gadget_path_) / "strings/0x409/serialnumber", config_.serial_number);
  }
}

std::vector<std::string> UsbGadgetManager::get_active_functions() const {
  std::vector<std::string> funcs;
  std::error_code ec;
  fs::path config_dir = fs::path(gadget_path_) / "configs" / "c.1";
  if (!fs::exists(config_dir, ec) || !fs::is_directory(config_dir, ec)) {
    if (config_.uac2_enabled) funcs.push_back("uac2.usb0");
    if (config_.midi_enabled) funcs.push_back("midi.usb0");
    if (config_.ncm_enabled) funcs.push_back("ncm.usb0");
    return funcs;
  }

  for (const auto& entry : fs::directory_iterator(config_dir, ec)) {
    if (entry.is_symlink(ec) || entry.is_directory(ec)) {
      std::string name = entry.path().filename().string();
      if (name != "strings" && name != "MaxPower") {
        funcs.push_back(name);
      }
    }
  }
  std::sort(funcs.begin(), funcs.end());
  return funcs;
}

bool UsbGadgetManager::validate_composition() const {
  auto funcs = get_active_functions();
  if (has_forbidden_functions()) {
    return false;
  }

  bool has_uac2 = false;
  bool has_midi = false;
  bool has_ncm = false;

  for (const auto& fn : funcs) {
    if (fn.rfind("uac2", 0) == 0) {
      has_uac2 = true;
    } else if (fn.rfind("midi", 0) == 0) {
      has_midi = true;
    } else if (fn.rfind("ncm", 0) == 0) {
      has_ncm = true;
    } else {
      return false; // Unknown or unpermitted function
    }
  }

  return has_uac2 && has_midi && has_ncm;
}

bool UsbGadgetManager::has_forbidden_functions() const {
  auto funcs = get_active_functions();
  for (const auto& fn : funcs) {
    if (fn.find("mass_storage") != std::string::npos ||
        fn.find("storage") != std::string::npos ||
        fn.find("acm") != std::string::npos ||
        fn.find("serial") != std::string::npos ||
        fn.find("rndis") != std::string::npos ||
        fn.find("ecm") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool UsbGadgetManager::is_mass_storage_exposed() const {
  auto funcs = get_active_functions();
  for (const auto& fn : funcs) {
    if (fn.find("mass_storage") != std::string::npos || fn.find("storage") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool UsbGadgetManager::is_serial_console_exposed() const {
  auto funcs = get_active_functions();
  for (const auto& fn : funcs) {
    if (fn.find("acm") != std::string::npos || fn.find("serial") != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string UsbGadgetManager::get_bound_udc() const {
  std::error_code ec;
  fs::path udc_path = fs::path(gadget_path_) / "UDC";
  if (!fs::exists(udc_path, ec)) {
    return "";
  }
  std::ifstream f(udc_path);
  std::string udc;
  if (f >> udc) {
    return udc;
  }
  return "";
}

GadgetStatus UsbGadgetManager::get_status() const {
  std::error_code ec;
  if (!fs::exists(gadget_path_, ec)) {
    return GadgetStatus::kUnbound;
  }
  std::string udc = get_bound_udc();
  if (udc.empty()) {
    return GadgetStatus::kUnbound;
  }
  if (!validate_composition()) {
    return GadgetStatus::kError;
  }
  return GadgetStatus::kBound;
}

}  // namespace velora::sysd
