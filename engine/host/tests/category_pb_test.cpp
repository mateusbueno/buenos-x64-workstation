// Story 5.5: proto/category.proto as compiled by protoc equals formatlib's category.hpp (ids, names, count).
#include <cctype>
#include <cstdio>
#include <string>

#include "category.pb.h"
#include "velora/format/category.hpp"

namespace fmt = velora::format;
namespace vlib = velora::library::v1;

int main() {
  int fail = 0;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      std::printf("FAIL %s\n", what);
      ++fail;
    }
  };
  const google::protobuf::EnumDescriptor* d = vlib::Category_descriptor();
  check(d->value_count() == static_cast<int>(fmt::kCategoryCount) + 1, "value count");
  check(vlib::Category_MAX == static_cast<int>(fmt::kCategoryCount) && vlib::Category_MIN == 0, "range");
  check(vlib::CATEGORY_NONE == 0 && !fmt::is_assignable(static_cast<fmt::Category>(vlib::CATEGORY_NONE)), "none");
  for (unsigned i = 1; i <= fmt::kCategoryCount; ++i) {
    const auto c = static_cast<fmt::Category>(i);
    std::string up(fmt::slug(c));
    for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    check(vlib::Category_IsValid(static_cast<int>(i)), "valid id");
    check(vlib::Category_Name(static_cast<vlib::Category>(i)) == "CATEGORY_" + up, "name of id");
    check(fmt::is_assignable(c), "assignable");
  }
  check(!vlib::Category_IsValid(18) && !vlib::Category_IsValid(-1), "no extra ids");
  if (fail) return 1;
  std::printf("category_pb_test: ok\n");
  return 0;
}
