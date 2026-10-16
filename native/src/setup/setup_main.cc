// StreamAudioFilter — saf-setup.exe
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Without arguments: a small window (choose Discord, Install / Uninstall).
// With arguments: the same operations for scripts and CI.
//
//   saf-setup.exe --list
//   saf-setup.exe --status
//   saf-setup.exe --install [--discord <folder>] [--payload-dir <dir>]
//   saf-setup.exe --uninstall
//   saf-setup.exe --version
//   common: --appdata <dir> --localappdata <dir>   (tests: a fake profile)
//
// The installation data (Vencord build + audio host) is embedded as resource 1
// (RCDATA); --payload-dir uses a folder instead (scripts/install.ps1 does that
// with a Vencord build made from source).
//
// Nothing is downloaded, nothing outside the user's profile is changed, no
// administrator rights are requested.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../version.h"
#include "installer.h"

using namespace saf::setup;

namespace {

bool russian() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN; }

const wchar_t* tr(const wchar_t* en, const wchar_t* ru) { return russian() ? ru : en; }

bool loadEmbeddedPayload(Payload& payload, std::string& error) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(1), RT_RCDATA);
    HGLOBAL handle = resource ? LoadResource(nullptr, resource) : nullptr;
    const char* data = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
    if (!data) {
        error = "this saf-setup.exe contains no installation data; use --payload-dir";
        return false;
    }
    return loadPayloadFromBytes(data, SizeofResource(nullptr, resource), payload, error);
}

std::wstring describe(const DiscordInstall& discord) {
    std::wstring text = discord.name + L" " + discord.version;
    if (discord.injection == "vencord") text += tr(L"  (Vencord installed)", L"  (Vencord установлен)");
    if (discord.running) text += tr(L"  - running", L"  - запущен");
    return text;
}

// ------------------------------------------------------------------ command line

/**
 * A GUI subsystem program has no console of its own. When started with
 * redirected output (scripts, CI) the inherited handle is used; when started
 * from a console window, that console is attached.
 */
HANDLE g_out = nullptr;

void print(const std::string& text) {
    if (!g_out) return;
    DWORD written = 0;
    WriteFile(g_out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

int runCli(const std::vector<std::wstring>& args) {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!g_out || g_out == INVALID_HANDLE_VALUE || GetFileType(g_out) == FILE_TYPE_UNKNOWN) {
        g_out = AttachConsole(ATTACH_PARENT_PROCESS) ? CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr) : nullptr;
        if (g_out == INVALID_HANDLE_VALUE) g_out = nullptr;
    }
    SetConsoleOutputCP(CP_UTF8);

    Paths paths = defaultPaths();
    std::wstring command, discordRoot, payloadDir;
    for (size_t i = 0; i < args.size(); i++) {
        const std::wstring& a = args[i];
        auto next = [&]() { return i + 1 < args.size() ? args[++i] : std::wstring(); };
        if (a == L"--appdata") paths.appData = next();
        else if (a == L"--localappdata") paths.localAppData = next();
        else if (a == L"--discord") discordRoot = next();
        else if (a == L"--payload-dir") payloadDir = next();
        else command = a;
    }
    const Log log = [](const std::string& line) { print(line + "\n"); };

    if (command == L"--version") {
        print(std::string(SAF_VERSION) + "\n");
        return 0;
    }
    const auto discords = findDiscordInstalls(paths);
    if (command == L"--list") {
        for (const auto& d : discords) {
            print(toUtf8(d.name) + "\t" + toUtf8(d.version) + "\t" + toUtf8(d.root) + "\t" + d.injection + (d.running ? "\trunning" : "") + "\n");
        }
        return 0;
    }
    if (command == L"--status") {
        const Status status = readStatus(paths);
        print("vencord=" + std::to_string(status.vencordPresent) + " ours=" + std::to_string(status.ourVencord) + " version=" + status.installedVersion
            + " host=" + std::to_string(status.hostPresent) + " foreignBackup=" + std::to_string(status.foreignBackup) + "\n");
        return 0;
    }
    if (command == L"--uninstall") return uninstall(paths, discords, log).ok ? 0 : 1;
    if (command == L"--install") {
        Payload payload;
        std::string error;
        const bool loaded = payloadDir.empty() ? loadEmbeddedPayload(payload, error) : loadPayloadFromDirectory(payloadDir, payload, error);
        if (!loaded) {
            log("ERROR: " + error);
            return 1;
        }
        const DiscordInstall* target = nullptr;
        for (const auto& d : discords) {
            if (discordRoot.empty() || _wcsicmp(d.root.c_str(), discordRoot.c_str()) == 0) {
                target = &d;
                break;
            }
        }
        if (!target) {
            log("ERROR: no Discord installation found" + (discordRoot.empty() ? std::string() : " at " + toUtf8(discordRoot)));
            return 1;
        }
        return install(paths, *target, payload, log).ok ? 0 : 1;
    }
    print("usage: saf-setup.exe --list | --status | --install [--discord <folder>] [--payload-dir <dir>] | --uninstall | --version\n");
    return 2;
}

