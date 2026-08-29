#include "mainloop.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/wasmfs.h>
#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <emsocketctl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

// libarchive
#include <archive.h>
#include <archive_entry.h>

// Root of the Luanti tree. RUN_IN_PLACE derives both path_share and path_user
// from it (see getCurrentExecPath in porting.cpp), so everything the game reads
// or writes lives below this point.
//
// When persistent storage is enabled, this is where OPFS is mounted, which
// makes worlds, minetest.conf, the cache and any content installed from
// ContentDB survive a page reload. Otherwise it is an ordinary in-memory
// directory and everything is lost when the tab closes.
//
// The OPFS backend is rooted at the OPFS directory of the same name rather
// than at the OPFS root (see emsdk_wasmfs_opfs_subdir.patch), so paths in
// persistent storage are the paths below.
#define LUANTI_ROOT "/luanti"

// Bookkeeping for installed packs. Inside LUANTI_ROOT so that it persists
// alongside the files it describes.
#define PACK_DB_DIR LUANTI_ROOT "/.packs"

extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void emloop_set_pointerlock(int want);

    // Choose the storage backend. Must be called (exactly once) before any
    // pack is installed. Answers asynchronously with emloop_fs_ready().
    EMSCRIPTEN_KEEPALIVE
    void emloop_init_fs(int want_opfs);

    // Takes ownership of `data`, which must come from malloc().
    // Answers asynchronously with emloop_pack_installed().
    EMSCRIPTEN_KEEPALIVE
    void emloop_install_pack(const char *name, const char *version, void *data, size_t size);

    EMSCRIPTEN_KEEPALIVE
    void emloop_invoke_main(int argc, char* argv[]);

    EMSCRIPTEN_KEEPALIVE
    void emloop_set_conf(const char *conf);
}

namespace emloop_private {
    pthread_t mainThreadId;

    // Filesystem work is done on main's worker thread rather than in the
    // browser thread that calls the emloop_* entry points. OPFS requires it:
    // WasmFS proxies every operation to a dedicated worker and blocks until it
    // answers, which the browser thread must stay free to relay.
    std::mutex queue_mutex;
    std::condition_variable queue_cond;
    std::deque<std::function<void()>> queue;

    bool main_requested = false;
    int main_argc = 0;
    char **main_argv = nullptr;

    // Whether LUANTI_ROOT is backed by OPFS.
    bool persistent = false;

    void post(std::function<void()> work) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            queue.push_back(std::move(work));
        }
        queue_cond.notify_one();
    }
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

    bool isEscape = (strcmp(event->code, "Escape") == 0);

    bool want = wantPointerLock.load();
    if (havePointerLock && !want) {
        emscripten_exit_pointerlock();
    } else if (!havePointerLock && want && !isEscape) {
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

/////////////////////////////////////////////////////////////////////////////
// Small filesystem helpers
/////////////////////////////////////////////////////////////////////////////

static bool readWholeFile(const std::string &path, std::string &out) {
    std::ifstream is(path, std::ifstream::binary);
    if (!is)
        return false;
    std::ostringstream ss;
    ss << is.rdbuf();
    out = ss.str();
    return true;
}

static bool writeWholeFile(const std::string &path, const std::string &data) {
    std::ofstream os(path, std::ofstream::binary | std::ofstream::trunc);
    if (!os)
        return false;
    os.write(data.data(), data.size());
    os.close();
    return os.good();
}

static bool makeDirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++) {
        if (i != path.size() && path[i] != '/')
            continue;
        std::string sub = path.substr(0, i);
        if (mkdir(sub.c_str(), 0777) != 0 && errno != EEXIST)
            return false;
    }
    return true;
}

// Turn an archive entry path ("./luanti/builtin/init.lua") into the absolute
// path it is extracted to. The extractor runs with the working directory set
// to "/" (see do_init_fs).
static std::string entryAbsolutePath(const std::string &p) {
    std::string r = p;
    while (r.size() >= 2 && r[0] == '.' && r[1] == '/')
        r.erase(0, 2);
    if (r.empty() || r[0] != '/')
        r = "/" + r;
    while (r.size() > 1 && r.back() == '/')
        r.pop_back();
    return r;
}

