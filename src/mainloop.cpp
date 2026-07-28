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
    void emloop_install_pack(const char *name, void *data, size_t size);

    EMSCRIPTEN_KEEPALIVE
    void emloop_reenter(void);

    EMSCRIPTEN_KEEPALIVE
    void emloop_reenter_blessed(void);

    EMSCRIPTEN_KEEPALIVE
    void emloop_invoke_main(int argc, char* argv[]);

    EMSCRIPTEN_KEEPALIVE
    void emloop_pause();

    EMSCRIPTEN_KEEPALIVE
    void emloop_unpause();

    EMSCRIPTEN_KEEPALIVE
    void emloop_init_sound();

    EMSCRIPTEN_KEEPALIVE
    void emloop_set_conf(const char *conf);

    EM_BOOL irrlicht_want_pointerlock(void);
    void irrlicht_force_pointerlock(void);
}

namespace emloop_private {
    std::function<void()> next_callback;
    bool blessed = false;
    bool paused = false;
    bool invokedMain = false;
    bool busy = false; // executing main
    bool dead = false; // Main exited, or got uncaught exception
    int warnCount = 0;
    pthread_t mainThreadId;

    pthread_t helperThread;
    std::mutex helperMutex;
    std::condition_variable helperCond;
    AsyncPayload helperTask;
};

using namespace emloop_private;


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

void* helper_thread_main(void*) {
    std::unique_lock<std::mutex> ul(helperMutex);
    for (;;) {
        while (!helperTask) {
            helperCond.wait(ul);
        }
        AsyncPayload task = std::move(helperTask);
        helperTask = nullptr;
        MainLoop::NextFrame(task());
    }
}

void emloop_init() {
    mainThreadId = pthread_self();
    // Launch async helper thread, used instead of the main thread during blocking network tasks.
    int rc = pthread_create(&helperThread, NULL, helper_thread_main, NULL);
    if (rc != 0) {
        std::cerr << "MainLoop: Failed to launch helper thread" << std::endl;
        abort();
    }
}

void MainLoop::RunAsyncThenResume(AsyncPayload payload) {
    {
        std::lock_guard<std::mutex> lock(helperMutex);
        assert(!helperTask);
        helperTask = payload;
    }
    helperCond.notify_all();
}

void MainLoop::NextFrame(std::function<void()> resolve) {
    assert(!next_callback);
    next_callback = resolve;
}

void MainLoop::DelayNextFrameUntilRedraw() {
    EM_ASM({
        emloop_request_animation_frame();
    });
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

void emloop_reenter_blessed(void) {
    // If we're already pointerlocked, there's no need to expedite main re-entry,
    // and it may be better for performance to not do so. (prevents WebGL stalls)
    if (havePointerLock) {
        return;
    }
    blessed = true;
    emloop_reenter();
    blessed = false;
}

void emloop_reenter(void) {
    if (dead || !invokedMain || paused) {
        return;
    }
    if (busy) {
        if (warnCount < MAX_WARNINGS) {
            std::cout << "[WARNING] MainLoop attempted reentry" << std::endl;
            warnCount++;
        }
        return;
    }
    if (!pthread_equal(pthread_self(), mainThreadId)) {
        if (warnCount < MAX_WARNINGS) {
            std::cout << "[WARNING] MainLoop reentry attempted off main thread" << std::endl;
            warnCount++;
        }
        return;
    }
    busy = true;
    try {
        if (next_callback) {
            std::function<void()> callback = std::move(next_callback);
            next_callback = nullptr;
            callback();

            // Adjust pointer lock
            bool wantPointerLock = irrlicht_want_pointerlock();
            if (blessed && !havePointerLock && wantPointerLock) {
                emscripten_request_pointerlock("#canvas", EM_FALSE);
            } else if (havePointerLock && !wantPointerLock) {
                emscripten_exit_pointerlock();
            }
        }
    } catch (const std::exception &exc) {
        std::cout << "Unhandled exception: " << exc.what() << std::endl;
        busy = false;
        dead = true;
        throw;
    } catch (...) {
        std::cout << "Unhandled exception (non-standard)" << std::endl;
        busy = false;
        dead = true;
        throw;
    }
    busy = false;
}

void main_resolve(int rv) {
    std::cout << "main() exited with return value " << rv << std::endl;
}

void main2(int argc, char *argv[], std::function<void(int)> resolve);

void emloop_pause() {
    paused = true;
}

void emloop_unpause() {
    paused = false;
}

extern "C" {
    extern void preinit_sound(void);
}

void emloop_init_sound() {
    preinit_sound();
}

void emloop_set_conf(const char *contents) {
    std::ofstream os("/minetest/minetest.conf", std::ofstream::trunc);
    if (!os.good())
        return;
    os << contents;
    os.flush();
    os.close();
}

void emloop_invoke_main(int argc, char* argv[]) {
    // Caller must guarantee that argv remains valid forever.
    if (invokedMain) {
        std::cout << "ERROR: emloop_invoke_main called more than once" << std::endl;
        return;
    }
    invokedMain = true;
    MainLoop::NextFrame([argc, argv]() {
        main2(argc, argv, main_resolve);
    });
    blessed = true;
    emloop_reenter();
    blessed = false;
}

int main(int argc, char *argv[])
{
    std::cout << "ENTERED main()" << std::endl;
    emloop_init();

    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockchange);
    emscripten_set_pointerlockchange_callback("#canvas", 0, 1, report_pointerlockchange);
    emscripten_set_pointerlockerror_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockerror);
    emscripten_set_pointerlockerror_callback("#canvas", 0, 1, report_pointerlockerror);

    // The first next_frame() is special. It will be called directly from a user-generated click,
    // which allows sound to be initialized. (and maybe full screen mode)
    std::cout << "Main thread waiting for play signal (click on canvas to activate)" << std::endl;
    EM_ASM({
        emloop_ready();
    });
    emscripten_set_main_loop(emloop_reenter, 0, 1);
    std::cout << "THIS SHOULD BE UNREACHABLE" << std::endl;
    return 0;
}

