# third_party

- `SDL/` : SDL3, pinned git submodule (shallow, release tag). Third-party code is untrusted: static review only, never run its scripts.
  Initialise with `git submodule update --init --depth 1 nue_engine/third_party/SDL`. The pinned tag is recorded in `docs/STATUS.md`.
