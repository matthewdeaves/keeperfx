#ifndef PLATFORM_MACOS_H
#define PLATFORM_MACOS_H

#include "kfx/platform/PlatformLinux.h"

/** macOS desktop platform. Shares the POSIX directory walk, redbook stubs and
 *  SDL video init with PlatformLinux; adds the .app bundle handling that Linux
 *  has no equivalent of. */
class PlatformMacOS : public PlatformLinux {
public:
    const char* GetOSVersion() const override;

    /** Finder launches an .app with cwd "/", but the game data sits next to the
     *  .app, so the working directory has to be fixed before anything opens a
     *  file. Also records Contents/Resources as the bundled-defaults dir. */
    void EarlyStartup() override;
};

#endif // PLATFORM_MACOS_H
