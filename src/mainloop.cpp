#include "mainloop.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cassert>
#include <iostream>
#include <fstream>
#include <emsocketctl.h>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

// libarchive
#include <archive.h>
#include <archive_entry.h>

#define MAX_WARNINGS 10

extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void emloop_set_pointerlock(int want);

    EMSCRIPTEN_KEEPALIVE
    void emloop_install_pack(const char *name, void *data, size_t size);

    EMSCRIPTEN_KEEPALIVE
    void emloop_invoke_main(int argc, char* argv[]);

    EMSCRIPTEN_KEEPALIVE
    void emloop_set_conf(const char *conf);
}

namespace emloop_private {
    pthread_t mainThreadId;
    int main_argc;
    char **main_argv;
    // For signaling that args are ready
    pthread_mutex_t main_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t main_cond = PTHREAD_COND_INITIALIZER;
};

using namespace emloop_private;

static std::atomic<bool> wantPointerLock{false};
static bool havePointerLock = false;

EM_BOOL report_pointerlockchange(int eventType, const EmscriptenPointerlockChangeEvent *pointerlockChangeEvent, void *userData) {
    bool isActive = pointerlockChangeEvent->isActive ? true : false;
    if (havePointerLock != isActive) {
      std::cout << "PointerLockChange: isActive=" << isActive << std::endl;
      havePointerLock = isActive;
    }
    return 0;
}

EM_BOOL report_pointerlockerror(int eventType, const void *reserved, void *userData) {
    std::cout << "PointerLockError!" << std::endl;
    return 0;
}

// This is called from main's worker thread.
void emloop_set_pointerlock(int want) {
    wantPointerLock.store(want);
    if (!want) {
        emscripten_exit_pointerlock();
    }
}

static EM_BOOL on_key(
    int event_type,
    const EmscriptenKeyboardEvent* event,
    void* user_data) {

    bool want = wantPointerLock.load();
    if (havePointerLock && !want) {
        emscripten_exit_pointerlock();
    } else if (!havePointerLock && want) {
        emscripten_request_pointerlock("#canvas", false);
    }
    return EM_FALSE;
}

static EM_BOOL on_mouse(
    int event_type,
    const EmscriptenMouseEvent* event,
    void* user_data) {

    bool want = wantPointerLock.load();
    if (havePointerLock && !want) {
        emscripten_exit_pointerlock();
    } else if (!havePointerLock && want) {
        emscripten_request_pointerlock("#canvas", false);
    }
    return EM_FALSE;
}

void emloop_init() {
    mainThreadId = pthread_self();
}

static std::string pathjoin(std::string a, std::string b) {
    if (a.size() > 0 && a[a.size() - 1] == '/') {
        return a + b;
    }
    return a + "/" + b;
}

static std::string spaces(size_t count)
{
    return std::string(count, ' ');
}

