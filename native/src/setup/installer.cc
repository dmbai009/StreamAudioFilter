// StreamAudioFilter — installer
// SPDX-License-Identifier: GPL-3.0-or-later

#include "installer.h"
#include "settings_json.h"

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace saf::setup {

namespace {

constexpr char kMagic[] = "SAFPAYLOAD1\n";
constexpr wchar_t kStateFile[] = L"install.txt";
constexpr uintmax_t kLoaderMaxSize = 64 * 1024;

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path) result = path;
    CoTaskMemFree(path);
    return result;
}

bool readFile(const fs::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

bool writeFile(const fs::path& path, const std::string& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(file);
}

std::map<std::string, std::string> parseKeyValues(const std::string& text) {
    std::map<std::string, std::string> values;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos) values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

/** Version strings like 1.0.9262, compared numerically part by part. */
bool versionLess(const std::wstring& a, const std::wstring& b) {
    std::wistringstream sa(a), sb(b);
    std::wstring pa, pb;
    while (true) {
        const bool ha = static_cast<bool>(std::getline(sa, pa, L'.'));
        const bool hb = static_cast<bool>(std::getline(sb, pb, L'.'));
        if (!ha && !hb) return false;
        const long va = ha ? std::wcstol(pa.c_str(), nullptr, 10) : 0;
        const long vb = hb ? std::wcstol(pb.c_str(), nullptr, 10) : 0;
        if (va != vb) return va < vb;
    }
}

bool processRunningUnder(const std::wstring& root) {
    const std::wstring prefix = lower(root) + L"\\";
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry { sizeof(entry) };
    bool found = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry)) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        wchar_t image[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(image));
        if (QueryFullProcessImageNameW(process, 0, image, &size) && lower(image).rfind(prefix, 0) == 0) found = true;
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}

fs::path vencordDir(const Paths& paths) { return fs::path(paths.appData) / L"Vencord"; }
fs::path safDir(const Paths& paths) { return vencordDir(paths) / L"streamAudioFilter"; }

std::string jsonEscape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

/**
 * Vencord's loader as Electron expects it: a tiny asar archive with an
 * index.js that loads Vencord's patcher, which in turn starts the original
 * Discord app from _app.asar. Same layout as Vencord's own installer.
 */
std::string makeLoaderAsar(const fs::path& patcher) {
    const std::string index = "require(\"" + jsonEscape(toUtf8(patcher.wstring())) + "\")";
    const std::string package = "{\n\t\"name\": \"discord\",\n\t\"main\": \"index.js\"\n}";
    const std::string header = "{\"files\":{\"index.js\":{\"size\":" + std::to_string(index.size()) + ",\"offset\":\"0\"},\"package.json\":{\"size\":"
        + std::to_string(package.size()) + ",\"offset\":\"" + std::to_string(index.size()) + "\"}}}";
    const uint32_t padded = static_cast<uint32_t>((header.size() + 3) & ~size_t(3));
    auto u32 = [](uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); };
    std::string out = u32(4) + u32(padded + 8) + u32(padded + 4) + u32(static_cast<uint32_t>(header.size())) + header;
    out.append(padded - header.size(), '\0');
    return out + index + package;
}

bool isLoaderFor(const fs::path& asar, const fs::path& patcher) {
    std::error_code ec;
    if (!fs::exists(asar, ec) || fs::file_size(asar, ec) > kLoaderMaxSize) return false;
    std::string data;
    if (!readFile(asar, data)) return false;
    return data.find(jsonEscape(toUtf8(patcher.wstring()))) != std::string::npos;
}

/** Runs `exe --version` and returns its first token ("" on failure). */
std::string runVersion(const fs::path& exe) {
    SECURITY_ATTRIBUTES sa { sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi {};
    std::wstring command = L"\"" + exe.wstring() + L"\" --version";
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    std::string output;
    if (started) {
        char buffer[256];
        DWORD read = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read) output.append(buffer, read);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(readPipe);
    const size_t end = output.find_first_of(" \r\n");
    return output.substr(0, end);
}

/** Undo journal: every completed step registers how to revert it. */
class Journal {
public:
    void add(std::function<void()> undo) { steps_.push_back(std::move(undo)); }
    void rollback(const Log& log) {
        log("rolling back");
        for (auto it = steps_.rbegin(); it != steps_.rend(); ++it) {
            try {
                (*it)();
            } catch (...) {
            }
        }
        steps_.clear();
    }
    void commit() { steps_.clear(); }

private:
    std::vector<std::function<void()>> steps_;
};

bool moveTo(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) error = "could not move " + toUtf8(from.wstring()) + " to " + toUtf8(to.wstring()) + ": " + ec.message();
    return !ec;
}

