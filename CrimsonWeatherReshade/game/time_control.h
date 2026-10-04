#pragma once

struct ResolvedEnv {
    long long entity = 0;
    long long weatherState = 0;
    long long cloudNode = 0;
    long long windNode = 0;
    long long particleMgr = 0;
    bool valid = false;
};

ResolvedEnv ResolveEnv();
float Clamp01(float v);
float NormalizeHour24(float h);
bool ResolveTimeContext(void*& outEnvMgr, long long& outEntity);
bool TryReadCurrentTimeRaw(void* envMgr, float& outRaw);
bool TrySetTimeRaw(long long entity, float raw);
void CaptureTimeLimitBaseline(long long entity);
void RestoreTimeLimitBaseline(long long entity);
float UIHourToEngineRaw(float hour);
bool TryReadCurrentHourFromEntity(long long entity, float& outHour);
void TickTimeControl();
void SuspendTimeControl();