// ------------------------------------------------------------------------ window

enum : int { kCombo = 100, kInstall, kUninstall, kClose, kLog, kInfo };

struct Ui {
    HWND window = nullptr, combo = nullptr, log = nullptr, info = nullptr, install = nullptr, uninstall = nullptr;
    HFONT font = nullptr, bold = nullptr;
    Paths paths = defaultPaths();
    std::vector<DiscordInstall> discords;
    Payload payload;
    std::string payloadError;
    int dpi = 96;
} ui;

int S(int value) { return MulDiv(value, ui.dpi, 96); }

void appendLog(const std::string& line) {
    std::wstring text = toWide(line) + L"\r\n";
    const int length = GetWindowTextLengthW(ui.log);
    SendMessageW(ui.log, EM_SETSEL, length, length);
    SendMessageW(ui.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void refresh() {
    ui.discords = findDiscordInstalls(ui.paths);
    SendMessageW(ui.combo, CB_RESETCONTENT, 0, 0);
    for (const auto& d : ui.discords) SendMessageW(ui.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(describe(d).c_str()));
    if (!ui.discords.empty()) SendMessageW(ui.combo, CB_SETCURSEL, 0, 0);

    const Status status = readStatus(ui.paths);
    std::wstring info;
    if (status.ourVencord) {
        info = tr(L"Installed: StreamAudioFilter ", L"Установлено: StreamAudioFilter ") + toWide(status.installedVersion)
            + tr(L". Install again to repair or update.", L". Повторная установка исправит или обновит.");
    } else if (status.vencordPresent) {
        info = tr(L"Vencord is installed. Its build will be replaced by Vencord + StreamAudioFilter; your Vencord settings and plugins' settings stay. The current build is kept and restored on uninstall.",
                  L"Vencord установлен. Его сборка будет заменена на Vencord + StreamAudioFilter; настройки Vencord и плагинов сохранятся. Текущая сборка сохраняется и вернётся при удалении.");
    } else {
        info = tr(L"Vencord is not installed: it will be installed together with StreamAudioFilter.",
                  L"Vencord не установлен: он будет установлен вместе со StreamAudioFilter.");
    }
    if (!ui.payloadError.empty()) info = toWide("Error: " + ui.payloadError);
    SetWindowTextW(ui.info, info.c_str());
    EnableWindow(ui.install, !ui.discords.empty() && ui.payloadError.empty());
    EnableWindow(ui.uninstall, status.ourVencord || status.hostPresent);
}

void onInstall() {
    const LRESULT index = SendMessageW(ui.combo, CB_GETCURSEL, 0, 0);
    if (index < 0 || index >= static_cast<LRESULT>(ui.discords.size())) return;
    refresh();  // the running state may have changed
    const DiscordInstall& target = ui.discords[static_cast<size_t>(index)];
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const Result result = install(ui.paths, target, ui.payload, appendLog);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    refresh();
    MessageBoxW(ui.window,
        result.ok ? tr(L"StreamAudioFilter is installed.\n\nStart Discord. The plugin is in Settings > Vencord > Plugins; a button next to the microphone opens its panel.",
                       L"StreamAudioFilter установлен.\n\nЗапустите Discord. Плагин — в Настройки > Vencord > Plugins; кнопка рядом с микрофоном открывает его панель.")
                  : toWide(result.error).c_str(),
        L"StreamAudioFilter", result.ok ? MB_ICONINFORMATION : MB_ICONWARNING);
}

void onUninstall() {
    if (MessageBoxW(ui.window, tr(L"Remove StreamAudioFilter? Discord must be closed.", L"Удалить StreamAudioFilter? Discord должен быть закрыт."),
            L"StreamAudioFilter", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    refresh();
    const Result result = uninstall(ui.paths, ui.discords, appendLog);
    refresh();
    MessageBoxW(ui.window, result.ok ? tr(L"StreamAudioFilter was removed.", L"StreamAudioFilter удалён.") : toWide(result.error).c_str(),
        L"StreamAudioFilter", result.ok ? MB_ICONINFORMATION : MB_ICONWARNING);
}

HWND control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), ui.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return hwnd;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        ui.window = hwnd;
        ui.font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        ui.bold = CreateFontW(-S(20), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        control(L"STATIC", L"StreamAudioFilter " TEXT(SAF_VERSION), 0, 20, 16, 500, 28, 0, ui.bold);
        control(L"STATIC", tr(L"Choose which applications the viewers of your Discord screen share hear.",
                              L"Выбор приложений, которые слышат зрители вашей демонстрации экрана в Discord."), 0, 20, 48, 520, 20, 0, ui.font);
        control(L"STATIC", L"Discord:", 0, 20, 84, 70, 22, 0, ui.font);
        ui.combo = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 90, 80, 450, 200, kCombo, ui.font);
        ui.info = control(L"STATIC", L"", 0, 20, 116, 520, 56, kInfo, ui.font);
        ui.log = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 20, 178, 520, 120, kLog, ui.font);
        ui.install = control(L"BUTTON", tr(L"Install", L"Установить"), BS_DEFPUSHBUTTON | WS_TABSTOP, 20, 312, 150, 32, kInstall, ui.font);
        ui.uninstall = control(L"BUTTON", tr(L"Uninstall", L"Удалить"), WS_TABSTOP, 180, 312, 150, 32, kUninstall, ui.font);
        control(L"BUTTON", tr(L"Close", L"Закрыть"), WS_TABSTOP, 390, 312, 150, 32, kClose, ui.font);
        if (!loadEmbeddedPayload(ui.payload, ui.payloadError)) appendLog("ERROR: " + ui.payloadError);
        else appendLog("StreamAudioFilter " + ui.payload.version + ", Vencord " + ui.payload.vencord);
        refresh();
        if (ui.discords.empty()) appendLog(toUtf8(tr(L"No Discord installation was found in this user profile.", L"Discord в этом профиле пользователя не найден.")));
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kInstall: onInstall(); return 0;
        case kUninstall: onUninstall(); return 0;
        case kClose: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lParam) != ui.log) {
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int runGui(HINSTANCE instance) {
    INITCOMMONCONTROLSEX icc { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(nullptr);
    ui.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);

    WNDCLASSW wc {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"StreamAudioFilterSetup";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    RECT rect { 0, 0, S(560), S(360) };
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);
    CreateWindowW(wc.lpszClassName, L"StreamAudioFilter Setup", style, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    ShowWindow(ui.window, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(ui.window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    LocalFree(argv);
    return args.empty() ? runGui(instance) : runCli(args);
}