std::string timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buffer;
}

} // namespace

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring toWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

Paths defaultPaths() {
    return { knownFolder(FOLDERID_RoamingAppData), knownFolder(FOLDERID_LocalAppData) };
}

std::vector<DiscordInstall> findDiscordInstalls(const Paths& paths) {
    static const std::pair<const wchar_t*, const wchar_t*> kFlavours[] = {
        { L"Discord", L"Discord" },
        { L"Discord PTB", L"DiscordPTB" },
        { L"Discord Canary", L"DiscordCanary" },
        { L"Discord Development", L"DiscordDevelopment" },
    };
    std::vector<DiscordInstall> result;
    for (const auto& [name, folder] : kFlavours) {
        const fs::path root = fs::path(paths.localAppData) / folder;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;

        DiscordInstall install;
        install.name = name;
        install.root = root.wstring();
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            const std::wstring dir = entry.path().filename().wstring();
            if (!entry.is_directory() || dir.rfind(L"app-", 0) != 0) continue;
            if (!fs::exists(entry.path() / L"resources", ec)) continue;
            const std::wstring version = dir.substr(4);
            if (install.version.empty() || versionLess(install.version, version)) {
                install.version = version;
                install.appDir = entry.path().wstring();
            }
        }
        if (install.appDir.empty()) continue;
        const fs::path resources = fs::path(install.appDir) / L"resources";
        install.injection = fs::exists(resources / L"_app.asar", ec) ? "vencord" : "none";
        install.running = processRunningUnder(install.root);
        result.push_back(std::move(install));
    }
    return result;
}

// --------------------------------------------------------------------- payload

bool loadPayloadFromBytes(const char* data, size_t size, Payload& payload, std::string& error) {
    const size_t magic = sizeof(kMagic) - 1;
    if (size < magic || std::string(data, magic) != kMagic) {
        error = "the embedded installation data is damaged (no payload header)";
        return false;
    }
    size_t pos = magic;
    std::vector<std::pair<std::string, size_t>> entries;
    while (true) {
        const size_t end = std::string_view(data + pos, size - pos).find('\n');
        if (end == std::string_view::npos) {
            error = "the embedded installation data is damaged (header)";
            return false;
        }
        const std::string line(data + pos, end);
        pos += end + 1;
        if (line.empty()) break;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            error = "the embedded installation data is damaged (entry)";
            return false;
        }
        entries.emplace_back(line.substr(tab + 1), static_cast<size_t>(std::stoull(line.substr(0, tab))));
    }
    for (const auto& [path, length] : entries) {
        if (pos + length > size || path.find("..") != std::string::npos) {
            error = "the embedded installation data is damaged (file " + path + ")";
            return false;
        }
        payload.files[path] = std::string(data + pos, length);
        pos += length;
    }
    const auto manifest = parseKeyValues(payload.files["saf-version.txt"]);
    payload.version = manifest.count("version") ? manifest.at("version") : "";
    payload.protocol = manifest.count("protocol") ? manifest.at("protocol") : "";
    payload.vencord = manifest.count("vencord") ? manifest.at("vencord") : "";
    return true;
}

