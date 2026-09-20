#pragma once

#if defined(CW_DEV_BUILD)

#include <cstddef>

struct ID3D12Device;

struct PerformanceBenchmarkStatus {
    bool active = false;
    bool abortPending = false;
    size_t scenarioIndex = 0;
    size_t scenarioCount = 0;
    float scenarioProgress = 0.0f;
    float overallProgress = 0.0f;
    unsigned int estimatedSecondsRemaining = 0;
    const char* phase = "Idle";
    const char* scenario = "";
    const char* reportPath = "";
    const char* message = "Ready";
};

bool PerformanceBenchmarkStart(char* outError, size_t outErrorSize);
void PerformanceBenchmarkAbort();
void PerformanceBenchmarkOnPresent(ID3D12Device* device);
void PerformanceBenchmarkShutdown();
PerformanceBenchmarkStatus PerformanceBenchmarkGetStatus();

#endif
