# NUE architecture

## Layers

```
 game / app (recompiled NFSU2, later Most Wanted, samples)
        |  C API  (include/nue/*.h)     C++ wrapper (include/nuepp/*.hpp)
 +------v------------------------------------------------------------------+
 | CORE (src/core, platform independent)                                   |
 |  init, log, config, hashing, clocks, threads | event queue + state tables |
 |  last-used-device | VFS + async I/O + import | audio streams + MIXER     |
 |  power request bookkeeping | diag (reports, counters) | gfx lifecycle    |
 +------+------------------------------------------+-----------------------+
        | backend.h (devices)                       | platform_ops.h (OS extras)
 +------v-----------+                       +-------v--------------------------+
 | backend: null    |                       | platform: windows/linux/android/ios |
 | backend: sdl3    |                       | game mode, power, storage rules,   |
 | (window, input,  |                       | sysinfo, thread prio, crash, SAF   |
 |  audio, time)    |                       +------------------------------------+
 +------------------+
        gfx: nue_gfx (bgfx) <-- listeners --> nue_d3d9 (Direct3D 9 -> bgfx), linked by the game
```

* **Core** owns policy and everything testable without hardware. It never includes SDL or OS headers except in the small `os_*` files
  (file I/O, threads, clocks) with `#ifdef` per OS.
* **Backends** produce raw events and devices. They never keep game state. `null` is complete and deterministic; `sdl3` maps SDL3.
* **Platform ops** is the only place for OS specific policy. A missing entry degrades gracefully and clears the capability bit.
* **State tables**: `nue_internal_push_event` is the single entry for events; it updates key/mouse/gamepad snapshots, the
  last-used-device tracker, runs watchers, then queues. Polling functions read the snapshots.

## Ownership

* The context (`nue_init`) owns: log sinks, config store, VFS mounts, event queue, backend, platform layer, every window/stream/mixer/file
  still open at `nue_shutdown` (they are closed with a WARN log).
* `nue_window`/`nue_gfx`/`nue_audio_stream`/`nue_mixer`/`nue_thread` handles are owned by the caller until destroyed; destroy order is free
  (`nue_gfx` listens for window destruction and invalidates itself; streams die with their mixer).
* Attached native windows are never destroyed by NUE.
* Memory: engine allocations go through the allocator hook; caller buffers are never retained after a call except where documented
  (async read buffers, `copy_data == FALSE` mixer buffers).

## Threads

* **Event thread**: init, pump, window/cursor/text-input changes. Rebind with `nue_set_event_thread` (the host game creates windows on its own thread).
* **Audio thread**(s): owned by the backend device; run stream callbacks / `nue_mixer_render`. Real-time rules in `audio.h`.
* **I/O workers**: a small pool for `nue_io_read_async`, imports and `nue_vfs_verify_manifest` helpers.
* **Render thread**: the thread that owns bgfx (the game thread for the D3D9 layer). `nue_gfx_present`, `nue_frame_mark` there.
* Locks: one lock per subsystem, never held across callbacks (log sink lock excepted, documented), lock order core -> backend -> platform.

## Time

`nue_time_ns()` is the one clock (monotonic). Event timestamps, audio positions and frame statistics use it. The null backend can replace it
(deterministic mode). Sleeping: `NUE_SLEEP_PRECISE` sleeps most of the interval and spins/yields for the last ~1 ms; on Windows the platform
layer supplies a high-resolution waitable timer through `platform_ops.sleep_until` and the timer-resolution request.

## Storage

Roots are directory mounts with an overlay order; path resolution is: normalise -> split root -> for each mount (newest first): exact
match, else per-segment case-insensitive match from a cached listing. Write targets the first writable mount. File ids are generation-checked slots in a table
(no pointers leak out). Async reads run on the I/O pool. Imports copy through `import_source_ops` into the destination root with resumable, verified writes.

## Audio

`stream` = format conversion + queue (push) or direct callback, on top of one backend device. `mixer` = voices -> buses -> master in float, resampled
linearly, rendered in blocks (default 256 frames); a mixer attached to a stream renders inside the stream callback; manual mode renders on demand (tests).
Ring voices keep a circular buffer and publish play/write cursors per block (the DirectSound streaming-buffer pattern).

## Graphics seam

See `GFX_D3D9_SEAM.md`.
