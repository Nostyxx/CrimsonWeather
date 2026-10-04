#include "update/update_install.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

std::string TempDirectory() {
    char tempPath[MAX_PATH] = {};
    char tempFile[MAX_PATH] = {};
    Require(GetTempPathA(MAX_PATH, tempPath) != 0, "temporary directory is available");
    Require(GetTempFileNameA(tempPath, "cwu", 0, tempFile) != 0, "temporary directory name is available");
    DeleteFileA(tempFile);
    Require(CreateDirectoryA(tempFile, nullptr) != 0, "temporary directory can be created");
    return tempFile;
}

std::string Join(const std::string& directory, const char* name) {
    return directory + "\\" + name;
}

void WriteFile(const std::string& path, const char* contents) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "test artifact can be created");
    const DWORD length = static_cast<DWORD>(std::strlen(contents));
    DWORD written = 0;
    const BOOL writeSucceeded = ::WriteFile(file, contents, length, &written, nullptr);
    CloseHandle(file);
    Require(writeSucceeded && written == length, "test artifact can be written");
}

std::string ReadContents(const std::string& path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "expected artifact can be opened");
    char contents[64] = {};
    DWORD read = 0;
    const BOOL readSucceeded = ::ReadFile(file, contents, sizeof(contents) - 1, &read, nullptr);
    CloseHandle(file);
    Require(readSucceeded, "expected artifact can be read");
    return std::string(contents, read);
}

void RemoveDirectoryWithFiles(const std::string& directory) {
    const char* names[] = { "current.addon", "current.addon.new", "current.addon.old" };
    for (const char* name : names) DeleteFileA(Join(directory, name).c_str());
    RemoveDirectoryA(directory.c_str());
}

void TestInterruptedRenameRecovery() {
    const std::string directory = TempDirectory();
    const std::string current = Join(directory, "current.addon");
    const std::string staged = Join(directory, "current.addon.new");
    const std::string backup = Join(directory, "current.addon.old");
    WriteFile(backup, "previous-valid-addon");
    WriteFile(staged, "unactivated-new-addon");

    std::string detail;
    Require(update_install::RecoverInterruptedInstall(current.c_str(), staged.c_str(), backup.c_str(), detail),
        "startup restores the previous add-on when current is missing");
    Require(ReadContents(current) == "previous-valid-addon", "restored file contains the previous add-on");
    Require(GetFileAttributesA(staged.c_str()) == INVALID_FILE_ATTRIBUTES, "stale staged add-on is removed after restore");
    Require(GetFileAttributesA(backup.c_str()) == INVALID_FILE_ATTRIBUTES, "backup is removed only after current is readable");
    RemoveDirectoryWithFiles(directory);
}

void TestSuccessfulActivation() {
    const std::string directory = TempDirectory();
    const std::string current = Join(directory, "current.addon");
    const std::string staged = Join(directory, "current.addon.new");
    const std::string backup = Join(directory, "current.addon.old");
    WriteFile(current, "old-addon");
    WriteFile(staged, "new-addon");
    std::string error;
    Require(update_install::ActivateStagedFile(current.c_str(), staged.c_str(), backup.c_str(), error),
        "valid staged add-on activates");
    Require(ReadContents(current) == "new-addon", "new add-on becomes current");
    Require(GetFileAttributesA(backup.c_str()) == INVALID_FILE_ATTRIBUTES, "backup is removed after activation validates");
    RemoveDirectoryWithFiles(directory);
}

void TestLockedCurrentPreservesExistingAddon() {
    const std::string directory = TempDirectory();
    const std::string current = Join(directory, "current.addon");
    const std::string staged = Join(directory, "current.addon.new");
    const std::string backup = Join(directory, "current.addon.old");
    WriteFile(current, "old-addon");
    WriteFile(staged, "new-addon");
    HANDLE lock = CreateFileA(current.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "locked-target fixture opens");
    std::string error;
    const bool installed = update_install::ActivateStagedFile(current.c_str(), staged.c_str(), backup.c_str(), error);
    CloseHandle(lock);
    Require(!installed, "locked current add-on prevents replacement");
    Require(ReadContents(current) == "old-addon", "locked install failure preserves current add-on bytes");
    Require(GetFileAttributesA(staged.c_str()) == INVALID_FILE_ATTRIBUTES, "failed staging is cleaned up");
    RemoveDirectoryWithFiles(directory);
}

void TestFailedActivationRestoresAndCanRecover() {
    const std::string directory = TempDirectory();
    const std::string current = Join(directory, "current.addon");
    const std::string staged = Join(directory, "current.addon.new");
    const std::string backup = Join(directory, "current.addon.old");
    WriteFile(current, "old-addon");
    WriteFile(staged, "new-addon");
    int moveCount = 0;
    const auto failSecondMove = [&moveCount](const char* source, const char* destination, DWORD flags) {
        ++moveCount;
        if (moveCount == 2) {
            SetLastError(ERROR_ACCESS_DENIED);
            return false;
        }
        return MoveFileExA(source, destination, flags) != 0;
    };
    std::string error;
    Require(!update_install::ActivateStagedFile(current.c_str(), staged.c_str(), backup.c_str(), error, failSecondMove),
        "activation failure is reported");
    Require(ReadContents(current) == "old-addon", "failed activation restores previous bytes");
    Require(GetFileAttributesA(backup.c_str()) == INVALID_FILE_ATTRIBUTES, "successful rollback consumes backup");

    WriteFile(current, "old-addon");
    WriteFile(staged, "new-addon");
    moveCount = 0;
    const auto failActivationAndRollback = [&moveCount](const char* source, const char* destination, DWORD flags) {
        ++moveCount;
        if (moveCount >= 2) {
            SetLastError(ERROR_ACCESS_DENIED);
            return false;
        }
        return MoveFileExA(source, destination, flags) != 0;
    };
    Require(!update_install::ActivateStagedFile(current.c_str(), staged.c_str(), backup.c_str(), error, failActivationAndRollback),
        "failed activation and rollback leave recovery files");
    Require(GetFileAttributesA(current.c_str()) == INVALID_FILE_ATTRIBUTES, "failed rollback leaves current absent");
    Require(ReadContents(backup) == "old-addon", "failed rollback preserves backup bytes");
    std::string detail;
    Require(update_install::RecoverInterruptedInstall(current.c_str(), staged.c_str(), backup.c_str(), detail),
        "next startup recovers an interrupted activation");
    Require(ReadContents(current) == "old-addon", "startup recovery restores the old add-on");
    RemoveDirectoryWithFiles(directory);
}

} // namespace

int main() {
    TestInterruptedRenameRecovery();
    TestSuccessfulActivation();
    TestLockedCurrentPreservesExistingAddon();
    TestFailedActivationRestoresAndCanRecover();
    std::puts("Updater install recovery harness passed.");
    return 0;
}