bool loadPayloadFromDirectory(const std::wstring& directory, Payload& payload, std::string& error) {
    const fs::path root(directory);
    std::error_code ec;
    for (const auto& name : { L"saf-audio-host.exe", L"saf-version.txt" }) {
        std::string data;
        if (!readFile(root / name, data)) {
            error = "missing in the payload folder: " + toUtf8(name);
            return false;
        }
        payload.files[toUtf8(name)] = data;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root / L"dist", ec)) {
        if (!entry.is_regular_file() || entry.path().extension() == L".map") continue;
        std::string data;
        if (!readFile(entry.path(), data)) continue;
        std::wstring relative = fs::relative(entry.path(), root).generic_wstring();
        payload.files[toUtf8(relative)] = data;
    }
    if (ec) {
        error = "the payload folder has no dist\\ folder";
        return false;
    }
    if (fs::is_directory(root / L"clean-dist", ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(root / L"clean-dist", ec)) {
            if (!entry.is_regular_file() || entry.path().extension() == L".map") continue;
            std::string data;
            if (!readFile(entry.path(), data)) { error = "could not read clean Vencord payload"; return false; }
            payload.files[toUtf8(fs::relative(entry.path(), root).generic_wstring())] = data;
        }
        if (ec) { error = "could not read clean Vencord payload"; return false; }
    }
    const auto manifest = parseKeyValues(payload.files["saf-version.txt"]);
    payload.version = manifest.count("version") ? manifest.at("version") : "";
    payload.protocol = manifest.count("protocol") ? manifest.at("protocol") : "";
    payload.vencord = manifest.count("vencord") ? manifest.at("vencord") : "";
    return true;
}

// ---------------------------------------------------------------------- status

Status readStatus(const Paths& paths) {
    Status status;
    const fs::path dist = vencordDir(paths) / L"dist";
    std::error_code ec;
    status.vencordPresent = fs::exists(dist / L"patcher.js", ec);
    std::string marker;
    if (readFile(dist / L"saf-version.txt", marker)) {
        status.ourVencord = true;
        const auto values = parseKeyValues(marker);
        if (values.count("version")) status.installedVersion = values.at("version");
    }
    std::string record;
    const bool recorded = readFile(safDir(paths) / kStateFile, record);
    if (status.installedVersion.empty() && recorded) status.installedVersion = parseKeyValues(record)["version"];
    status.hostPresent = fs::exists(safDir(paths) / L"native" / L"saf-audio-host.exe", ec)
        || (!status.installedVersion.empty() && fs::exists(safDir(paths) / L"native" / toWide(status.installedVersion) / L"saf-audio-host.exe", ec));
    status.foreignBackup = fs::exists(vencordDir(paths) / L"dist.saf-backup", ec);
    status.installed = status.ourVencord || recorded || fs::exists(safDir(paths) / L"native", ec);
    return status;
}

bool newerVersion(const std::string& available, const std::string& installed) {
    return versionLess(toWide(installed), toWide(available));
}

// --------------------------------------------------------------------- install

