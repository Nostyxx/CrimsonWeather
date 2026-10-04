#include "pch.h"

#include "config.h"
#include "logging.h"
#include "mod_metadata.h"

#include <share.h>

FILE* g_logFile = nullptr;
#if defined(CW_DEV_BUILD)
FILE* g_devLaunchLogFile = nullptr;
#endif
bool g_logEnabled = true;

namespace {

#if defined(CW_DEV_BUILD)
constexpr const char* kDevOptimizationLogDir = "C:\\Games\\Crimson Desert\\bin64\\optimizationLOG";

void EnsureDevOptimizationLogDir() {
    char path[MAX_PATH] = {};
    strcpy_s(path, kDevOptimizationLogDir);
    for (char* p = path; *p; ++p) {
        if (*p != '\\' || p == path || p[-1] == ':') {
            continue;
        }
        *p = '\0';
        CreateDirectoryA(path, nullptr);
        *p = '\\';
    }
    CreateDirectoryA(path, nullptr);
}
#endif

} // namespace

void Log(const char* fmt, ...) {
#if defined(CW_DEV_BUILD)
    if ((!g_logEnabled || !g_logFile) && !g_devLaunchLogFile) {
        return;
    }
#else
    if (!g_logEnabled || !g_logFile) {
        return;
    }
#endif

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    va_list args;
    va_start(args, fmt);

    if (g_logEnabled && g_logFile) {
        va_list copy{};
        va_copy(copy, args);
        fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        vfprintf(g_logFile, fmt, copy);
        fflush(g_logFile);
        va_end(copy);
    }

#if defined(CW_DEV_BUILD)
    if (g_devLaunchLogFile) {
        va_list copy{};
        va_copy(copy, args);
        fprintf(g_devLaunchLogFile, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        vfprintf(g_devLaunchLogFile, fmt, copy);
        fflush(g_devLaunchLogFile);
        va_end(copy);
    }
#endif

    va_end(args);
}

void OpenLogFile(const char* dir) {
    g_logEnabled = g_cfg.logEnabled;
    if (g_cfg.logEnabled) {
        char path[MAX_PATH] = {};
        if (dir && dir[0]) {
            sprintf_s(path, "%s\\%s", dir, MOD_LOG_FILE);
        } else {
            strcpy_s(path, MOD_LOG_FILE);
        }
        g_logFile = _fsopen(path, "w", _SH_DENYNO);
    }
    if (g_cfg.logEnabled && !g_logFile) {
        g_logEnabled = false;
    }

#if defined(CW_DEV_BUILD)
    const DevLaunchOption launchOption = g_devLaunchOption.load();
    if (launchOption != DevLaunchOption::Full) {
        EnsureDevOptimizationLogDir();
        char devPath[MAX_PATH] = {};
        sprintf_s(devPath, sizeof(devPath), "%s\\crimsonweather_%s.log",
            kDevOptimizationLogDir,
            DevLaunchOptionName(launchOption));
        g_devLaunchLogFile = _fsopen(devPath, "w", _SH_DENYNO);
        if (g_devLaunchLogFile) {
            Log("[dev] Optimization isolation log opened: %s\n", devPath);
        }
    }
#endif
}
