# Reverse engineering with NUE

A recompiled game is only as faithful as the layer that answers its calls. To write that layer you have to see what the game does:
which calls, in which order, with which arguments, and what the original platform did in response. NUE is built so that this is
a loop you can repeat quickly.

```
 record  ->  trace  ->  replay  ->  compare  ->  fix the layer  ->  record again
```

## What exists today (Direct3D 9)

| Step | Tool | Where |
|---|---|---|
| Record | The call recorder in the recompiled game's launcher captures every Direct3D 9 call into a versioned trace | game project |
| Inspect | `d3d9_trace_info.py` summarises a trace, `d3d9_trace_census.py` counts calls and states, `d3d9_trace_samplers.py` lists sampler use | `src/legacy/d3d9/scripts/` |
| Shaders | `scan_d3d9_shaders.py` finds and classifies the shader bytecode a game uses | `src/legacy/d3d9/scripts/` |
| Replay | `nue_replay` runs a trace with no game: headless with a deterministic call hash (`--null --twice`), through bgfx (`--bgfx d3d11`), or against the real Direct3D 9 as ground truth (`--native`) | `src/legacy/d3d9/tools/` |
| Bisect | Replay only a range of draws (`--draws a-b`) to find the call where the output starts to differ | `nue_replay` |
| Dump | Environment switches write the translated shaders (`NUE_DUMP_GLSL`), save textures and list the last draws before a frame | `nue_replay` |
| Compare images | `bmp_diff_grid.py` and `bmp_compare_crop.py` turn two renders into a difference map and cropped views | `src/legacy/d3d9/scripts/` |
| Ground truth | `nue_format_test` and `nue_state_test` run the same calls through the layer and the system's Direct3D 9 and compare pixels | `src/legacy/d3d9/tools/` |
| Observe live | The layer writes an audit log and `timeline:` and `spike:` lines that say where a frame's time went | `nue_d3d9.log` |

The ground-truth tests cover over a hundred state cases and a set of texture formats. Their results are listed in
[the Direct3D 9 notes](../src/legacy/d3d9/docs/D3D9_BGFX.md).

## Working with it

1. **Record a real session.** Play a few minutes and keep the trace.
2. **Look at the trace.** Run the census to see which calls, formats and states the game actually uses. That is the real coverage
   target, which is usually much smaller than the whole API.
3. **Replay with the null backend.** Run it twice. The two call hashes must match. A mismatch means non-determinism in the layer.
4. **Replay with bgfx and with the reference.** Render the same frames both ways and compare.
5. **Bisect.** If the images differ, replay a shrinking range of draws until one call explains it.
6. **Write a ground-truth test** for the behaviour you found, so it never regresses.
7. **Fix the layer** and go back to step 4.

## Planned

- A shared trace container, so every legacy layer records and replays in the same format.
- A replay harness that runs any layer's trace headless, with the same flags.
- Recorders and reference comparisons for the audio and input layers.
- Per-layer coverage reports generated from the traces, instead of kept by hand.
- Tools for command-stream consoles: a display-list viewer and a step-by-step interpreter.