Result install(const Paths& paths, const DiscordInstall& discord, const Payload& payload, const Log& log) {
    Result result;
    Journal journal;
    auto fail = [&](const std::string& error) {
        log("ERROR: " + error);
        journal.rollback(log);
        result.error = error;
        return result;
    };

    // 1. A complete payload from one release.
    for (const char* required : { "dist/patcher.js", "dist/renderer.js", "dist/preload.js", "clean-dist/patcher.js", "clean-dist/renderer.js", "clean-dist/preload.js", "saf-audio-host.exe", "saf-version.txt" }) {
        if (!payload.files.count(required)) return fail(std::string("the installation data is incomplete: ") + required);
    }
    if (payload.version.empty()) return fail("the installation data has no version");
    if (payload.files.at("clean-dist/renderer.js").find("forceHookSoundshare") != std::string::npos) return fail("the clean Vencord payload contains the plugin");
    if (payload.version.find_first_not_of("0123456789.") != std::string::npos) return fail("invalid payload version");
    const Status installed = readStatus(paths);
    if (installed.installed && newerVersion(installed.installedVersion, payload.version)) return fail("a newer version is installed; downgrading is refused");
    if (payload.files.at("dist/renderer.js").find("\"" + payload.version + "\"") == std::string::npos
        || payload.files.at("dist/renderer.js").find("forceHookSoundshare") == std::string::npos) {
        return fail("the Vencord build in the installation data does not contain StreamAudioFilter " + payload.version);
    }
    const auto allDiscords = findDiscordInstalls(paths);
    if (processRunningUnder(discord.root) || std::any_of(allDiscords.begin(), allDiscords.end(), [](const auto& d) { return d.running; })) {
        return fail("Discord is running. Quit it completely (tray icon -> Quit Discord) and try again.");
    }
    log("installing StreamAudioFilter " + payload.version + " (Vencord " + payload.vencord + ") for " + toUtf8(discord.name) + " " + toUtf8(discord.version));

    const fs::path vencord = vencordDir(paths);
    const fs::path dist = vencord / L"dist";
    const fs::path staged = vencord / L"dist.saf-new";
    const fs::path previous = vencord / L"dist.saf-previous";
    const fs::path backup = vencord / L"dist.saf-backup";
    const fs::path nativeRoot = safDir(paths) / L"native";
    const fs::path nativeDir = nativeRoot / toWide(payload.version);
    const fs::path hostExe = nativeDir / L"saf-audio-host.exe";
    const fs::path hostNew = nativeDir / L"saf-audio-host.exe.new";
    const fs::path hostOld = nativeDir / L"saf-audio-host.exe.old";
    const fs::path resources = fs::path(discord.appDir) / L"resources";
    std::error_code ec;
    std::string error;

    const auto oldState = [&] {
        std::string text;
        readFile(safDir(paths) / kStateFile, text);
        return parseKeyValues(text);
    }();

    // 2. Stage the new files next to the old ones.
    fs::remove_all(staged, ec);
    journal.add([staged] { std::error_code e; fs::remove_all(staged, e); });
    for (const auto& [path, data] : payload.files) {
        if (path.rfind("dist/", 0) != 0) continue;
        if (!writeFile(staged / toWide(path.substr(5)), data)) return fail("could not write " + path);
    }
    if (!writeFile(staged / L"saf-version.txt", payload.files.at("saf-version.txt"))) return fail("could not write the version marker");

    if (!writeFile(hostNew, payload.files.at("saf-audio-host.exe"))) return fail("could not write saf-audio-host.exe");
    journal.add([hostNew] { std::error_code e; fs::remove(hostNew, e); });

    // 3. The parts must belong to one release.
    const std::string hostVersion = runVersion(hostNew);
    if (hostVersion != payload.version) {
        return fail("version mismatch: the audio host reports \"" + hostVersion + "\", the plugin is " + payload.version
            + ". The installation data is inconsistent; download the installer again.");
    }
    log("audio host " + hostVersion + " checked");

    // 4. Keep what is there now.
    bool foreignBackup = oldState.count("foreignBackup") && oldState.at("foreignBackup") == "1";
    if (fs::exists(dist, ec)) {
        const bool ours = fs::exists(dist / L"saf-version.txt", ec);
        if (ours) {
            fs::remove_all(previous, ec);
            if (!moveTo(dist, previous, error)) return fail(error);
            journal.add([dist, previous] { std::error_code e; fs::remove_all(dist, e); fs::rename(previous, dist, e); });
        } else {
            if (fs::exists(backup, ec)) {
                // An older backup from an earlier installation: keep it under a dated name.
                const auto archived = vencord / toWide("dist.saf-backup-" + timestamp());
                if (!moveTo(backup, archived, error)) return fail(error);
                journal.add([backup, archived] { std::error_code e; fs::rename(archived, backup, e); });
            }
            if (!moveTo(dist, backup, error)) return fail(error);
            journal.add([dist, backup] { std::error_code e; fs::remove_all(dist, e); fs::rename(backup, dist, e); });
            foreignBackup = true;
            log("the existing Vencord build was kept as dist.saf-backup (restored on uninstall)");
        }
    }

    // 5. Put the new build in place.
    if (!moveTo(staged, dist, error)) return fail(error);
    journal.add([dist, staged] { std::error_code e; fs::rename(dist, staged, e); });

    // 6. The audio host.
    fs::remove(hostOld, ec);
    if (fs::exists(hostExe, ec)) {
        if (!moveTo(hostExe, hostOld, error)) return fail(error + " (is the audio host still running?)");
        journal.add([hostExe, hostOld] { std::error_code e; fs::remove(hostExe, e); fs::rename(hostOld, hostExe, e); });
    }
    if (!moveTo(hostNew, hostExe, error)) return fail(error);
    journal.add([hostExe, hostNew] { std::error_code e; fs::rename(hostExe, hostNew, e); });

    // 7. Vencord's loader in Discord.
    const fs::path appAsar = resources / L"app.asar";
    const fs::path original = resources / L"_app.asar";
    const fs::path patcher = dist / L"patcher.js";
    bool injectedByUs = oldState.count("injectedByUs") && oldState.at("injectedByUs") == "1"
        && oldState.count("discordRoot") && toWide(oldState.at("discordRoot")) == discord.root;
    if (fs::exists(original, ec)) {
        std::string oldLoader;
        readFile(appAsar, oldLoader);
        if (!isLoaderFor(appAsar, patcher)) {
            if (!writeFile(appAsar, makeLoaderAsar(patcher))) return fail("could not update Vencord's loader in " + toUtf8(resources.wstring()));
            journal.add([appAsar, oldLoader] { writeFile(appAsar, oldLoader); });
            log("Vencord's loader now points at " + toUtf8(patcher.wstring()));
        } else {
            log("Vencord is already loaded by this Discord");
        }
    } else if (fs::exists(appAsar, ec)) {
        if (!moveTo(appAsar, original, error)) return fail(error);
        journal.add([appAsar, original] { std::error_code e; fs::remove(appAsar, e); fs::rename(original, appAsar, e); });
        if (!writeFile(appAsar, makeLoaderAsar(patcher))) return fail("could not write Vencord's loader");
        injectedByUs = true;
        log("Vencord's loader was added to " + toUtf8(discord.name) + " " + toUtf8(discord.version));
    } else {
        return fail("unexpected Discord folder layout: no app.asar in " + toUtf8(resources.wstring()));
    }

    // 8. Remember what was done, for repair and removal.
    std::ostringstream state;
    state << "version=" << payload.version << "\n"
          << "protocol=" << payload.protocol << "\n"
          << "vencord=" << payload.vencord << "\n"
          << "discordRoot=" << toUtf8(discord.root) << "\n"
          << "discordAppDir=" << toUtf8(discord.appDir) << "\n"
          << "injectedByUs=" << (injectedByUs ? 1 : 0) << "\n"
          << "foreignBackup=" << (foreignBackup ? 1 : 0) << "\n";
    const auto record = safDir(paths) / kStateFile;
    std::string oldRecord;
    const bool hadRecord = readFile(record, oldRecord);
    journal.add([record, oldRecord, hadRecord] { if (hadRecord) writeFile(record, oldRecord); else { std::error_code e; fs::remove(record, e); } });
    if (!writeFile(record, state.str())) return fail("could not write the installation record");

    // Keep a plain Vencord build for removal when no original build exists.
    const fs::path clean = safDir(paths) / L"clean-dist";
    const fs::path cleanOld = safDir(paths) / L"clean-dist.previous";
    if (payload.files.count("clean-dist/patcher.js")) {
        if (fs::exists(cleanOld, ec)) return fail("unfinished clean Vencord backup; finish recovery before installing");
        if (fs::exists(clean, ec)) {
            if (!moveTo(clean, cleanOld, error)) return fail(error);
            journal.add([clean, cleanOld] { std::error_code e; fs::remove_all(clean, e); fs::rename(cleanOld, clean, e); });
        } else journal.add([clean] { std::error_code e; fs::remove_all(clean, e); });
        for (const auto& [path, data] : payload.files) {
            if (path.rfind("clean-dist/", 0) == 0 && !writeFile(clean / toWide(path.substr(11)), data)) return fail("could not write clean Vencord build");
        }
    }
    journal.commit();
    fs::remove_all(cleanOld, ec);
    fs::remove(nativeRoot / L"streamaudiofilter.node", ec);
    fs::remove(nativeRoot / L"saf-audio-host.exe", ec);
    for (const auto& entry : fs::directory_iterator(nativeRoot, ec)) {
        if (entry.is_directory() && entry.path().filename() != nativeDir.filename()) fs::remove_all(entry.path(), ec);
    }
    fs::remove_all(previous, ec);
    fs::remove(hostOld, ec);
    log("done. Start Discord; StreamAudioFilter is enabled in Settings -> Vencord -> Plugins.");
    result.ok = true;
    return result;
}

