#include "unity/unity_runtime.h"

// From AHNX-Translation-Core (this module is compiled into it as a submodule).
#include "compat/loader.h"
#include "compat/jni.h"   // JNIEnv, jobject, jboolean

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

    // ── 3. Look up the registered player entry points ────────────────────────
    // Unity registers these via RegisterNatives (not exported symbols), so we
    // fetch them from the Core's registered-natives table. Signatures were
    // recovered from libunity's method tables:
    //   initJni(JNIEnv*, jobject, jobject context)            (Landroid/content/Context;)V
    //   nativeRecreateGfxState(JNIEnv*, jobject, jint, jobject surface)  (ILandroid/view/Surface;)V
    //   nativeSendSurfaceChangedEvent(JNIEnv*, jobject)       ()V
    //   nativeRender(JNIEnv*, jobject) -> jboolean            ()Z
    typedef void     (*InitJni_fn)(void*, void*, void*);
    typedef jboolean (*Render_fn)(void*, void*);
    auto initJni      = (InitJni_fn)jniFindRegisteredNative("initJni");
    auto nativeRender = (Render_fn) jniFindRegisteredNative("nativeRender");
    void* il2cpp_init = libil2cpp->findSym("il2cpp_init");
    compatLogFmt("unity: entry points — initJni=%p nativeRender=%p il2cpp_init=%p",
                 (void*)initJni, (void*)nativeRender, il2cpp_init);

    if (!initJni) {
        r.ok = false;
        r.errorStage  = "Unity init entry (initJni not registered)";
        r.errorDetail = "libunity ran JNI_OnLoad but didn't register initJni — can't "
                        "start the player. See the RegisterNative lines in compat_log.txt.";
        compatLog("unity: FATAL — initJni not found among registered natives");
        return r;
    }

    // ── 4. Drive Unity's own init (M4/M5, in progress) ───────────────────────
    // initJni forwards to the real init, which brings up the IL2CPP runtime and
    // makes a flood of JNI callbacks into the Android Context/Activity we fake.
    // Every FindClass/GetMethodID/CallXMethod it makes is logged by the Core's
    // JNI env — so even when this faults, compat_log.txt becomes the exact list
    // of Java framework surface Unity needs next. jobject 0x4001 is the same
    // fake Activity the Core hands the cocos2d-x path.
    JNIEnv* env = (JNIEnv*)compatGet()->env_outer;
    void*   ctx = (void*)(uintptr_t)0x4001;   // fake Activity/Context
    compatLog("unity: calling initJni(env, activity, context) — Unity init begins");
    compatLogFlush();
    initJni(env, ctx, ctx);
    compatLog("unity: initJni returned (did not fault)");
    compatLogFlush();

    // If init survived, try one nativeRender to see how far the player loop gets
    // before we've stood up a real GLES surface (expected to need
    // nativeRecreateGfxState with a Surface first — this is purely diagnostic).
    if (nativeRender) {
        compatLog("unity: calling nativeRender() once (no surface yet — diagnostic)");
        compatLogFlush();
        jboolean cont = nativeRender(env, ctx);
        compatLogFmt("unity: nativeRender returned %d", (int)cont);
    }

    r.ok = false;
    r.errorStage  = "Unity player init (in progress)";
    r.errorDetail = "initJni was driven — see compat_log.txt for the JNI callbacks Unity "
                    "made and where it stopped. UnityPlayer/Activity Java shims and the "
                    "GLES3 surface are the next milestones.";
    compatLog("═══ AHNX Unity Runtime: initJni driven; see JNI callback trace above ═══");
    return r;
}
