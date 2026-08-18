//
// Created by maks on 05.06.2023.
//

#include "nsbypass.h"
#include "android_namespace_func.h"

#include <dlfcn.h>
#include <android/dlext.h>
#include <android/log.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <string.h>
#include <stdio.h>
#include <linux/limits.h>
#include <errno.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <elf.h>
#include <elf_defs.h>
#include <inttypes.h>

#define TAG __FILE_NAME__
#include <log.h>

/* Library search path */
#ifdef PLATFORM_64
#define SEARCH_PATH "/system/lib64"
#else
#define SEARCH_PATH "/system/lib"
#endif

static struct android_namespace_t* driver_namespace = NULL;

bool linker_ns_load(const char* lib_search_path) {
    if (driver_namespace != NULL)
        return true;

    if (lib_search_path == NULL || lib_search_path[0] == '\0') {
        LOGE("linker_ns_load: invalid library search path");
        return false;
    }

    android_ldfuncs_t ldfuncs = {0};

    if (!locate_namespace_funcs(&ldfuncs)) {
        LOGE("Failed to locate Android namespace functions");
        return false;
    }

    /*
     * Assemble the full library search path.
     */
    size_t full_path_len =
            strlen(SEARCH_PATH) +
            strlen(lib_search_path) +
            2;

    char* full_path = malloc(full_path_len);

    if (full_path == NULL) {
        LOGE("Failed to allocate namespace search path");
        if (ldfuncs.close && ldfuncs.dl_handle)
            ldfuncs.close(ldfuncs.dl_handle);
        return false;
    }

    snprintf(
            full_path,
            full_path_len,
            "%s:%s",
            SEARCH_PATH,
            lib_search_path
    );

    driver_namespace = ldfuncs.create_namespace(
            "pojav-driver",
            full_path,
            full_path,
            3 /* TYPE_SHARED | TYPE_ISOLATED */,
            "/system/:/system_ext/:/data/:/vendor/:/apex/",
            NULL
    );

    free(full_path);

    if (driver_namespace == NULL) {
        LOGE("Failed to create driver namespace");

        if (ldfuncs.close && ldfuncs.dl_handle)
            ldfuncs.close(ldfuncs.dl_handle);

        return false;
    }

    /*
     * Link the driver namespace with the global namespace.
     *
     * This is required for Android's internal __loader symbol
     * resolution on a number of Android versions.
     */
    if (ldfuncs.link_namespaces(
            driver_namespace,
            NULL,
            "ld-android.so") == NULL) {

        LOGE("Failed to link ld-android.so namespace");
    }

    /*
     * Allow libnativeloader to resolve from the global namespace.
     * This works around issues seen on some EMUI devices.
     */
    if (ldfuncs.link_namespaces(
            driver_namespace,
            NULL,
            "libnativeloader.so") == NULL) {

        LOGE("Failed to link libnativeloader.so namespace");
    }

    if (ldfuncs.link_namespaces(
            driver_namespace,
            NULL,
            "libnativeloader_lazy.so") == NULL) {

        LOGE("Failed to link libnativeloader_lazy.so namespace");
    }

    if (ldfuncs.close && ldfuncs.dl_handle)
        ldfuncs.close(ldfuncs.dl_handle);

    return true;
}

void* linker_ns_dlopen(const char* name, int flag) {
    if (name == NULL || driver_namespace == NULL) {
        LOGE("linker_ns_dlopen: namespace or name is NULL");
        return NULL;
    }

    android_dlextinfo dlextinfo = {0};

    dlextinfo.flags = ANDROID_DLEXT_USE_NAMESPACE;
    dlextinfo.library_namespace = driver_namespace;

    return android_dlopen_ext(
            name,
            flag,
            &dlextinfo
    );
}

bool patch_elf_soname(
        int patchfd,
        int realfd,
        size_t size,
        const char* patchname) {

    if (patchfd < 0 || realfd < 0 ||
        size == 0 || patchname == NULL) {
        return false;
    }

    char* target = mmap(
            NULL,
            size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            patchfd,
            0
    );

    /*
     * mmap() returns MAP_FAILED, not NULL, on failure.
     */
    if (target == MAP_FAILED)
        return false;

    ssize_t bytes_read = read(realfd, target, size);

    if (bytes_read != (ssize_t) size)
        goto fail;

    ELF_EHDR* ehdr = (ELF_EHDR*) target;

    /*
     * Basic ELF validation.
     */
    if (ehdr->e_shoff == 0 ||
        ehdr->e_shnum == 0 ||
        ehdr->e_shentsize < sizeof(ELF_SHDR)) {
        goto fail;
    }

    ELF_SHDR* shdr =
            (ELF_SHDR*) (target + ehdr->e_shoff);

    for (ELF_HALF i = 0; i < ehdr->e_shnum; i++) {

        ELF_SHDR* hdr = &shdr[i];

        if (hdr->sh_type != SHT_DYNAMIC)
            continue;

        if (hdr->sh_link >= ehdr->e_shnum)
            goto fail;

        if (hdr->sh_entsize == 0)
            goto fail;

        char* strtab =
                target + shdr[hdr->sh_link].sh_offset;

        ELF_DYN* dynEntries =
                (ELF_DYN*) (target + hdr->sh_offset);

        ELF_XWORD entry_count =
                hdr->sh_size / hdr->sh_entsize;

        for (ELF_XWORD k = 0; k < entry_count; k++) {

            ELF_DYN* dynEntry = &dynEntries[k];

            if (dynEntry->d_tag != DT_SONAME)
                continue;

            char* soname =
                    strtab + dynEntry->d_un.d_val;

            size_t soname_len = strlen(soname);
            size_t patchname_len = strlen(patchname);

            /*
             * Keep the replacement the same length so we don't
             * corrupt the ELF string table.
             */
            if (patchname_len != soname_len)
                goto fail;

            memcpy(
                    soname,
                    patchname,
                    patchname_len + 1
            );

            munmap(target, size);
            return true;
        }
    }

fail:
    munmap(target, size);
    return false;
}