// ------------------------------------------------------------------- uninstall

Result uninstall(const Paths& paths, const std::vector<DiscordInstall>& discords, const Log& log, bool keepSettings) {
    Result result;
    Journal journal;
    auto fail = [&](const std::string& error) {
        log("ERROR: " + error); journal.rollback(log); result.error = error; return result;
    };
    for (const auto& discord : discords) {
        if (processRunningUnder(discord.root)) return fail("Discord is running. Quit it completely (tray icon -> Quit Discord) and try again.");
    }
    const fs::path vencord = vencordDir(paths), saf = safDir(paths);
    const fs::path dist = vencord / L"dist", backup = vencord / L"dist.saf-backup";
    const fs::path retired = vencord / L"dist.saf-removing", retiredSaf = vencord / L"streamAudioFilter.removing";
    std::error_code ec;
    std::string error;
    if (fs::exists(retired, ec) || fs::exists(retiredSaf, ec)) return fail("an unfinished removal needs recovery; no files were changed");
    const bool ours = fs::exists(dist / L"saf-version.txt", ec);
    const bool hasBackup = fs::is_directory(backup, ec);
    const fs::path clean = saf / L"clean-dist";
    const bool hasClean = fs::exists(clean / L"patcher.js", ec) && fs::exists(clean / L"renderer.js", ec) && fs::exists(clean / L"preload.js", ec);
    // Never replace a Vencord build the user installed after ours.
    if (ours && !hasBackup && !hasClean) return fail("the original Vencord backup is unavailable. Reinstall using the latest StreamAudioFilter installer, then remove it again. Nothing was removed.");

    // Validate every shared settings file before modifying any files.
    std::vector<std::pair<fs::path, std::string>> settings;
    if (!keepSettings) {
        try {
            for (const auto* name : {L"settings.json", L"native-settings.json"}) {
                const fs::path file = vencord / L"settings" / name;
                std::string text;
                if (fs::exists(file, ec)) {
                    if (!readFile(file, text)) return fail("could not read Vencord settings");
                    settings.emplace_back(file, SettingsJson(text).withoutPlugin());
                }
            }
        } catch (const std::exception& e) { return fail(std::string("settings were not changed: ") + e.what()); }
    }
    if (ours) {
        if (!moveTo(dist, retired, error)) return fail(error);
        journal.add([dist, retired] { std::error_code e; fs::rename(retired, dist, e); });
        const fs::path replacement = hasBackup ? backup : clean;
        if (!moveTo(replacement, dist, error)) return fail(error);
        journal.add([dist, replacement] { std::error_code e; fs::rename(dist, replacement, e); });
        log(hasBackup ? "the original Vencord build was restored" : "plain Vencord was restored; only StreamAudioFilter was removed");
    } else log("the current Vencord build is not owned by StreamAudioFilter and was left unchanged");

    // Quarantine only this plugin's directory; no other plugins or Discord
    // injection files need changing because Vencord remains installed.
    if (fs::exists(saf, ec)) {
        if (!moveTo(saf, retiredSaf, error)) return fail(error);
        journal.add([saf, retiredSaf] { std::error_code e; fs::remove_all(saf, e); fs::rename(retiredSaf, saf, e); });
    } else journal.add([saf] { std::error_code e; fs::remove_all(saf, e); });
    std::string preferences;
    const bool saved = keepSettings && readFile(retiredSaf / L"preferences.json", preferences);
    if (saved) {
        if (!writeFile(saf / L"preferences.json", preferences)) return fail("could not preserve plugin preferences");
    } else if (!keepSettings) {
        // Legacy IndexedDB belongs to Discord and other plugins. Never erase
        // it. This tombstone prevents any old rules being imported again;
        // the plugin deletes its legacy key at its next start.
        if (!writeFile(saf / L"preferences.json", "{\"reset\":true}")) return fail("could not record preference reset");
    }
    for (const auto& [file, replacement] : settings) {
        std::string original;
        if (!readFile(file, original)) return fail("could not read Vencord settings");
        journal.add([file, original] { writeFile(file, original); });
        if (!writeFile(file, replacement)) return fail("could not update Vencord settings");
    }
    journal.commit();
    fs::remove_all(retired, ec);
    if (ec) { result.error = "plugin was removed but old bundle cleanup failed: " + ec.message(); log("ERROR: " + result.error); return result; }
    fs::remove_all(retiredSaf, ec);
    if (ec) { result.error = "plugin was removed but old host cleanup failed: " + ec.message(); log("ERROR: " + result.error); return result; }
    log(keepSettings ? "plugin removed; settings preserved. Restart Discord." : "plugin files and settings removed. Legacy Discord database entries, if any, are cleared on the plugin's next start. Restart Discord.");
    result.ok = true;
    return result;
}

} // namespace saf::setup
