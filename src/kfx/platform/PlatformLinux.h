#ifndef PLATFORM_LINUX_H
#define PLATFORM_LINUX_H

#include "kfx/platform/IPlatform.h"

#include <cstddef>

/** Linux desktop platform. */
class PlatformLinux : public IPlatform {
public:
    const char* GetOSVersion() const override;
    const void* GetImageBase() const override;
    const char* GetWineVersion() const override;
    const char* GetWineHost() const override;

    TbFileFind* FileFindFirst(const char* filespec, TbFileEntry* entry) override;

    void   SetRedbookVolume(SoundVolume vol) override;
    TbBool PlayRedbookTrack(int track) override;
    void   PauseRedbookTrack() override;
    void   ResumeRedbookTrack() override;
    void   StopRedbookTrack() override;

    int  InitSteam() override;
    void ShutdownSteam() override;

    bool VideoInit() override;

    void SetupUserDataDirectories() override;

protected:
    /** Where this OS keeps per-user application data. Linux follows the XDG
     *  base-directory spec; macOS overrides with ~/Library/Application Support.
     *  False when there is nowhere sensible to write (no HOME). */
    virtual bool GetUserDataBaseDir(char* out, size_t out_size) const;
};

#endif // PLATFORM_LINUX_H
