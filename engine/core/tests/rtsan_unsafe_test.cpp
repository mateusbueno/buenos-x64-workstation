// Deliberately RT-unsafe: allocates inside a [[clang::nonblocking]] function.
// RealtimeSanitizer must abort this process; CTest registers it WILL_FAIL.
#include <cstdlib>

static void* unsafe_alloc() [[clang::nonblocking]] {
  void* volatile p = std::malloc(64);  // RTSan violation
  return p;
}

int main() {
  void* p = unsafe_alloc();
  std::free(p);
  return 0;
}
