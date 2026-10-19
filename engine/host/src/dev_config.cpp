#include "velora/host/dev_config.hpp"

#include <google/protobuf/util/json_util.h>

#include <fstream>
#include <sstream>

namespace velora::host {

bool parse_dev_config(const std::string& json, ws::v1::State& out, std::string& err) {
  out.Clear();
  google::protobuf::util::JsonParseOptions opt;
  opt.ignore_unknown_fields = false;
  const auto st = google::protobuf::util::JsonStringToMessage(json, &out, opt);
  if (!st.ok()) {
    err = std::string(st.message());
    return false;
  }
  return true;
}

bool load_dev_config(const std::string& path, ws::v1::State& out, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = path + ": cannot open the engine dev configuration";
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string why;
  if (!parse_dev_config(ss.str(), out, why)) {
    err = path + ": invalid engine dev configuration: " + why;
    return false;
  }
  return true;
}

}  // namespace velora::host
