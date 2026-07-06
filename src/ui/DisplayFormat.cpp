#include "ui/DisplayFormat.h"

#include <cstdio>
#include <ctime>

namespace indexed {

std::string FormatFileSize(uint64_t bytes) {
    if (bytes < 1024ULL) {
        return std::to_string(bytes) + " B";
    }
    const char* unit = "KB";
    double value = static_cast<double>(bytes) / 1024.0;
    if (value >= 1024.0) {
        value /= 1024.0;
        unit = "MB";
        if (value >= 1024.0) {
            value /= 1024.0;
            unit = "GB";
        }
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f %s", value, unit);
    return buf;
}

std::string FormatDateTime(uint64_t nsSinceEpoch) {
    const time_t seconds = static_cast<time_t>(nsSinceEpoch / 1'000'000'000ULL);
    struct tm localTm{};
    localtime_r(&seconds, &localTm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &localTm);
    return buf;
}

}  // namespace indexed
