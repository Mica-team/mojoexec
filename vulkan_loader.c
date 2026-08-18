//
// Created by maks on 10.04.2026.
//

#include <android/api-level.h>
#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdbool.h>
#include <jni.h>

#include <driver_helper/nsbypass.h>
#include <android/dlext.h>
#include <mojoexec.h>

static bool turnip_enabled = false;

#ifdef ENABLE_TURNIP_LOADER

static bool load_turnip_vulkan(void) {
    static bool driver_loaded = false;

    if (driver_loaded) {
        return true;
    }

    const char *cache_dir = getenv("TMPDIR");

    if (cache_dir == NULL || cache_dir[0] == '\0') {
        fprintf(stderr, "MojoExec: TMPDIR is not set\n");
        return false;
    }

    if (mojoexec_native_dir == NULL || mojoexec_native_dir[0] == '\0') {
        fprintf(stderr, "MojoExec: Native library directory is not set\n");
        return false;
    }

    /*
     * Load the launcher's native library directory into the Android
     * linker namespace so that the Turnip libraries can be resolved.
     */
    if (!linker_ns_load(mojoexec_native_dir)) {
        fprintf(stderr, "MojoExec: Failed to load native linker namespace\n");
        return false;
    }

    void *linkerhook = linker_ns_dlopen(
            "liblinkerhook.so",
            RTLD_LOCAL | RTLD_NOW
    );

    if (linkerhook == NULL) {
        fprintf(stderr,
                "MojoExec: Failed to load liblinkerhook.so: %s\n",
                dlerror());
        return false;
    }

    /*
     * Load the Mesa Turnip/Freedreno Vulkan driver.
     */
    void *turnip_driver_handle = linker_ns_dlopen(
            "libvulkan_freedreno.so",
            RTLD_LOCAL | RTLD_NOW
    );

    if (turnip_driver_handle == NULL) {
        fprintf(stderr,
                "MojoExec: Failed to load Turnip (libvulkan_freedreno.so): %s\n",
                dlerror());
        dlclose(linkerhook);
        return false;
    }

    /*
     * Android's linker namespace API.
     */
    void *dl_android = linker_ns_dlopen(
            "libdl_android.so",
            RTLD_LOCAL | RTLD_LAZY
    );

    if (dl_android == NULL) {
        fprintf(stderr,
                "MojoExec: Failed to load libdl_android.so: %s\n",
                dlerror());
        dlclose(turnip_driver_handle);
        dlclose(linkerhook);
        return false;
    }

    void *android_get_exported_namespace =
            dlsym(dl_android, "android_get_exported_namespace");

    void (*linkerhook_pass_handles)(void *, void *, void *) =
            dlsym(
                    linkerhook,
                    "app__pojav_linkerhook_pass_handles"
            );

    if (android_get_exported_namespace == NULL ||
        linkerhook_pass_handles == NULL) {

        fprintf(stderr,
                "MojoExec: Failed to resolve linkerhook symbols\n");

        dlclose(dl_android);
        dlclose(turnip_driver_handle);
        dlclose(linkerhook);
        return false;
    }

    /*
     * Give the linker hook the Turnip driver and Android linker
     * interfaces it needs.
     */
    linkerhook_pass_handles(
            turnip_driver_handle,
            (void *) android_dlopen_ext,
            android_get_exported_namespace
    );

    /*
     * Create/load our private Vulkan loader.
     *
     * LWJGL will ultimately receive this handle instead of the
     * system libvulkan.so.
     */
    void *libvulkan = linker_ns_dlopen_unique(
            cache_dir,
            "libvulkan.so",
            "libmjlvlk.so",
            RTLD_LOCAL | RTLD_NOW
    );

    printf(
            "MojoExec: Loaded mjlvlk, ptr=%p\n",
            libvulkan
    );

    if (libvulkan != NULL) {
        driver_loaded = true;

        /*
         * Keep the handles alive. The custom Vulkan loader depends
         * on the Turnip driver and linker hook remaining loaded.
         */
        dlclose(dl_android);

        return true;
    }

    fprintf(stderr,
            "MojoExec: Failed to load custom Vulkan loader\n");

    dlclose(dl_android);
    dlclose(turnip_driver_handle);
    dlclose(linkerhook);

    return false;
}

#endif // ENABLE_TURNIP_LOADER


void *mojoexec_acq_vulkan_handle(void) {
    const int flags = RTLD_LOCAL | RTLD_NOW;

#ifdef ENABLE_TURNIP_LOADER

    /*
     * The Turnip loader requires Android API 28 or newer.
     */
    if (android_get_device_api_level() >= 28) {

        if (turnip_enabled && load_turnip_vulkan()) {

            /*
             * Return a separate reference to the custom Vulkan
             * loader so that libraries calling dlclose() don't
             * unload the loader used by the launcher.
             */
            void *turnip_vulkan = linker_ns_dlopen(
                    "libmjlvlk.so",
                    flags
            );

            if (turnip_vulkan != NULL) {
                printf(
                        "MojoExec: Using Turnip Vulkan, ptr=%p\n",
                        turnip_vulkan
                );

                return turnip_vulkan;
            }

            fprintf(stderr,
                    "MojoExec: Failed to acquire libmjlvlk.so: %s\n",
                    dlerror());
        }
    }

#endif // ENABLE_TURNIP_LOADER

    /*
     * Fall back to Android's normal Vulkan loader.
     */
    void *vulkan_ptr = dlopen("libvulkan.so", flags);

    if (vulkan_ptr == NULL) {
        fprintf(stderr,
                "MojoExec: Failed to load system Vulkan: %s\n",
                dlerror());
    } else {
        printf(
                "MojoExec: Loaded system Vulkan, ptr=%p\n",
                vulkan_ptr
        );
    }

    return vulkan_ptr;
}


JNIEXPORT void JNICALL
Java_git_artdeell_mojoexec_MojoExec_setUseTurnip(
        JNIEnv *env,
        jclass clazz,
        jboolean enable
) {
    (void) env;
    (void) clazz;

    turnip_enabled = (enable == JNI_TRUE);

    printf(
            "MojoExec: Turnip %s\n",
            turnip_enabled ? "enabled" : "disabled"
    );
}


JNIEXPORT void JNICALL
Java_git_artdeell_mojoexec_MojoExec_preloadVulkan(
        JNIEnv *env,
        jclass clazz
) {
    (void) env;
    (void) clazz;

#ifdef ENABLE_TURNIP_LOADER

    if (!turnip_enabled) {
        return;
    }

    if (!load_turnip_vulkan()) {
        fprintf(stderr,
                "MojoExec: Failed to preload Turnip\n");
    }

#endif // ENABLE_TURNIP_LOADER
}
