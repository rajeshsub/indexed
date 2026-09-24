#include "settings/HelperSettings.h"

#include <unistd.h>

#include <filesystem>

namespace indexed {

ElevationError LoadSettingsForRoot(const std::string& configPath, const TargetUser& user,
                                   Settings& out) {
    const std::string configDir = std::filesystem::path(configPath).parent_path().string();
    int fd = -1;
    const ElevationError error = OpenRegularFileForRootRead(configPath, user.uid, configDir, &fd);
    if (error != ElevationError::kNone) {
        return error;
    }
    Settings viaFd("/proc/self/fd/" + std::to_string(fd), user.homeDir);
    const bool loaded = viaFd.Load();
    close(fd);
    if (!loaded) {
        return ElevationError::kOpenFailed;
    }
    out = viaFd;
    return ElevationError::kNone;
}

}  // namespace indexed
