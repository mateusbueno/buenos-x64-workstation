# engine/host

Role: host/OS adapters (ALSA, UMP, input edge threads, io_uring streaming, OP-TEE, control thread) implementing `engine/ports`.

Dependency rule: may depend on `engine/core`, `engine/ports` and OS libraries; `engine/core` must never depend on it.