static bool insideLuantiRoot(const std::string &abspath) {
    static const std::string prefix = LUANTI_ROOT "/";
    return abspath.compare(0, prefix.size(), prefix) == 0;
}

/////////////////////////////////////////////////////////////////////////////
// Storage backend
/////////////////////////////////////////////////////////////////////////////

// OPFS is only usable if the browser exposes createSyncAccessHandle, which is
// only available to workers. This runs in main's worker, so the check is
// meaningful; the launcher has already verified from the page that the origin
// private file system can actually be opened.
static bool opfsUsableHere() {
    return EM_ASM_INT({
        try {
            return (typeof FileSystemFileHandle !== 'undefined' &&
                    typeof FileSystemFileHandle.prototype.createSyncAccessHandle === 'function' &&
                    typeof navigator !== 'undefined' &&
                    navigator.storage &&
                    typeof navigator.storage.getDirectory === 'function') ? 1 : 0;
        } catch (e) {
            return 0;
        }
    }) != 0;
}

static void do_init_fs(bool want_opfs) {
    // Packs are extracted relative to the working directory.
    chdir("/");

    if (want_opfs) {
        if (!opfsUsableHere()) {
            std::cout << "emloop_init_fs: OPFS is not available to this worker" << std::endl;
        } else {
            backend_t backend = wasmfs_create_opfs_backend();
            if (!backend) {
                std::cout << "emloop_init_fs: could not create the OPFS backend" << std::endl;
            } else if (wasmfs_create_directory(LUANTI_ROOT, 0777, backend) != 0) {
                std::cout << "emloop_init_fs: could not mount OPFS at " LUANTI_ROOT << std::endl;
            } else {
                persistent = true;
            }
        }
    }

    if (!persistent && mkdir(LUANTI_ROOT, 0777) != 0 && errno != EEXIST) {
        std::cout << "emloop_init_fs: could not create " LUANTI_ROOT << ": "
                  << strerror(errno) << std::endl;
    }

    std::cout << "emloop_init_fs: " LUANTI_ROOT " is backed by "
              << (persistent ? "OPFS (persistent)" : "memory (not persistent)")
              << std::endl;

    MAIN_THREAD_EM_ASM({
        emloop_fs_ready($0);
    }, persistent ? 1 : 0);
}

void emloop_init_fs(int want_opfs) {
    bool want = want_opfs ? true : false;
    post([want]() { do_init_fs(want); });
}

/////////////////////////////////////////////////////////////////////////////
// Pack installation
/////////////////////////////////////////////////////////////////////////////

static std::string packMetaPath(const std::string &name, const char *suffix) {
    return std::string(PACK_DB_DIR "/") + name + suffix;
}

// A pack name ends up in a file name, so keep it to something harmless.
static bool validPackName(const std::string &name) {
    if (name.empty() || name.size() > 64 || name[0] == '.' || name[0] == '-')
        return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok)
            return false;
    }
    return true;
}

