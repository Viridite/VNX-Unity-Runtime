# VNX Unity Runtime

The **Unity IL2CPP runtime layer** for [Viridite](https://github.com/AndroidHorizon/AndroidHorizonNX) — the piece that boots Unity games on a Nintendo Switch.

It lives in its own repo for browsability, but it isn't a standalone program: it's compiled straight into [VNX-Translation-Core](https://github.com/AndroidHorizon/VNX-Translation-Core) as a git submodule and calls back into the Core's ELF loader, JNI environment, and logging rather than duplicating any of it.

> **Status: very early.** This currently boots the Unity native libraries and runs their `JNI_OnLoad` handlers, then stops at the edge of what's implemented and reports cleanly. No Unity game runs yet. See [Milestones](#milestones).

## Why Unity needs a separate runtime

The Translation Core drives **cocos2d-x** games (like Hill Climb Racing) by finding an *exported* `Java_org_cocos2dx_..._nativeRender` symbol and calling it in a loop. That works because cocos2d-x exports its native entry points by name.

Unity exports almost nothing. `libunity.so`'s entire dynamic symbol table is essentially `JNI_OnLoad` + `UnitySendMessage`. Everything that matters is set up at **runtime**:

1. Java's `UnityPlayer` calls `NativeLoader.load("main")` → `dlopen("libmain.so")` → its `JNI_OnLoad` registers the `NativeLoader` helpers via `RegisterNatives`.
2. `NativeLoader.load("unity")` → `dlopen("libunity.so")` → its `JNI_OnLoad` registers **every** `com.unity3d.player.UnityPlayer` native (`nativeRender`, `nativePause`, `nativeResume`, `nativeInjectEvent`, …) via `RegisterNatives`.
3. `libunity` brings up the **IL2CPP** runtime in `libil2cpp.so` — `il2cpp_init` plus `global-metadata.dat` — and the game's C#-compiled-to-C++ logic runs from there.

None of that is reachable by scanning for exported symbols. So this module reproduces the boot **from native code**: it loads the libraries through the Core's real `dlopen`, runs the `JNI_OnLoad`s in the right order, and (eventually) stands in for the Java `UnityPlayer` and the GLES surface.

The reference title is **Brain It On!** (`com.orbital.brainiton`), Unity **2018.4.13f1**, arm64-v8a IL2CPP.

## How it plugs into the Core

- **Detection** lives in the Core (`launchApk`): if `libunity.so`/`libil2cpp.so` are present, it logs the Unity engine and (once wired) calls `unityRun` instead of the cocos2d-x loop.
- **Real `dlopen`/`dlsym`** also lives in the Core (`elfDlopen`/`elfFindLoaded`/`elfSetDlopenDir`) — reusable by any game that loads native deps at runtime, not just Unity.
- **This module** owns the Unity-specific bringup: `unityRun` in [`src/unity_runtime.cpp`](src/unity_runtime.cpp), interface in [`include/unity/unity_runtime.h`](include/unity/unity_runtime.h).

### Building

You don't build this repo directly. It's added as a submodule under the Core, whose `Makefile` compiles `src/` and adds `include/` to the include path, and whose release workflow checks it out with the Core. Everything targets devkitPro / libnx / the Switch aarch64 toolchain via the Core's build.

## Milestones

- [x] **M1** — Reverse-engineer the Unity native boot sequence (`libmain` → `libunity` → IL2CPP).
- [x] **M2** — Engine detection + real `dlopen`/`dlsym` in the Core.
- [x] **M3** — Load the Unity libs, run `JNI_OnLoad`, capture the registered `UnityPlayer` natives. *(current)*
- [ ] **M4** — Synthesize the `com.unity3d.player.UnityPlayer` Java class the native side calls back into.
- [ ] **M5** — Drive `il2cpp_init` + `global-metadata.dat`, stand up the GLES3 surface, and run the registered `nativeRender` loop. *(the hard part)*

Development is log-driven: each increment ships in a build, runs on real hardware, and the resulting `compat_log.txt` points at the next blocker.

## License

Same as the rest of Viridite — see the umbrella project.
