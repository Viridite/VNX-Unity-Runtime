#pragma once
#include <string>

// ── AHNX Unity Runtime ─────────────────────────────────────────────────────────
// The Unity IL2CPP bringup for Android Horizon NX. Kept in its own repo (this
// one) for browsability, but compiled straight into AHNX-Translation-Core as a
// submodule — it calls back into the Core's ELF loader, JNI env, and logging
// (compat/loader.h) rather than duplicating any of it.
//
// Why Unity needs its own path at all: the Core drives cocos2d-x games by
// finding an *exported* Java_...nativeRender symbol and calling it in a loop.
// Unity exports almost nothing — libunity.so's dynamic table is just
// JNI_OnLoad + UnitySendMessage. Everything real (the player loop, input,
// lifecycle) is registered at runtime via JNI_OnLoad → RegisterNatives on the
// com.unity3d.player.UnityPlayer class, and the game logic is IL2CPP-compiled
// C# in libil2cpp.so driven off global-metadata.dat. So this module boots Unity
// the way the Java UnityPlayer would, but entirely from native code.

struct UnityLaunchResult {
    // true  → this module took over the launch (whether it ultimately succeeded
    //         or failed) and the caller should NOT fall back to the cocos2d-x
    //         path. false → not a Unity game / not handled.
    bool        handled     = false;
    bool        ok          = false;   // reached a running player loop
    const char* errorStage  = nullptr; // short stage label if !ok
    std::string errorDetail;           // human-readable detail if !ok
};

// True if the on-disk library set looks like a Unity IL2CPP game (libunity.so
// and/or libil2cpp.so present under lib_dir). Cheap filename check; the Core
// already does the same detection to decide whether to call unityRun.
bool unityIsGame(const std::string& lib_dir);

// Boot a Unity game. lib_dir is the extracted lib/ folder, apk_path the source
// APK, data_path the game's writable data dir, pkg_name its package id. Assumes
// the Core has already: set up the JNI env (jniSetup), loaded the initial .so
// batch, and pointed elfSetDlopenDir at lib_dir. Runs on the main thread with
// SDL2's GL context current, same as the cocos2d-x loop.
UnityLaunchResult unityRun(const std::string& lib_dir, const std::string& apk_path,
                           const std::string& data_path, const std::string& pkg_name);
