#include "loader/notify.hpp"
#include <windows.h>
#include <shellapi.h>
#include <cwchar>

// This target has no /utf-8: accented text is written with escapes.
bool ShowIncompatibleGameNotice() {
    const HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!window) return false;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    data.hIcon = LoadIconW(nullptr, IDI_WARNING);
    data.dwInfoFlags = NIIF_WARNING;
    wcscpy_s(data.szTip, L"Dofus Enhanced");
    wcscpy_s(data.szInfoTitle, L"Dofus Enhanced en attente");
    wcscpy_s(data.szInfo, L"Dofus a été mis à jour et aucune version du mod ne lui correspond encore. "
        L"Le jeu reste intact. Le mod reviendra au lancement qui suivra la publication d’une version adaptée.");
    const bool shown = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (shown) {
        Sleep(20000);
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    DestroyWindow(window);
    return shown;
}
