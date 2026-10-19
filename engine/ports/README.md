# engine/ports

Role: port interfaces through which `engine/core` reaches the outside world; implemented by adapters in `engine/host/`.

Dependency rule: header-only interfaces that may depend only on the C++ standard library (and other ports).

Sample-source port (Story 1.15, `velora/ports/sample_source.hpp`): an instrument's region table (`SampleRegionDesc`) and fully RAM-resident float PCM, planar per channel (`SamplePcm`), as plain structs and spans. `SampleEngine` in `engine/core` consumes it; `engine/host` (`sample_bundle_loader`) fills it from a formatlib bundle. The source owns the memory and must outlive the engine's use of it.
