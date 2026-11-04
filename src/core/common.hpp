#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

inline constexpr char ExpectedGameHash[] = "ad013415455d93bd865aef70fcf1b7c6f704c799f85e2ef301aa7fa610ef2591";
inline constexpr char ExpectedMetadataHash[] = "ad2880162d7e0aee219fdc81e4a3817cc86cca4aba3df3ce86fd470c1bb9a6af";
std::string FileHash(const std::filesystem::path& path);
std::filesystem::path ModulePath(HMODULE module);
bool SupportedClient(const std::filesystem::path& exe);
std::filesystem::path StartupStatusPath(const std::filesystem::path& folder, DWORD pid);
void WriteStartupStatus(const std::filesystem::path& folder, DWORD pid, const std::string& status);
std::string ReadStartupStatus(const std::filesystem::path& folder, DWORD pid);
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
