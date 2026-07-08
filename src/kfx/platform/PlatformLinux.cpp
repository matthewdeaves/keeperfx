#include "pre_inc.h"
#include "kfx/platform/PlatformLinux.h"
#include "kfx/platform/FileFind.h"
#include "platform.h" // kfxmain
#include "bflib_fileio.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <memory>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <unistd.h>
#include <limits.h>
#include <cerrno>
#include <cstdio>
#include "post_inc.h"

// Per-user writable dir, kept out of the read-only game folder (see config.h).
extern "C" char keeper_userdata_directory[640];
extern "C" char keeper_runtime_directory[152];

static bool filespec_is_pattern(const char* filespec)
{
    return strchr(filespec, '*') != nullptr;
}

static std::string directory_from_filespec(const char* filespec)
{
    const auto sep = strrchr(filespec, '/');
    if (sep && sep != filespec) {
        return std::string(filespec, sep - filespec);
    }
    return ".";
}

const char* PlatformLinux::GetOSVersion() const { return "Linux"; }
const void* PlatformLinux::GetImageBase() const { return nullptr; }
const char* PlatformLinux::GetWineVersion() const { return nullptr; } // running native
const char* PlatformLinux::GetWineHost() const { return nullptr; }    // running native

TbFileFind* PlatformLinux::FileFindFirst(const char* filespec, TbFileEntry* entry)
{
    try {
        auto ff = std::make_unique<TbFileFind>();
        bool is_pattern = filespec_is_pattern(filespec);
        std::string path = is_pattern ? directory_from_filespec(filespec) : filespec;
        DIR* handle = opendir(path.c_str());
        if (handle) {
            while (auto de = readdir(handle)) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                    continue;
                }
                const std::string file_path = path + "/" + de->d_name;
                if (is_pattern && fnmatch(filespec, file_path.c_str(), FNM_FILE_NAME | FNM_CASEFOLD) != 0) {
                    continue;
                }
                struct stat sb;
                if (stat(file_path.c_str(), &sb) < 0 || !S_ISREG(sb.st_mode)) {
                    continue;
                }
                std::string key = de->d_name;
                for (size_t i = 0; i < key.size(); i++) {
                    key[i] = (char)tolower((unsigned char)key[i]);
                }
                ff->names.emplace_back(key, de->d_name);
            }
            closedir(handle);
        }
        if (!ff->names.empty()) {
            std::sort(ff->names.begin(), ff->names.end());
            entry->Filename = ff->names[0].second.c_str();
            return ff.release();
        }
    } catch (...) {}
    return nullptr;
}

bool PlatformLinux::VideoInit()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        return false;
    atexit(SDL_Quit);
    return true;
}

void   PlatformLinux::SetRedbookVolume(SoundVolume) {}
TbBool PlatformLinux::PlayRedbookTrack(int) { return false; }
void   PlatformLinux::PauseRedbookTrack() {}
void   PlatformLinux::ResumeRedbookTrack() {}
void   PlatformLinux::StopRedbookTrack() {}

int  PlatformLinux::InitSteam() { return -1; }
void PlatformLinux::ShutdownSteam() {}

/******************************************************************************/
// Per-user data directory.

// Create a directory and any missing parents (like mkdir -p). Returns false on failure.
static bool make_dirs(const char* path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0)
        return false;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

// One-time: if dst is empty, copy the regular files from src (the old in-place
// save/ dir) so existing saves survive the move. Copies, never moves.
static void migrate_dir(const char* src, const char* dst)
{
    DIR* dd = opendir(dst);
    if (dd != nullptr) {
        struct dirent* e;
        while ((e = readdir(dd)) != nullptr) {
            if (e->d_name[0] == '.')
                continue;
            closedir(dd);
            return; // already populated -- leave it alone
        }
        closedir(dd);
    }
    DIR* sd = opendir(src);
    if (sd == nullptr)
        return;
    struct dirent* e;
    while ((e = readdir(sd)) != nullptr) {
        if (e->d_name[0] == '.')
            continue;
        char sp[PATH_MAX], dp[PATH_MAX];
        snprintf(sp, sizeof(sp), "%s/%s", src, e->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, e->d_name);
        struct stat st;
        if (stat(sp, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        FILE* in = fopen(sp, "rb");
        if (in == nullptr)
            continue;
        FILE* out = fopen(dp, "wb");
        if (out == nullptr) {
            fclose(in);
            continue;
        }
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
            fwrite(buf, 1, n, out);
        fclose(in);
        fclose(out);
    }
    closedir(sd);
}

bool PlatformLinux::GetUserDataBaseDir(char* out, size_t out_size) const
{
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg != nullptr && xdg[0] == '/') { // spec: relative XDG paths are invalid
        snprintf(out, out_size, "%s/keeperfx", xdg);
        return true;
    }
    const char* home = getenv("HOME");
    if (home == nullptr || home[0] == '\0')
        return false;
    snprintf(out, out_size, "%s/.local/share/keeperfx", home);
    return true;
}

// Point saves/screenshots at this platform's per-user dir (see ADR 0001),
// creating it and migrating any existing saves once.
void PlatformLinux::SetupUserDataDirectories()
{
    char base[640];
    if (!GetUserDataBaseDir(base, sizeof(base)))
        return;
    if (!make_dirs(base))
        return;

    char savedir[700], shotdir[700];
    snprintf(savedir, sizeof(savedir), "%s/save", base);
    snprintf(shotdir, sizeof(shotdir), "%s/scrshots", base);
    if (make_dirs(savedir) && keeper_runtime_directory[0] != '\0') {
        char oldsave[200];
        snprintf(oldsave, sizeof(oldsave), "%s/save", keeper_runtime_directory);
        migrate_dir(oldsave, savedir);
    }
    make_dirs(shotdir);

    snprintf(keeper_userdata_directory, sizeof(keeper_userdata_directory), "%s", base);
}

/******************************************************************************/
// Process entry point.

extern "C" int main(int argc, char *argv[])
{
    GetPlatform()->EarlyStartup();
    return kfxmain(argc, argv);
}
