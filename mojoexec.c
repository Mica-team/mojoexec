//
// Created by maks on 30.06.2026.
//

#include <mojoexec.h>
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <dlfcn.h>

#include <driver_helper/nsbypass.h>

mojoexec_renderspec_t mojoexec_renderspec;

const char* mojoexec_native_dir = NULL;
char* native_egl_path = NULL;
bool egl_use_bypass = false;

static void save_jvm_string(JNIEnv* env, char** target, jstring str) {
    if (target == NULL) {
        return;
    }

    if (*target != NULL) {
        free(*target);
        *target = NULL;
    }

    if (str == NULL) {
        return;
    }

    const char* path = (*env)->GetStringUTFChars(env, str, NULL);

    if (path == NULL) {
        return;
    }

    *target = strdup(path);

    (*env)->ReleaseStringUTFChars(env, str, path);
}

JNIEXPORT jboolean JNICALL
Java_git_artdeell_mojoexec_MojoExec_prepareEgl(
        JNIEnv* env,
        jclass clazz,
        jstring egl_path,
        jboolean use_bypass,
        jboolean use_gles,
        jint gles_version) {

    (void) clazz;

    /*
     * Store the EGL library path.
     */
    save_jvm_string(env, &native_egl_path, egl_path);

    if (native_egl_path == NULL) {
        fprintf(stderr,
                "MojoExec: EGL path is NULL\n");
        return JNI_FALSE;
    }

    /*
     * The namespace bypass requires a valid native library
     * directory.
     */
    if (use_bypass == JNI_TRUE) {

        if (mojoexec_native_dir == NULL ||
            mojoexec_native_dir[0] == '\0') {

            fprintf(stderr,
                    "MojoExec: Native library directory is not set\n");

            return JNI_FALSE;
        }

        if (!linker_ns_load(mojoexec_native_dir)) {
            fprintf(stderr,
                    "MojoExec: Failed to load native linker namespace\n");

            return JNI_FALSE;
        }
    }

    egl_use_bypass = (use_bypass == JNI_TRUE);

    /*
     * Load EGL immediately so that renderer initialization can
     * fail early instead of failing later inside Minecraft.
     */
    void* preload_handle = mojoexec_acq_egl_handle();

    if (preload_handle == NULL) {
        fprintf(stderr,
                "MojoExec: Failed to load EGL '%s': %s\n",
                native_egl_path,
                dlerror());

        return JNI_FALSE;
    }

    printf(
            "MojoExec: Loaded EGL %s (namespace bypass: %s)\n",
            native_egl_path,
            egl_use_bypass ? "yes" : "no"
    );

    /*
     * Configure the renderer specification.
     */
    mojoexec_renderspec.force_gles_context =
            (use_gles == JNI_TRUE);

    mojoexec_renderspec.override_major_version =
            (int) gles_version;

    return JNI_TRUE;
}


JNIEXPORT void JNICALL
Java_git_artdeell_mojoexec_MojoExec_setNativeLibraryDir(
        JNIEnv* env,
        jclass clazz,
        jstring dir) {

    (void) clazz;

    save_jvm_string(
            env,
            (char**) &mojoexec_native_dir,
            dir
    );
}


JNIEXPORT void JNICALL
Java_git_artdeell_mojoexec_MojoExec_setDisplayParams(
        JNIEnv* env,
        jclass clazz,
        jint width,
        jint height,
        jfloat hz) {

    (void) env;
    (void) clazz;

    /*
     * Store display parameters for the renderer.
     */
    mojoexec_renderspec.disp_width = width;
    mojoexec_renderspec.disp_height = height;
    mojoexec_renderspec.disp_hz = hz;
}


void* mojoexec_acq_egl_handle(void) {
    const int flags = RTLD_LOCAL | RTLD_NOW;

    if (native_egl_path == NULL ||
        native_egl_path[0] == '\0') {

        fprintf(stderr,
                "MojoExec: EGL path has not been configured\n");

        return NULL;
    }

    if (egl_use_bypass) {
        if (mojoexec_native_dir == NULL ||
            mojoexec_native_dir[0] == '\0') {

            fprintf(stderr,
                    "MojoExec: Cannot use EGL namespace bypass "
                    "without a native library directory\n");

            return NULL;
        }

        return linker_ns_dlopen(
                native_egl_path,
                flags
        );
    }

    return dlopen(
            native_egl_path,
            flags
    );
}
