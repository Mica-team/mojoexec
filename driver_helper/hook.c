//
// Created by maks on 05.06.2023.
//

#include <android/dlext.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// Silence the warnings about using reserved identifiers.
// We need these symbols for Android's linker interaction.
// NOLINTBEGIN

static void* (*android_dlopen_ext_p)(
        const char* filename,
        int flags,
        const android_dlextinfo* extinfo,
        const void* caller_addr);

static struct android_namespace_t* (*android_get_exported_namespace_p)(
        const char* name);

// NOLINTEND

static void* ready_handle = NULL;

static const char* sphal_namespaces[3] = {
        "sphal",
        "vendor",
        "default"
};

/**
 * Pass the handles obtained from Android's linker to this library.
 */
__attribute__((visibility("default"), used))
void app__pojav_linkerhook_pass_handles(
        void* data,
        void* android_dlopen_ext,
        void* android_get_exported_namespace) {

    ready_handle = data;

    android_dlopen_ext_p =
            (void* (*)(const char*, int, const android_dlextinfo*, const void*))
            android_dlopen_ext;

    android_get_exported_namespace_p =
            (struct android_namespace_t* (*)(const char*))
            android_get_exported_namespace;
}

/**
 * Intercept Vulkan library loading.
 *
 * For Vulkan libraries, return the already-loaded custom Vulkan driver.
 * Everything else is forwarded to Android's real android_dlopen_ext().
 */
__attribute__((visibility("default"), used))
void* android_dlopen_ext(
        const char* filename,
        int flags,
        const android_dlextinfo* extinfo) {

    if (filename == NULL || android_dlopen_ext_p == NULL) {
        return NULL;
    }

    /*
     * Only intercept Vulkan libraries.
     *
     * strstr() is used here because Android may request names such as:
     * libvulkan.so
     * libvulkan.so.1
     */
    if (strstr(filename, "vulkan.") == NULL) {
        return android_dlopen_ext_p(
                filename,
                flags,
                extinfo,
                (const void*) &android_dlopen_ext
        );
    }

    if (ready_handle == NULL) {
        return NULL;
    }

    return ready_handle;
}

/**
 * Load a library through Android's SP-HAL/vendor namespace.
 */
__attribute__((visibility("default"), used))
void* android_load_sphal_library(
        const char* filename,
        int flags) {

    if (filename == NULL ||
        android_dlopen_ext_p == NULL ||
        android_get_exported_namespace_p == NULL) {
        return NULL;
    }

    /*
     * Vulkan is supplied by our custom driver.
     */
    if (strstr(filename, "vulkan.") != NULL) {
        return ready_handle;
    }

    struct android_namespace_t* androidNamespace = NULL;

    /*
     * Try the available exported namespaces in order.
     */
    for (int i = 0; i < 3; i++) {
        androidNamespace =
                android_get_exported_namespace_p(sphal_namespaces[i]);

        if (androidNamespace != NULL) {
            break;
        }
    }

    /*
     * If none of the namespaces exist, fall back to the normal loader.
     */
    if (androidNamespace == NULL) {
        return android_dlopen_ext_p(
                filename,
                flags,
                NULL,
                (const void*) &android_load_sphal_library
        );
    }

    /*
     * Explicitly initialize the structure.
     * This is important because android_dlextinfo contains fields
     * other than flags and library_namespace.
     */
    android_dlextinfo info = {};

    info.flags = ANDROID_DLEXT_USE_NAMESPACE;
    info.library_namespace = androidNamespace;

    return android_dlopen_ext_p(
            filename,
            flags,
            &info,
            (const void*) &android_load_sphal_library
    );
}

/**
 * Compatibility implementation for older Android versions which
 * don't export atrace_get_enabled_tags().
 */
__attribute__((visibility("default"), used))
uint64_t atrace_get_enabled_tags(void) {
    return 0;
}