#define PAGE_ALIGN(addr) \
    (((addr) + pagesize - 1) & ~(pagesize - 1))

void* linker_ns_dlopen_unique(
        const char* tmpdir,
        const char* name,
        const char* patch_name,
        int flags) {

    if (tmpdir == NULL ||
        name == NULL ||
        patch_name == NULL ||
        driver_namespace == NULL) {

        LOGE("linker_ns_dlopen_unique: invalid arguments");
        return NULL;
    }

    int pagesize = getpagesize();

    if (pagesize <= 0)
        return NULL;

    char pathbuf[PATH_MAX];

    static uint16_t patchid = 0;

    int patch_fd = -1;
    int real_fd = -1;

    size_t fsize;
    size_t totalsize;

    /*
     * Locate the original system library.
     */
    int path_len = snprintf(
            pathbuf,
            sizeof(pathbuf),
            "%s/%s",
            SEARCH_PATH,
            name
    );

    if (path_len < 0 || path_len >= (int) sizeof(pathbuf)) {
        LOGE("Library path is too long");
        return NULL;
    }

    real_fd = open(pathbuf, O_RDONLY);

    if (real_fd == -1) {
        LOGE(
                "Failed to open %s: %s",
                pathbuf,
                strerror(errno)
        );
        return NULL;
    }

    /*
     * Get the library size.
     */
    struct stat64 real_stat;

    if (fstat64(real_fd, &real_stat) != 0)
        goto fail_real;

    if (real_stat.st_size <= 0)
        goto fail_real;

    fsize = (size_t) real_stat.st_size;

    totalsize =
            (fsize + (size_t) pagesize - 1) &
            ~((size_t) pagesize - 1);

    /*
     * Create an anonymous file for the patched ELF.
     */
    patch_fd = (int) syscall(
            __NR_memfd_create,
            patch_name,
            MFD_CLOEXEC
    );

    if (patch_fd == -1) {

        /*
         * Fallback for Android versions without memfd_create.
         *
         * The caller provides TMPDIR for this purpose.
         */
        int tmp_len = snprintf(
                pathbuf,
                sizeof(pathbuf),
                "%s/%" PRIu16,
                tmpdir,
                patchid++
        );

        if (tmp_len < 0 || tmp_len >= (int) sizeof(pathbuf))
            goto fail_real;

        patch_fd = open(
                pathbuf,
                O_CREAT | O_RDWR,
                S_IRUSR | S_IWUSR
        );

        if (patch_fd == -1)
            goto fail_real;

        /*
         * The fallback file should be removed after opening.
         * The descriptor remains usable.
         */
        unlink(pathbuf);
    }

    if (ftruncate64(
            patch_fd,
            (off64_t) totalsize) == -1) {
        goto fail_both;
    }

    /*
     * Copy the original ELF and replace its SONAME.
     */
    bool patch_result =
            patch_elf_soname(
                    patch_fd,
                    real_fd,
                    fsize,
                    patch_name
            );

    close(real_fd);
    real_fd = -1;

    if (!patch_result) {
        close(patch_fd);
        return NULL;
    }

    /*
     * Load the patched ELF into our driver namespace.
     */
    android_dlextinfo extinfo = {0};

    extinfo.flags =
            ANDROID_DLEXT_USE_NAMESPACE |
            ANDROID_DLEXT_USE_LIBRARY_FD;

    extinfo.library_fd = patch_fd;
    extinfo.library_namespace = driver_namespace;

    void* handle = android_dlopen_ext(
            patch_name,
            flags,
            &extinfo
    );

    /*
     * The linker has its own reference after android_dlopen_ext().
     * We can close our descriptor.
     */
    close(patch_fd);

    return handle;

fail_both:
    close(patch_fd);

fail_real:
    close(real_fd);

    return NULL;
}
