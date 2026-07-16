#include "unity/unity_runtime.h"

// From AHNX-Translation-Core (this module is compiled into it as a submodule).
#include "compat/loader.h"

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

// ── The Unity boot sequence we're reproducing ──────────────────────────────────
// On a real device, Java does this: UnityPlayer's constructor calls
// NativeLoader.load("main") (dlopen libmain.so, run its JNI_OnLoad, which
// RegisterNatives-registers NativeLoader.load/unload), then NativeLoader.load
// ("unity") which dlopen's libunity.so and runs ITS JNI_OnLoad — that one
// RegisterNatives-registers every com.unity3d.player.UnityPlayer native
// (nativeRender, nativePause, nativeResume, nativeInjectEvent, …). libunity in
// turn brings up the IL2CPP runtime in libil2cpp.so (il2cpp_init +
// global-metadata.dat) and the game's C# is off to the races.
//
// We have no Java UnityPlayer, so we do the equivalent from native code: load
// the libs through the Core's real dlopen and call the JNI_OnLoads ourselves,
// in order. Each RegisterNatives call is already logged by the Core's JNI env,
// so the resulting compat_log.txt shows exactly which Unity natives registered
// and where (if anywhere) the boot faults — the same log-driven loop we used to
// bring up cocos2d-x.

namespace {

typedef int (*JniOnLoad_fn)(void* /*JavaVM*/, void* /*reserved*/);

// Call a loaded library's JNI_OnLoad(vm, nullptr), if it exports one. Returns
// the reported JNI version, or -1 if there's no JNI_OnLoad, or -2 if the call
// was skipped. Any fault inside is caught and logged by the Core's process-wide
// exception handler, so a bad JNI_OnLoad shows up in the log rather than
// silently killing the boot.
int runJniOnLoad(LoadedSo* so, const char* label, void* vm) {
    if (!so) { compatLogFmt("unity: %s not loaded — can't run JNI_OnLoad", label); return -2; }
    auto fn = (JniOnLoad_fn)so->findSym("JNI_OnLoad");
    if (!fn) { compatLogFmt("unity: %s exports no JNI_OnLoad", label); return -1; }
    compatLogFmt("unity: calling %s JNI_OnLoad @%p ...", label, (void*)fn);
    compatLogFlush();
    int ver = fn(vm, nullptr);
    compatLogFmt("unity: %s JNI_OnLoad returned 0x%X", label, (unsigned)ver);
    return ver;
}

} // namespace

bool unityIsGame(const std::string& lib_dir) {
    struct stat st;
    return stat((lib_dir + "/libunity.so").c_str(),  &st) == 0
        || stat((lib_dir + "/libil2cpp.so").c_str(), &st) == 0;
}

UnityLaunchResult unityRun(const std::string& lib_dir, const std::string& apk_path,
                           const std::string& data_path, const std::string& pkg_name) {
    UnityLaunchResult r;
    r.handled = true;  // we own this launch from here

    compatLog("═══ AHNX Unity Runtime: bringup start ═══");
    compatLogFmt("unity: pkg=%s lib_dir=%s", pkg_name.c_str(), lib_dir.c_str());
    (void)apk_path; (void)data_path;

    void* vm = compatGet()->vm_outer;

    // ── 1. Ensure the Unity libs are loaded (dedups with the Core's initial
    //       batch; libgpg is optional Google Play Games). ────────────────────
    LoadedSo* libmain   = elfDlopen("libmain.so");
    LoadedSo* libunity  = elfDlopen("libunity.so");
    LoadedSo* libil2cpp = elfDlopen("libil2cpp.so");
    elfDlopen("libgpg.so");  // optional; ignore result
    compatLogFmt("unity: libs — main=%p unity=%p il2cpp=%p",
                 (void*)libmain, (void*)libunity, (void*)libil2cpp);

    if (!libunity || !libil2cpp) {
        r.ok = false;
        r.errorStage  = "Loading Unity libraries";
        r.errorDetail = "libunity.so / libil2cpp.so failed to load — see compat_log.txt.";
        compatLog("unity: FATAL — core Unity libs missing, aborting bringup");
        return r;
    }

    // ── 2. Boot the native side in Java's order: libmain first (registers the
    //       NativeLoader helpers), then libunity (registers every UnityPlayer
    //       native — watch the RegisterNative lines in the log). ─────────────
    runJniOnLoad(libmain,  "libmain.so",  vm);
    runJniOnLoad(libunity, "libunity.so", vm);

    // ── 3. Probe what we now have to work with, so the log tells us the exact
    //       state before the parts that aren't built yet. ─────────────────────
    void* il2cpp_init = libil2cpp->findSym("il2cpp_init");
    void* nativeRender_export = libunity->findSym(
        "Java_com_unity3d_player_UnityPlayer_nativeRender");
    compatLogFmt("unity: probes — il2cpp_init=%p unity.nativeRender(exported)=%p",
                 il2cpp_init, nativeRender_export);
    compatLog("unity: NOTE nativeRender is normally NOT exported — it's registered "
              "via RegisterNatives above; a null here is expected.");

    // ── 4. Stop honestly at the edge of what's implemented. ──────────────────
    // Next milestones (see README): synthesize the com.unity3d.player.UnityPlayer
    // Java class so its constructor path can run, drive UnityPlayer.init +
    // il2cpp_init(+global-metadata.dat), stand up the GLES3 surface, then call
    // the registered nativeRender in a loop.
    r.ok = false;
    r.errorStage  = "Unity IL2CPP init (not yet implemented)";
    r.errorDetail = "Unity libraries loaded and JNI_OnLoad ran — see compat_log.txt "
                    "for the registered UnityPlayer natives. IL2CPP runtime init, the "
                    "UnityPlayer Java shims, and the GLES render loop are the next "
                    "milestones and aren't built yet.";
    compatLog("═══ AHNX Unity Runtime: bringup reached JNI_OnLoad; IL2CPP init pending ═══");
    return r;
}
