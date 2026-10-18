# engine/ports

Role: port interfaces through which `engine/core` reaches the outside world; implemented by adapters in `engine/host/`.

Dependency rule: header-only interfaces that may depend only on the C++ standard library (and other ports).
