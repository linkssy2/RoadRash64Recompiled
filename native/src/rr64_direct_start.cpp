#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768)
        return 1;
    const auto folder = std::filesystem::path(path).parent_path();
    const auto game = folder / L"RoadRash64Recompiled.exe";
    std::wstring command = L"\"" + game.wstring() + L"\" --skip-launcher";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, folder.c_str(), &startup, &process)) {
        MessageBoxW(
            nullptr,
            L"Could not start RoadRash64Recompiled.exe. Keep both executables together in the extracted build folder.",
            L"Road Rash 64 Recompiled", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
