#include "platform.h"
#include "steam_api.hpp"
#include "bflib_crash.h"
#include "bflib_fileio.h"
#include "cdrom.h"
#include <algorithm>
#include <ctype.h>
#include <string>
#include <memory>
#include <utility>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fnmatch.h>
#include <limits.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
// Per-user writable dir, kept out of the read-only game folder (see config.h).
extern "C" char keeper_userdata_directory[640];
extern "C" char keeper_runtime_directory[152];
#ifdef __APPLE__
#include <mach-o/dyld.h>
// Bundled config-defaults dir (see config_keeperfx.c); set from the .app's Contents/Resources.
extern "C" char keeper_defaults_directory[640];
#endif

extern "C" const char * get_os_version() {
#ifdef __APPLE__
    return "macOS";
#else
    return "Linux";
#endif
}

extern "C" const void * get_image_base()
{
    return nullptr;
}

extern "C" const char * get_wine_version()
{
    return nullptr; // we're running native
}

extern "C" const char * get_wine_host()
{
    return nullptr; // we're running native
}

extern "C" void install_exception_handler()
{
	LbErrorParachuteInstall();
}

extern "C" int steam_api_init()
{
    // Steam not supported on Linux
    return 0;
}

extern "C" void steam_api_shutdown()
{
    // Steam not supported on Linux
}

extern "C" void SetRedbookVolume(SoundVolume)
{
    // TODO: implement CDROM features
}

extern "C" TbBool PlayRedbookTrack(int)
{
    // TODO: implement CDROM features
    return false;
}

extern "C" void PauseRedbookTrack()
{
    // TODO: implement CDROM features
}

extern "C" void ResumeRedbookTrack()
{
    // TODO: implement CDROM features
}

extern "C" void StopRedbookTrack()
{
    // TODO: implement CDROM features
}

struct TbFileFind {
	std::vector<std::pair<std::string, std::string>> names;
	size_t index = 0;
};

bool filespec_is_pattern(const char * filespec) {
	return strchr(filespec, '*') != nullptr;
}

std::string directory_from_filespec(const char * filespec) {
	const auto sep = strrchr(filespec, '/');
	if (sep && sep != filespec) {
		return std::string(filespec, sep - filespec);
	} else {
		return ".";
	}
}

extern "C" TbFileFind * LbFileFindFirst(const char * filespec, TbFileEntry * fe)
{
	try {
		auto ff = std::make_unique<TbFileFind>();
		bool is_pattern = filespec_is_pattern(filespec);
		std::string path;
		if (is_pattern) {
			path = directory_from_filespec(filespec);
		} else {
			path = filespec;
		}
		DIR *handle = opendir(path.c_str());
		if (handle) {
			while (true) {
				auto de = readdir(handle);
				if (!de) {
					break;
				}
				if (strcmp(de->d_name, ".") == 0) {
					continue;
				}
				if (strcmp(de->d_name, "..") == 0) {
					continue;
				}
				const std::string file_path = path + "/" + de->d_name;
				if (is_pattern) {
					if (fnmatch(filespec, file_path.c_str(), FNM_FILE_NAME | FNM_CASEFOLD) != 0) {
						continue;
					}
				}
				struct stat sb;
				if (stat(file_path.c_str(), &sb) < 0) {
					continue;
				}
				if (!S_ISREG(sb.st_mode)) {
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
			fe->Filename = ff->names[0].second.c_str();
			return ff.release();
		}
	} catch (...) {}
	return nullptr;
}

extern "C" int32_t LbFileFindNext(TbFileFind * ff, TbFileEntry * fe)
{
	try {
		if (ff) {
			ff->index++;
			if (ff->index < ff->names.size()) {
				fe->Filename = ff->names[ff->index].second.c_str();
				return 1;
			}
		}
	} catch (...) {}
	return -1;
}

extern "C" void LbFileFindEnd(TbFileFind * ff)
{
	delete ff;
}

#ifdef __APPLE__
// Finder launches an .app with cwd "/", but the game data sits next to the .app.
// When running from inside <name>.app/Contents/MacOS/, chdir to the folder holding
// the .app. Outside a bundle (e.g. running bin/keeperfx directly), leave cwd alone.
static void macos_chdir_to_bundle_parent(void)
{
	char exe[PATH_MAX];
	uint32_t size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &size) != 0)
		return;
	char resolved[PATH_MAX];
	if (realpath(exe, resolved) == nullptr)
		return;
	char *marker = strstr(resolved, "/Contents/MacOS/");
	if (marker == nullptr)
		return; // not inside an .app bundle; leave the working directory alone
	*marker = '\0';                       // resolved -> ".../<name>.app"
	// Record Contents/Resources as the bundled config-defaults fallback dir.
	snprintf(keeper_defaults_directory, sizeof(keeper_defaults_directory),
	         "%s/Contents/Resources", resolved);
	char *slash = strrchr(resolved, '/');
	if (slash == nullptr)
		return;
	*slash = '\0';                        // resolved -> folder containing the .app
	if (chdir(resolved) != 0)
		return;
}
#endif // __APPLE__

// Create a directory and any missing parents (like mkdir -p). Returns false on failure.
static bool make_dirs(const char *path)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", path);
	size_t len = strlen(tmp);
	if (len == 0)
		return false;
	if (tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	for (char *p = tmp + 1; *p != '\0'; p++) {
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
static void migrate_dir(const char *src, const char *dst)
{
	DIR *dd = opendir(dst);
	if (dd != nullptr) {
		struct dirent *e;
		while ((e = readdir(dd)) != nullptr) {
			if (e->d_name[0] == '.')
				continue;
			closedir(dd);
			return; // already populated — leave it alone
		}
		closedir(dd);
	}
	DIR *sd = opendir(src);
	if (sd == nullptr)
		return;
	struct dirent *e;
	while ((e = readdir(sd)) != nullptr) {
		if (e->d_name[0] == '.')
			continue;
		char sp[PATH_MAX], dp[PATH_MAX];
		snprintf(sp, sizeof(sp), "%s/%s", src, e->d_name);
		snprintf(dp, sizeof(dp), "%s/%s", dst, e->d_name);
		struct stat st;
		if (stat(sp, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		FILE *in = fopen(sp, "rb");
		if (in == nullptr)
			continue;
		FILE *out = fopen(dp, "wb");
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

// Point saves/screenshots at this platform's per-user dir (see ADR 0001),
// creating it and migrating any existing saves once. Call after the runtime dir
// is known.
extern "C" void setup_userdata_directories(void)
{
	char base[640];
#ifdef __APPLE__
	const char *home = getenv("HOME");
	if (home == nullptr || home[0] == '\0')
		return;
	snprintf(base, sizeof(base), "%s/Library/Application Support/KeeperFX", home);
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg != nullptr && xdg[0] == '/') {  // spec: relative XDG paths are invalid
		snprintf(base, sizeof(base), "%s/keeperfx", xdg);
	} else {
		const char *home = getenv("HOME");
		if (home == nullptr || home[0] == '\0')
			return;
		snprintf(base, sizeof(base), "%s/.local/share/keeperfx", home);
	}
#endif
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

extern "C" int main(int argc, char *argv[]) {
#ifdef __APPLE__
	macos_chdir_to_bundle_parent();
#endif
	return kfxmain(argc, argv);
}
