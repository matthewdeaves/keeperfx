#include "pre_inc.h"
#include "kfx/platform/PlatformMacOS.h"
#include <mach-o/dyld.h>
#include <limits.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "post_inc.h"

// Bundled config-defaults dir (see config_keeperfx.c); set from the .app's Contents/Resources.
extern "C" char keeper_defaults_directory[640];

const char* PlatformMacOS::GetOSVersion() const { return "macOS"; }

bool PlatformMacOS::GetUserDataBaseDir(char* out, size_t out_size) const
{
    const char* home = getenv("HOME");
    if (home == nullptr || home[0] == '\0')
        return false;
    snprintf(out, out_size, "%s/Library/Application Support/KeeperFX", home);
    return true;
}

// When running from inside <name>.app/Contents/MacOS/, chdir to the folder holding
// the .app. Outside a bundle (e.g. running bin/keeperfx directly), leave cwd alone.
void PlatformMacOS::EarlyStartup()
{
    char exe[PATH_MAX];
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0)
        return;
    char resolved[PATH_MAX];
    if (realpath(exe, resolved) == nullptr)
        return;
    char* marker = strstr(resolved, "/Contents/MacOS/");
    if (marker == nullptr)
        return; // not inside an .app bundle; leave the working directory alone
    *marker = '\0';                       // resolved -> ".../<name>.app"
    // Record Contents/Resources as the bundled config-defaults fallback dir.
    snprintf(keeper_defaults_directory, sizeof(keeper_defaults_directory),
             "%s/Contents/Resources", resolved);
    char* slash = strrchr(resolved, '/');
    if (slash == nullptr)
        return;
    *slash = '\0';                        // resolved -> folder containing the .app
    if (chdir(resolved) != 0)
        return;
}
