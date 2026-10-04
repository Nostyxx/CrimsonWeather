#pragma once

#include <cstdio>

extern FILE* g_logFile;
#if defined(CW_DEV_BUILD)
extern FILE* g_devLaunchLogFile;
#endif
extern bool g_logEnabled;

void Log(const char* fmt, ...);
void OpenLogFile(const char* dir);
