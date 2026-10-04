#pragma once

#include <functional>
#include <string>

#include <Windows.h>

namespace update_install {

using MoveFile = std::function<bool(const char* source, const char* destination, DWORD flags)>;

bool IsUsableFile(const char* path);
bool RecoverInterruptedInstall(
    const char* currentPath,
    const char* stagedPath,
    const char* backupPath,
    std::string& outDetail);
bool ActivateStagedFile(
    const char* currentPath,
    const char* stagedPath,
    const char* backupPath,
    std::string& outError,
    MoveFile moveFile = {});

} // namespace update_install
