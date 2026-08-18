//
// Created by maks on 11.05.2026.
//

#include "android_namespace_func.h"
#include "fake_dlfcn.h"
#include <elf_defs.h>
#include <dlfcn.h>

#define TAG __FILE_NAME__
#include <log.h>

typedef void* (*dlsym_impl)(void* handle, const char* proc);

static bool load_symbols(dlsym_impl idlsym, void* handle, android_ldfuncs_t* funcs) {
    if (idlsym == NULL || handle == NULL || funcs == NULL) {
        LOGE("load_symbols: invalid arguments");
        return false;
    }

    funcs->create_namespace = idlsym(handle, "android_create_namespace");
    funcs->link_namespaces = idlsym(handle, "android_link_namespaces");

    bool res = funcs->create_namespace && funcs->link_namespaces;
    LOGI("load_symbols res: %i", res);

    if (!res) {
        funcs->create_namespace = NULL;
        funcs->link_namespaces = NULL;
    }

    return res;
}

static void* fakel_locate_libdl_android() {
    // u mad? :trollface:
#ifdef PLATFORM_64
    void* linker_handle = fake_dlopen("linker64", 0);
#else
    void* linker_handle = fake_dlopen("linker", 0);
#endif

    if (!linker_handle) {
        LOGE("fakel failed to find linker in VA space");
        return NULL;
    }

    loader_dlopen_t loader_dlopen =
            fake_dlsym(linker_handle, "__loader_dlopen");

    if (!loader_dlopen) {
        LOGE("fakel failed to find loader_dlopen entrypoint");
        fake_dlclose(linker_handle);
        return NULL;
    }

    void* dl_android =
            loader_dlopen("libdl_android.so", RTLD_NOW, &dlopen);

    if (!dl_android) {
        LOGE("fakel failed to load libdl_android: %s", dlerror());
        fake_dlclose(linker_handle);
        return NULL;
    }

    fake_dlclose(linker_handle);
    return dl_android;
}

#ifdef USE_ARM64_LOCATOR
extern void* arm64l_locate_libdl_android();
#endif

bool locate_namespace_funcs(android_ldfuncs_t* funcs) {
    if (funcs == NULL) {
        LOGE("locate_namespace_funcs: funcs is NULL");
        return false;
    }

    funcs->dl_handle = NULL;
    funcs->close = NULL;
    funcs->create_namespace = NULL;
    funcs->link_namespaces = NULL;

#ifdef USE_ARM64_LOCATOR
    // Path 1: attempt to load libdl_android using the ARM64
    // loader workaround.
    void* handle = arm64l_locate_libdl_android();

    if (handle) {
        if (load_symbols(dlsym, handle, funcs)) {
            funcs->dl_handle = handle;
            funcs->close = dlclose;
            return true;
        }

        LOGE("ARM64 locator found libdl_android but required symbols are missing");
        dlclose(handle);
    }
#endif

    // Path 2: attempt to load libdl_android using the internal
    // linker entrypoint.
    void* handle = fakel_locate_libdl_android();

    if (handle) {
        if (load_symbols(dlsym, handle, funcs)) {
            funcs->dl_handle = handle;
            funcs->close = dlclose;
            return true;
        }

        LOGE("Fake linker locator found libdl_android but required symbols are missing");
        dlclose(handle);
    }

    // Path 3: attempt to locate an already-loaded libdl_android
    // directly using the fake ELF loader.
    handle = fake_dlopen("libdl_android.so", 0);

    if (handle) {
        if (load_symbols(fake_dlsym, handle, funcs)) {
            funcs->dl_handle = handle;
            funcs->close = fake_dlclose;
            return true;
        }

        LOGE("Fake dlopen found libdl_android but required symbols are missing");
        fake_dlclose(handle);
    }

    LOGE("Failed to locate Android namespace functions");
    return false;
}