// Delete what a previous version of this pack left behind, so that files
// dropped upstream do not linger in persistent storage. Only paths recorded in
// the pack's own manifest are touched, so worlds and content the player
// installed are left alone.
static void removeInstalledFiles(const std::string &name) {
    std::string manifest;
    if (!readWholeFile(packMetaPath(name, ".files"), manifest))
        return;

    std::vector<std::string> dirs;
    std::istringstream is(manifest);
    std::string line;
    while (std::getline(is, line)) {
        if (line.size() < 3 || line[1] != ' ')
            continue;
        const char kind = line[0];
        std::string path = line.substr(2);
        if (!insideLuantiRoot(path))
            continue;
        if (kind == 'D') {
            dirs.push_back(path);
        } else {
            unlink(path.c_str());
        }
    }

    // Deepest first, so that a directory is empty by the time it is reached.
    // rmdir fails harmlessly on anything that still holds files.
    std::sort(dirs.begin(), dirs.end(), [](const std::string &a, const std::string &b) {
        return a.size() > b.size();
    });
    for (const std::string &dir : dirs) {
        if (dir == PACK_DB_DIR)
            continue;
        rmdir(dir.c_str());
    }
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

static void notifyPackProgress(const std::string &name, double fraction) {
    MAIN_THREAD_EM_ASM({
        emloop_pack_progress(UTF8ToString($0), $1);
    }, name.c_str(), fraction);
}

// Adapted from https://github.com/libarchive/libarchive/wiki/Examples#a-complete-extractor
//
// Every path extracted below LUANTI_ROOT is appended to `manifest` so that the
// next version of the pack can clean up after this one.
static bool extract_archive(struct archive *a, const std::string &name, size_t total,
                            std::string &manifest)
{
    struct archive *ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_PERM);
    archive_write_disk_set_standard_lookup(ext);
    // Unpacking into OPFS takes a few seconds the first time, so keep the
    // launcher's progress bar moving. How much of the compressed input has been
    // consumed is a good enough stand-in for how much work is left.
    int reported = 0;
    for (;;) {
        if (total) {
            int pct = (int)((100 * archive_filter_bytes(a, -1)) / (la_int64_t)total);
            if (pct >= reported + 2) {
                reported = pct;
                notifyPackProgress(name, pct / 100.0);
            }
        }
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
        const char *rawpath = archive_entry_pathname(entry);
        if (rawpath) {
            std::string path = entryAbsolutePath(rawpath);
            if (insideLuantiRoot(path)) {
                manifest += (archive_entry_filetype(entry) == AE_IFDIR) ? "D " : "F ";
                manifest += path;
                manifest += '\n';
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

static bool unpack(const std::string &name, void *data, size_t size, std::string &manifest) {
    struct archive *a = archive_read_new();
    bool ok = false;
    if (archive_read_support_filter_zstd(a) != ARCHIVE_OK) {
        std::cout << "emloop_install_pack failed: zstd not supported" << std::endl;
    } else if (archive_read_support_format_tar(a) != ARCHIVE_OK) {
        std::cout << "emloop_install_pack failed: tar not supported" << std::endl;
    } else if (archive_read_open_memory(a, data, size) != ARCHIVE_OK) {
        std::cout << "emloop_install_pack failed: invalid archive" << std::endl;
    } else if (extract_archive(a, name, size, manifest)) {
        std::cout << "emloop_install_pack: Installed " << name << " successfully" << std::endl;
        ok = true;
    }
    archive_read_close(a);
    archive_read_free(a);
    return ok;
}

static void notifyPackInstalled(const std::string &name) {
    MAIN_THREAD_EM_ASM({
        emloop_pack_installed(UTF8ToString($0));
    }, name.c_str());
}

// `version` identifies the contents of the pack. An empty version means the
// pack is never remembered, which is what packs installing outside
// LUANTI_ROOT (the CA certificate bundle) need.
static void do_install_pack(const std::string &name, const std::string &version,
                            void *data, size_t size) {
    const bool remember = persistent && !version.empty();

    if (remember) {
        std::string installed;
        if (readWholeFile(packMetaPath(name, ".ver"), installed) && installed == version) {
            std::cout << "emloop_install_pack: " << name
                      << " is already installed, skipping" << std::endl;
            notifyPackInstalled(name);
            return;
        }
        // Whatever is on disk is stale. Drop it before laying down the new
        // version, and drop the marker first so that an interrupted install is
        // retried rather than trusted.
        unlink(packMetaPath(name, ".ver").c_str());
        removeInstalledFiles(name);
        unlink(packMetaPath(name, ".files").c_str());
    }

    std::string manifest;
    bool ok = unpack(name, data, size, manifest);

    if (remember && ok) {
        if (makeDirs(PACK_DB_DIR) &&
                writeWholeFile(packMetaPath(name, ".files"), manifest)) {
            writeWholeFile(packMetaPath(name, ".ver"), version);
        } else {
            std::cout << "emloop_install_pack: could not record " << name
                      << " as installed; it will be unpacked again next time"
                      << std::endl;
        }
    }
    notifyPackInstalled(name);
}

void emloop_install_pack(const char *name, const char *version, void *data, size_t size) {
    std::string packName(name ? name : "");
    std::string packVersion(version ? version : "");
    if (!validPackName(packName)) {
        std::cout << "emloop_install_pack: rejecting invalid pack name" << std::endl;
        free(data);
        return;
    }
    post([packName, packVersion, data, size]() {
        do_install_pack(packName, packVersion, data, size);
        free(data);
    });
}

/////////////////////////////////////////////////////////////////////////////
// minetest.conf
/////////////////////////////////////////////////////////////////////////////

static std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// The keys already present in a minetest.conf. Values supplied by the launcher
// are applied as defaults rather than overwritten on top, so that settings the
// player changed in-game survive the next launch.
static std::set<std::string> confKeys(const std::string &contents) {
    std::set<std::string> keys;
    std::istringstream is(contents);
    std::string line;
    while (std::getline(is, line)) {
        std::string entry = trim(line);
        if (entry.empty() || entry[0] == '#')
            continue;
        size_t eq = entry.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(entry.substr(0, eq));
        if (key.empty())
            continue;
        keys.insert(key);
        // Skip the body of a multi-line value.
        if (trim(entry.substr(eq + 1)) == "\"\"\"") {
            while (std::getline(is, line)) {
                if (trim(line) == "\"\"\"")
                    break;
            }
        }
    }
    return keys;
}

static void do_set_conf(const std::string &contents) {
    const std::string path = LUANTI_ROOT "/minetest.conf";

    std::string existing;
    readWholeFile(path, existing);
    std::set<std::string> present = confKeys(existing);

    std::string out = existing;
    if (!out.empty() && out.back() != '\n')
        out.push_back('\n');

    std::istringstream is(contents);
    std::string line;
    while (std::getline(is, line)) {
        std::string entry = trim(line);
        if (entry.empty() || entry[0] == '#')
            continue;
        size_t eq = entry.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(entry.substr(0, eq));
        if (key.empty() || present.count(key))
            continue;
        present.insert(key);
        out += entry;
        out.push_back('\n');
    }

    if (out == existing)
        return;
    if (!writeWholeFile(path, out))
        std::cout << "emloop_set_conf: could not write " << path << std::endl;
}

void emloop_set_conf(const char *contents) {
    std::string conf(contents ? contents : "");
    post([conf]() { do_set_conf(conf); });
}

/////////////////////////////////////////////////////////////////////////////
// Startup
/////////////////////////////////////////////////////////////////////////////

int main2(int argc, char *argv[]);

// This is called in the browser thread. main is called in a worker
void emloop_invoke_main(int argc, char* argv[]) {
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockchange);
    emscripten_set_pointerlockchange_callback("#canvas", 0, 1, report_pointerlockchange);
    emscripten_set_pointerlockerror_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, report_pointerlockerror);
    emscripten_set_pointerlockerror_callback("#canvas", 0, 1, report_pointerlockerror);

    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, on_key);
    emscripten_set_mousedown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, on_mouse);

    post([argc, argv]() {
        main_argc = argc;
        main_argv = argv;
        main_requested = true;
    });
}

// Run queued filesystem work until the launcher asks for main().
static void emloop_main_loop() {
    while (!main_requested) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cond.wait(lock, []() { return !queue.empty(); });
            work = std::move(queue.front());
            queue.pop_front();
        }
        work();
    }
}

int main(int argc, char *argv[])
{
    std::cout << "ENTERED main()" << std::endl;

    emloop_init();

    MAIN_THREAD_EM_ASM({
        emloop_ready();
    });

    emloop_main_loop();

    std::cout << "Main received args:" << std::endl;
    for (int i = 0; i < main_argc; i++) {
        std::cout << "    " << main_argv[i] << std::endl;
    }
    return main2(main_argc, main_argv);
}
