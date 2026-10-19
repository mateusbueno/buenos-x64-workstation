# library-gen

Synthetic library tree generator for scale tests (Story 1.14). Writes the layout the host library resolver reads
(`<root>/lib/<library_id>/current -> <bundle_id>/` and `<bundle_id>/bundle.vlb`; names shared via
`velora/host/library_resolver.hpp`). Each library is a small real formatlib bundle with two instruments
(`piano/soft.sfz`, `piano/loud.sfz`) and a distinct deterministic `library_id`.

```
build/tools/library-gen/velora-library-gen --root /tmp/libs --libraries 1000 --pad-gib 1
```

`--pad-gib G` adds a sparse `padding.bin` of G GiB next to each bundle (not part of the bundle; no disk used) to emulate
library size. `--first-index K` offsets library numbering. C++ API: `src/library_gen.hpp` (target `velora_libgen`).
