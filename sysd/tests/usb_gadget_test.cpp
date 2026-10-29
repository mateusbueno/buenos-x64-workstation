#include "velora/sysd/usb_gadget_manager.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace velora::sysd;

void test_default_config() {
  UsbGadgetManager mgr("/nonexistent/gadget/path");
  assert(mgr.config().id_vendor == 0x1209);
  assert(mgr.config().id_product == 0x5673);
  assert(mgr.config().product == "Velora 73");
  assert(mgr.config().manufacturer == "Velora");
  assert(!mgr.is_mass_storage_exposed());
  assert(!mgr.is_serial_console_exposed());
  assert(!mgr.has_forbidden_functions());
  assert(mgr.validate_composition());
  std::cout << "  PASS test_default_config" << std::endl;
}

void test_mocked_configfs() {
  fs::path tmp_dir = fs::temp_directory_path() / "velora_usb_test_mock";
  std::error_code ec;
  fs::remove_all(tmp_dir, ec);
  fs::create_directories(tmp_dir / "configs/c.1", ec);
  fs::create_directories(tmp_dir / "functions/uac2.usb0", ec);
  fs::create_directories(tmp_dir / "functions/midi2.usb0", ec);
  fs::create_directories(tmp_dir / "functions/ncm.usb0", ec);
  fs::create_directories(tmp_dir / "strings/0x409", ec);

  // Write descriptors
  {
    std::ofstream(tmp_dir / "idVendor") << "0x1209\n";
    std::ofstream(tmp_dir / "idProduct") << "0x5673\n";
    std::ofstream(tmp_dir / "strings/0x409/product") << "Velora 73\n";
    std::ofstream(tmp_dir / "strings/0x409/manufacturer") << "Velora\n";
    std::ofstream(tmp_dir / "strings/0x409/serialnumber") << "TEST_SERIAL_12345\n";
    std::ofstream(tmp_dir / "UDC") << "fc000000.usb\n";
  }

  // Create function links in config 1
  fs::create_directory_symlink(tmp_dir / "functions/uac2.usb0", tmp_dir / "configs/c.1/uac2.usb0", ec);
  fs::create_directory_symlink(tmp_dir / "functions/midi2.usb0", tmp_dir / "configs/c.1/midi.usb0", ec);
  fs::create_directory_symlink(tmp_dir / "functions/ncm.usb0", tmp_dir / "configs/c.1/ncm.usb0", ec);

  UsbGadgetManager mgr(tmp_dir.string());
  assert(mgr.config().id_vendor == 0x1209);
  assert(mgr.config().id_product == 0x5673);
  assert(mgr.config().product == "Velora 73");
  assert(mgr.config().manufacturer == "Velora");
  assert(mgr.config().serial_number == "TEST_SERIAL_12345");
  assert(mgr.get_bound_udc() == "fc000000.usb");
  assert(mgr.get_status() == GadgetStatus::kBound);
  assert(mgr.validate_composition());
  assert(!mgr.is_mass_storage_exposed());
  assert(!mgr.is_serial_console_exposed());
  assert(!mgr.has_forbidden_functions());

  // Inject forbidden mass_storage
  fs::create_directory_symlink(tmp_dir / "functions", tmp_dir / "configs/c.1/mass_storage.usb0", ec);
  assert(mgr.is_mass_storage_exposed());
  assert(mgr.has_forbidden_functions());
  assert(!mgr.validate_composition());
  assert(mgr.get_status() == GadgetStatus::kError);

  fs::remove_all(tmp_dir, ec);
  std::cout << "  PASS test_mocked_configfs" << std::endl;
}

int main() {
  std::cout << "== Running UsbGadgetManager Tests ==" << std::endl;
  test_default_config();
  test_mocked_configfs();
  std::cout << "All UsbGadgetManager tests passed!" << std::endl;
  return 0;
}
