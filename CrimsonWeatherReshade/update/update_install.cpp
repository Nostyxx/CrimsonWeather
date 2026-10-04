#include "pch.h"

#include "update_install.h"

namespace {

bool FileExists(const char* path) {
    return path && path[0] && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool DeleteIfPresent(const char* path) {
    if (!FileExists(path)) return true;
    if (DeleteFileA(path)) return true;
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool DefaultMoveFile(const char* source, const char* destination, DWORD flags) {
    return MoveFileExA(source, destination, flags);
}

} // namespace

namespace update_install {

bool IsUsableFile(const char* path) {
    if (!path || !path[0]) return false;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes.nFileSizeHigh == 0 && attributes.nFileSizeLow == 0)) {
        return false;
    }
    HANDLE file = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

bool RecoverInterruptedInstall(
    const char* currentPath,
    const char* stagedPath,
    const char* backupPath,
    std::string& outDetail) {
    outDetail.clear();
    const bool currentUsable = IsUsableFile(currentPath);
    const bool backupUsable = IsUsableFile(backupPath);
    if (!currentUsable && backupUsable) {
        if (!DefaultMoveFile(backupPath, currentPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            outDetail = "Could not restore the previous add-on from .old (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!IsUsableFile(currentPath)) {
            outDetail = "The restored add-on is not readable";
            return false;
        }
        outDetail = "Restored the previous add-on from .old";
    } else if (!currentUsable) {
        outDetail = "No readable current or backup add-on is available; preserved recovery files";
        return false;
    } else {
        outDetail = "Current add-on is readable";
    }

    // A usable current file now exists, so stale staging and backup files are safe to remove.
    if (!DeleteIfPresent(stagedPath)) {
        outDetail += "; could not remove stale .new file";
    }
    if (!DeleteIfPresent(backupPath)) {
        outDetail += "; could not remove stale .old backup";
    }
    return true;
}

bool ActivateStagedFile(
    const char* currentPath,
    const char* stagedPath,
    const char* backupPath,
    std::string& outError,
    MoveFile moveFile) {
    outError.clear();
    if (!IsUsableFile(stagedPath)) {
        outError = "Staged update is missing or unreadable";
        return false;
    }
    if (!IsUsableFile(currentPath)) {
        outError = "Current add-on is missing or unreadable";
        return false;
    }

    if (!moveFile) moveFile = DefaultMoveFile;
    if (!DeleteIfPresent(backupPath)) {
        outError = "Could not clear the previous backup (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!moveFile(currentPath, backupPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteIfPresent(stagedPath);
        outError = "Could not rename current add-on (" + std::to_string(code) + ")";
        return false;
    }
    if (!moveFile(stagedPath, currentPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        if (!moveFile(backupPath, currentPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            outError = "Could not activate new add-on (" + std::to_string(code) + "); previous add-on remains in .old";
        } else {
            DeleteIfPresent(stagedPath);
            outError = "Could not activate new add-on (" + std::to_string(code) + "); previous add-on restored";
        }
        return false;
    }
    if (!IsUsableFile(currentPath)) {
        outError = "Activated add-on is not readable; previous add-on remains in .old";
        return false;
    }
    // Keep .old if cleanup fails; startup recovery will only delete it after validating current.
    DeleteIfPresent(backupPath);
    return true;
}

} // namespace update_install
