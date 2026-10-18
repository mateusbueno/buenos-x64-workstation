// Control: RT-safe code inside a [[clang::nonblocking]] function must pass RTSan.
#include <cstdio>

static int mix(int a, int b) [[clang::nonblocking]] { return a + b; }

int main() {
  volatile int r = mix(1, 2);
  std::printf("rtsan_safe_test: %d\n", static_cast<int>(r));
  return r == 3 ? 0 : 1;
}