static void
debug_list_directory(std::string abspath, size_t depth)
{
    if (depth == 0) {
        std::cout << "Listing directory: " << abspath << std::endl;
    }
    struct dirent *ent;
    DIR *d = opendir(abspath.c_str());
    if (!d) {
        std::cout << "opendir(" << abspath << ") failed: " << strerror(errno) << std::endl;
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        struct stat st;
        std::string childpath = ent->d_name;
        if (childpath == "." || childpath == "..") continue;
        std::string abschildpath = pathjoin(abspath, childpath);
        if (lstat(abschildpath.c_str(), &st) == -1) {
            std::cout << "lstat failed on " << abschildpath << std::endl;
            continue;
        }
        std::cout << spaces(depth * 2 + 2) << childpath;
        if (S_ISDIR(st.st_mode)) {
            std::cout << std::endl;
            debug_list_directory(abschildpath, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            std::cout << " (" << st.st_size << " bytes)" << std::endl;
        } else {
            std::cout << " (unknown type)" << std::endl;
        }
    }
    closedir(d);
}

static int
copy_data(struct archive *ar, struct archive *aw)
{
    int r;
    const void *buff;
    size_t size;
    la_int64_t offset;

    for (;;) {
        r = archive_read_data_block(ar, &buff, &size, &offset);
        if (r == ARCHIVE_EOF)
            return ARCHIVE_OK;
        if (r < ARCHIVE_OK)
            return r;
        r = archive_write_data_block(aw, buff, size, offset);
        if (r < ARCHIVE_OK) {
            std::cout << "copy_data: " << archive_error_string(aw) << std::endl;
            return r;
        }
    }
}

// Adapted from https://github.com/libarchive/libarchive/wiki/Examples#a-complete-extractor
static bool extract_archive(struct archive *a)
{
    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_PERM);
    archive_write_disk_set_standard_lookup(ext);
    for (;;) {
        struct archive_entry *entry;
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_OK)
            std::cout << "emloop_install_pack: read next header: " << archive_error_string(a) << std::endl;
        if (r < ARCHIVE_WARN) {
            std::cout << "emloop_install_pack: Error while expanding pack" << std::endl;
            return false;
        }
        r = archive_write_header(ext, entry);
        if (r < ARCHIVE_OK)
            std::cout << "emloop_install_pack: write header: " << archive_error_string(a) << std::endl;
        else if (archive_entry_size(entry) > 0) {
            r = copy_data(a, ext);
            if (r < ARCHIVE_OK)
                std::cout << "emloop_install_pack: copy_data failed" << std::endl;
            if (r < ARCHIVE_WARN) {
                std::cout << "emloop_install_pack: copy_data fatal error" << std::endl;
                return false;
            }
        }
        r = archive_write_finish_entry(ext);
        if (r < ARCHIVE_OK)
            std::cout << "emloop_install_pack: archive_write_finish_entry: " << archive_error_string(ext) << std::endl;
        if (r < ARCHIVE_WARN) {
            std::cout << "emloop_install_pack: archive_write_finish_entry fatal error" << std::endl;
            return false;
        }
    }
    archive_write_close(ext);
    archive_write_free(ext);
    return true;
}

void emloop_install_pack(const char *name, void *data, size_t size) {
    struct archive *a = archive_read_new();
    if (archive_read_support_filter_zstd(a) != ARCHIVE_OK) {
        std::cout << "emloop_install_pack failed: zstd not supported" << std::endl;
        return;
    }
    if (archive_read_support_format_tar(a) != ARCHIVE_OK) {
        std::cout << "emloop_install_pack failed: tar not supported" << std::endl;
        return;
    }
    if (archive_read_open_memory(a, data, size) != ARCHIVE_OK) {
        std::cout << "emloop_install_pack failed: invalid archive" << std::endl;
        return;
    }
    if (extract_archive(a)) {
        std::cout << "emloop_install_pack: Installed " << name << " successfully" << std::endl;
    }
    archive_read_close(a);
    archive_read_free(a);
    //debug_list_directory("/", 0);
}

int main2(int argc, char *argv[]);

void emloop_set_conf(const char *contents) {
    std::ofstream os("/luanti/minetest.conf", std::ofstream::trunc);
    if (!os.good())
        return;
    os << contents;
    os.flush();
    os.close();
}

// This is called in the browser thread. main is called in a worker
void emloop_invoke_main(int argc, char* argv[]) {
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockchange);
    emscripten_set_pointerlockchange_callback("#canvas", 0, 1, report_pointerlockchange);
    emscripten_set_pointerlockerror_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockerror);
    emscripten_set_pointerlockerror_callback("#canvas", 0, 1, report_pointerlockerror);

    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, on_key);
    emscripten_set_mousedown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, on_mouse);

    pthread_mutex_lock(&main_mutex);
    main_argc = argc;
    main_argv = argv;
    pthread_cond_signal(&main_cond);
    pthread_mutex_unlock(&main_mutex);
}

int main(int argc, char *argv[])
{
    std::cout << "ENTERED main()" << std::endl;

    MAIN_THREAD_EM_ASM({
        emloop_ready();
    });

    pthread_mutex_lock(&main_mutex);
    while (main_argc == 0) {
        pthread_cond_wait(&main_cond, &main_mutex);
    }
    pthread_mutex_unlock(&main_mutex);

    std::cout << "Main received args:" << std::endl;
    for (int i = 0; i < main_argc; i++) {
        std::cout << "    " << main_argv[i] << std::endl;
    }
    return main2(main_argc, main_argv);
}
