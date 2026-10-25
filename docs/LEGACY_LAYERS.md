# Writing a legacy layer

A legacy layer answers the calls of one source platform and implements them on top of the native ABI. `legacy_d3d9`
(`src/legacy/d3d9`, the `nue_d3d9` translator) is the first and the reference. This page lists what it is made of, so the next layer
starts from a known shape instead of a blank page. See [ROADMAP.md](ROADMAP.md) for where layers are going.

## Anatomy, using Direct3D 9

```
 game calls  ->  facade            translates the game's calling convention and objects      (outside the library)
                    |
              Level A: semantics   the source API as plain handle-based calls                  include/nue/d3d9_device.hpp
                    |
              layer                state shadow, resources and locks, formats, shader          src/layer
                                   translation, fixed function as generated shaders
                    |
              Level B: GPU         what a modern API does, kept small                          include/nue/gpu.hpp
                    |
              adapters             bgfx for real output, null for deterministic tests          bgfx/, src/null_gpu.cpp
```

| Part | What it is for |
|---|---|
| Facade | The only code that knows the game's object model (COM, guest pointers, calling convention). It stays outside the library, so the library has no Windows types in its interface. |
| Level A | The source API with its exact semantics, as handles and spans. Every behaviour the game can observe lives here or below. |
| Layer | State tracking, state blocks, resources, locks, format conversion, shader translation. This is where most of the work and most of the bugs are. |
| Level B | A small interface for what any modern GPU API does. A new renderer implements this and nothing else. |
| Adapters | One for real output (bgfx) and one `null` adapter that records a deterministic hash of every call, so tests need no window or GPU. |
| Trace and replay | Record the calls a real game makes, replay them headless, compare. See [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md). |
| Ground-truth tests | The same calls run through the layer and through the reference implementation, with the results compared. |

## Checklist for a new layer

1. **Name and place.** `src/legacy/<api>/`, target `nue_legacy_<api>`, built on `nue::nue` only, never on another layer's internals.
2. **Interface first.** Write Level A as handles and spans. No platform types, no guest pointers, no globals.
3. **Null adapter early.** Make the layer work against a null backend that hashes every call, and test against that hash from day one.
4. **Trace format.** Decide what a recorded call looks like before recording anything. Version it.
5. **Reference.** Decide what the layer is compared with (the real runtime, a known-good implementation, captured output) and write the comparison as a test.
6. **Coverage table.** Keep an honest list of what is implemented, what is accepted and ignored, and what is missing.
7. **Known differences.** Write down every place the layer behaves differently from the original and why.
8. **No game knowledge.** Patches for one game belong in that game's project, not in the layer.
9. **Budgets.** Memory use is a budget the host sets at run time. Do not hard-code limits.

## What the Direct3D 9 layer teaches

- Ground-truth tests find the bugs a game never shows: wrong channel order in a 16-bit format, signed versus unsigned
  normalised formats, texture transforms that must not apply to pre-transformed vertices. Run them before trusting a replay.
- A deterministic null backend makes regressions visible without any image comparison.
- Recording real traces from the game, then replaying them, finds the cases the specification does not mention.
- Keep the layer free of the game. The same layer then serves the next game that uses the same API.
