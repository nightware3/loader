/*
    © 2026 Modular Content do not share, re-distribute or modify
    without permission of its author (admin@modularcontent.dev).
*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <shellapi.h>
#include <string>
#include <thread>
#include <optional>
#include <memory>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <tlhelp32.h>
#include <vector>
#include <atomic>
#include <shlobj.h>
#include <filesystem>
#include <system_error>

#include "resource.h"
#include "injector.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

constexpr UINT WM_API_RESULT = WM_APP + 1;
constexpr UINT_PTR TIMER_ID = 1;
constexpr UINT TIMER_INTERVAL = 8;

enum class AppState {
    Checking,
    Error,
    ApprovedFadeOut,
    ApprovedWait,
    ApprovedFadeIn,
    Ready,
    Downloading,
    DownloadError,
    UnsupportedGame
};

bool g_closeHovered = false;
bool g_discordHovered = false;
bool g_launchHovered = false;

enum class Game {
    GarrysMod,
    TeamFortress2
};

struct GameProcess {
    Game game;
    DWORD pid;
};

HWND g_gameCombo{};
std::vector<GameProcess> g_gameProcesses;
std::wstring g_processSignature;
std::chrono::steady_clock::time_point g_lastProcessScan{};
bool g_gameInterfaceVisible = false;

enum class DownloadState {
    Idle,
    Downloading,
    Completed,
    Failed,
    Unsupported
};

DownloadState g_downloadState = DownloadState::Idle;
bool g_gameDropdownOpen = false;
int g_gameHoveredIndex = -1;
Game g_selectedGame = Game::GarrysMod;
std::atomic_bool g_pendingLoading{ false };

constexpr int GAME_COMBO_Y = 280;
constexpr int GAME_COMBO_W = 280;
constexpr int GAME_COMBO_H = 22;
constexpr int GAME_OPTION_H = 22;

constexpr int LAUNCH_GAP = 18;
constexpr int LAUNCH_H = 26;

int GameComboX(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    return ((client.right - client.left) - GAME_COMBO_W) / 2;
}

RECT GetLaunchButtonRect(HWND window) {
    RECT client{};
    GetClientRect(window, &client);

    const int width = GAME_COMBO_W;
    const int height = LAUNCH_H;
    const int x = (client.right - client.left - width) / 2;
    const int y = GAME_COMBO_Y + GAME_COMBO_H + LAUNCH_GAP;

    return RECT{
        x,
        y,
        x + width,
        y + height
    };
}

bool IsPointInsideRect(const POINT& point, const RECT& rect) {
    return point.x >= rect.left &&
        point.x < rect.right &&
        point.y >= rect.top &&
        point.y < rect.bottom;
}

std::chrono::steady_clock::time_point g_animationStart =
std::chrono::steady_clock::now();

constexpr wchar_t DISCORD_URL[] = L"https://discord.gg/bMbT9CPYVm";

struct InternetHandle {
    HINTERNET value{};

    explicit InternetHandle(HINTERNET handle = nullptr) : value(handle) {}

    ~InternetHandle() {
        if (value) {
            WinHttpCloseHandle(value);
        }
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    explicit operator bool() const {
        return value != nullptr;
    }
};

HWND g_window{};
std::wstring g_api_url;
std::thread g_worker;
AppState g_state = AppState::Checking;
std::optional<int> g_progress;
float g_angle = 0.0f;
ULONG_PTR g_gdiplus_token{};

std::vector<GameProcess> FindGameProcesses() {
    std::vector<GameProcess> result;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"gmod.exe") == 0) {
                result.push_back({ Game::GarrysMod, entry.th32ProcessID });
            }
            else if (_wcsicmp(entry.szExeFile, L"tf_win64.exe") == 0) {
                result.push_back({ Game::TeamFortress2, entry.th32ProcessID });
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

const GameProcess* FindDetectedGame() {
    if (g_gameProcesses.empty()) {
        return nullptr;
    }

    return &g_gameProcesses.front();
}

const wchar_t* GameName(Game game) {
    return game == Game::GarrysMod
        ? L"Garry's Mod"
        : L"Team Fortress 2";
}

std::wstring GetDllUrl(Game game)
{
    const wchar_t* filename =
        game == Game::GarrysMod
        ? L"nightware3-gmod.dll"
        : L"nightware3-tf2.dll";

    return std::wstring(
        L"https://cdn.modularcontent.dev/files/"
    ) + filename;
}

std::filesystem::path GetDllPath(Game game)
{
    PWSTR localAppData = nullptr;

    if (FAILED(SHGetKnownFolderPath(
        FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
    {
        return {};
    }

    std::filesystem::path folder =
        std::filesystem::path(localAppData)
        / L"ModularContent"
        / L"Nightware";

    CoTaskMemFree(localAppData);

    const wchar_t* filename =
        game == Game::GarrysMod
        ? L"nightware3-gmod.dll"
        : L"nightware3-tf2.dll";

    return folder / filename;
}


bool IsDllPresent(Game game)
{
    const auto path = GetDllPath(game);

    if (path.empty())
        return false;

    std::error_code ec;

    return std::filesystem::is_regular_file(path, ec)
        && !ec
        && std::filesystem::file_size(path, ec) > 0
        && !ec;
}

bool GetRemoteDllETag(Game game, std::wstring& etag)
{
    const auto url = GetDllUrl(game);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = DWORD(-1);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return false;

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);

    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    HINTERNET session = WinHttpOpen(
        L"Nightware Updater",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!session)
        return false;

    HINTERNET connection =
        WinHttpConnect(session, host.c_str(), parts.nPort, 0);

    HINTERNET request = connection
        ? WinHttpOpenRequest(
            connection,
            L"HEAD",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            parts.nScheme == INTERNET_SCHEME_HTTPS
            ? WINHTTP_FLAG_SECURE : 0)
        : nullptr;

    bool success = false;

    if (request &&
        WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) &&
        WinHttpReceiveResponse(request, nullptr))
    {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);

        if (WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) &&
            status == 200)
        {
            wchar_t buffer[512]{};
            DWORD bufferSize = sizeof(buffer);

            if (WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_ETAG,
                WINHTTP_HEADER_NAME_BY_INDEX,
                buffer,
                &bufferSize,
                WINHTTP_NO_HEADER_INDEX))
            {
                etag = buffer;
                success = !etag.empty();
            }
        }
    }

    if (request)
        WinHttpCloseHandle(request);

    if (connection)
        WinHttpCloseHandle(connection);

    WinHttpCloseHandle(session);

    return success;
}

bool IsDllUpdateAvailable(Game game)
{
    if (!IsDllPresent(game))
        return true;

    std::wstring remoteETag;

    if (!GetRemoteDllETag(game, remoteETag))
        return false;

    auto versionPath = GetDllPath(game);
    versionPath += L".etag";

    std::wstring localETag;
    std::wifstream file(versionPath);

    if (file)
        std::getline(file, localETag);

    return localETag.empty() || localETag != remoteETag;
}

bool DownloadDll(Game game)
{
    const std::wstring url = GetDllUrl(game);
    const std::filesystem::path output = GetDllPath(game);

    if (output.empty())
        return false;

    try
    {
        std::filesystem::create_directories(output.parent_path());
    }
    catch (...)
    {
        return false;
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = DWORD(-1);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        return false;

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);

    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    HINTERNET session = WinHttpOpen(
        L"Nightware Updater",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!session)
        return false;

    HINTERNET connection =
        WinHttpConnect(session, host.c_str(), parts.nPort, 0);

    HINTERNET request = connection
        ? WinHttpOpenRequest(
            connection,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            parts.nScheme == INTERNET_SCHEME_HTTPS
            ? WINHTTP_FLAG_SECURE : 0)
        : nullptr;

    bool success = false;
    std::wstring downloadedETag;

    if (request &&
        WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) &&
        WinHttpReceiveResponse(request, nullptr))
    {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);

        if (WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) &&
            status == 200)
        {
            wchar_t buffer[512]{};
            DWORD bufferSize = sizeof(buffer);

            if (WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_ETAG,
                WINHTTP_HEADER_NAME_BY_INDEX,
                buffer,
                &bufferSize,
                WINHTTP_NO_HEADER_INDEX))
            {
                downloadedETag = buffer;
            }

            auto temp = output;
            temp += L".part";

            std::ofstream file(temp, std::ios::binary | std::ios::trunc);

            if (file)
            {
                char buffer[16384];
                DWORD bytesRead = 0;
                success = true;

                for (;;)
                {
                    if (!WinHttpReadData(
                        request,
                        buffer,
                        sizeof(buffer),
                        &bytesRead))
                    {
                        success = false;
                        break;
                    }

                    if (bytesRead == 0)
                        break;

                    file.write(
                        buffer,
                        static_cast<std::streamsize>(bytesRead));

                    if (!file)
                    {
                        success = false;
                        break;
                    }
                }

                file.close();

                if (file.fail())
                    success = false;

                if (success)
                {
                    std::error_code ec;
                    std::filesystem::rename(temp, output, ec);

                    if (ec)
                    {
                        ec.clear();
                        std::filesystem::remove(output, ec);

                        if (!ec)
                        {
                            ec.clear();
                            std::filesystem::rename(temp, output, ec);
                        }
                    }

                    if (ec)
                        success = false;
                }

                if (!success)
                {
                    std::error_code ec;
                    std::filesystem::remove(temp, ec);
                }
            }
        }
    }

    if (request)
        WinHttpCloseHandle(request);

    if (connection)
        WinHttpCloseHandle(connection);

    WinHttpCloseHandle(session);

    if (success && !downloadedETag.empty())
    {
        auto versionPath = output;
        versionPath += L".etag";

        std::wofstream file(versionPath, std::ios::trunc);

        if (file)
            file << downloadedETag << L'\n';
    }

    return success;
}

const wchar_t* GameExe(Game game) {
    return game == Game::GarrysMod
        ? L"gmod.exe"
        : L"tf2.exe";
}

DWORD FindGamePid(Game game) {
    for (const auto& process : g_gameProcesses) {
        if (process.game == game) {
            return process.pid;
        }
    }

    return 0;
}

void OnGameStarted(Game game, DWORD pid) {
    OutputDebugStringW(L"Game started.\n");

    const char* procName = game == Game::GarrysMod
        ? "gmod.exe"
        : "tf_win64.exe";

    std::filesystem::path dllPath = GetDllPath(game);

    if (dllPath.empty()) {
        MessageBoxW(
            g_window,
            L"GetDllPath returned an empty path.",
            L"Path Error",
            MB_ICONERROR | MB_OK
        );
        g_pendingLoading = false;
        return;
    }

    std::error_code ec;
    bool exists = std::filesystem::is_regular_file(dllPath, ec);

    if (!exists) {
        std::wstring message =
            L"DLL not found:\n" + dllPath.wstring();

        if (ec) {
            message += L"\n\nFilesystem error: ";
            message += std::to_wstring(ec.value());
            message += L" - ";
            message += std::wstring(
                ec.message().begin(),
                ec.message().end()
            );
        }

        MessageBoxW(
            g_window,
            message.c_str(),
            L"Path Error",
            MB_ICONERROR | MB_OK
        );

        g_pendingLoading = false;
        return;
    }

    if (!Injector::Inject(procName, dllPath.string().c_str())) {
    }

    g_pendingLoading = false;
}

bool IsAlreadyInjected(Game game) {
	DWORD pid = FindGamePid(game);
	if (pid == 0) {
		return false;
	}
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return false;
	}
	MODULEENTRY32W moduleEntry{};
	moduleEntry.dwSize = sizeof(moduleEntry);
	bool injected = false;
	if (Module32FirstW(snapshot, &moduleEntry)) {
		do {
			const wchar_t* targetModule = game == Game::GarrysMod
				? L"nightware3-gmod.dll"
				: L"nightware3-tf2.dll";
			if (_wcsicmp(moduleEntry.szModule, targetModule) == 0) {
				injected = true;
				break;
			}
		} while (Module32NextW(snapshot, &moduleEntry));
	}
	CloseHandle(snapshot);
	return injected;
}

void LaunchGame(Game game) {
	if (IsAlreadyInjected(game)) {
		MessageBoxW(
			g_window,
			L"The DLL is already injected into the game process.",
			L"Injection Error",
			MB_ICONERROR | MB_OK
		);
		return;
	}
    if (g_pendingLoading) return;
    g_pendingLoading = true;
    const int delay = 5000;
    if (FindGamePid(game) != 0) {
        std::thread([game, delay]() {
            //std::this_thread::sleep_for(
            //    std::chrono::milliseconds(delay)
            //);

            OnGameStarted(game, FindGamePid(game));

            g_pendingLoading = false;
            }).detach();
        return;
    }

    const wchar_t* appId = game == Game::GarrysMod
        ? L"4000"
        : L"440";

    std::wstring uri = L"steam://run/";
    uri += appId;

    ShellExecuteW(
        nullptr,
        L"open",
        uri.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL
    );

    std::thread([game, delay]() {
        while (true) {
            const auto processes = FindGameProcesses();

            const auto it = std::find_if(
                processes.begin(),
                processes.end(),
                [game](const GameProcess& process) {
                    return process.game == game;
                }
            );

            if (it != processes.end()) {
                const DWORD pid = it->pid;

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(delay)
                );

                OnGameStarted(game, pid);
                g_pendingLoading = false;
                return;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(250)
            );
        }
        }).detach();
}

void RefreshGameList() {
    g_gameProcesses = FindGameProcesses();

    if (g_window) {
        InvalidateRect(g_window, nullptr, FALSE);
    }
}

float AnimationProgress(int duration) {
    const float elapsed =
        std::chrono::duration<float>(
            std::chrono::steady_clock::now() - g_animationStart
        ).count();

    return std::clamp(elapsed * 1000.0f / duration, 0.0f, 1.0f);
}

float EaseOutCubic(float value) {
    const float inverse = 1.0f - value;
    return 1.0f - inverse * inverse * inverse;
}

float GameInterfaceProgress() {
    if (g_state == AppState::Ready) {
        return 1.0f;
    }

    if (g_state != AppState::ApprovedFadeIn) {
        return 0.0f;
    }

    return EaseOutCubic(AnimationProgress(480));
}

using json = nlohmann::json;

bool CheckApi(const std::wstring& url, DWORD& result) {
    result = ERROR_WINHTTP_CANNOT_CONNECT;

    if (url.empty()) {
        result = ERROR_INVALID_PARAMETER;
        return false;
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        result = GetLastError();
        return false;
    }

    if (parts.nScheme != INTERNET_SCHEME_HTTP &&
        parts.nScheme != INTERNET_SCHEME_HTTPS) {
        result = ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
        return false;
    }

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);

    if (path.empty())
        path = L"/";

    if (parts.dwExtraInfoLength != 0)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    InternetHandle session(
        WinHttpOpen(
            L"ModularContent-Status/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        )
    );

    if (!session) {
        result = GetLastError();
        return false;
    }

    if (!WinHttpSetTimeouts(session.value, 3000, 3000, 3000, 3000)) {
        result = GetLastError();
        return false;
    }

    InternetHandle connection(
        WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)
    );

    if (!connection) {
        result = GetLastError();
        return false;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS
        ? WINHTTP_FLAG_SECURE
        : 0;

    InternetHandle request(
        WinHttpOpenRequest(
            connection.value,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags
        )
    );

    if (!request) {
        result = GetLastError();
        return false;
    }

    if (!WinHttpSendRequest(
        request.value,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0)) {
        result = GetLastError();
        return false;
    }

    if (!WinHttpReceiveResponse(request.value, nullptr)) {
        result = GetLastError();
        return false;
    }

    DWORD httpStatus{};
    DWORD statusSize = sizeof(httpStatus);

    if (!WinHttpQueryHeaders(
        request.value,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &httpStatus,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX)) {
        result = GetLastError();
        return false;
    }

    result = httpStatus;

    if (httpStatus < 200 || httpStatus >= 300)
        return false;

    std::string body;

    for (;;) {
        DWORD available = 0;

        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            result = GetLastError();
            return false;
        }

        if (available == 0)
            break;

        std::string chunk(available, '\0');
        DWORD received = 0;

        if (!WinHttpReadData(
            request.value,
            chunk.data(),
            available,
            &received)) {
            result = GetLastError();
            return false;
        }

        if (received == 0)
            break;

        body.append(chunk.data(), received);
    }

    try {
        const auto data = json::parse(body);

        if (!data.contains("components") ||
            !data["components"].is_array()) {
            return false;
        }

        for (const auto& component : data["components"]) {
            if (!component.is_object())
                continue;

            if (component.value("id", std::string{}) != "cdn")
                continue;

            return component.value("status", std::string{}) == "operational";
        }
    }
    catch (const json::exception&) {
        return false;
    }

    return false;
}

void StartApiCheck(HWND window) {
    g_worker = std::thread([window, url = g_api_url]() {
        DWORD result{};
        const bool success = CheckApi(url, result);

        PostMessageW(
            window,
            WM_API_RESULT,
            success ? TRUE : FALSE,
            static_cast<LPARAM>(result)
        );
        });
}

void DrawCenteredText(
    Graphics& graphics,
    const std::wstring& text,
    REAL x,
    REAL y,
    REAL width,
    REAL height,
    REAL font_size,
    Color color
) {
    FontFamily family(L"Segoe UI");
    Font font(&family, font_size, FontStyleRegular, UnitPixel);
    SolidBrush brush(color);
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);

    RectF rect(x, y, width, height);
    graphics.DrawString(
        text.c_str(),
        static_cast<INT>(text.size()),
        &font,
        rect,
        &format,
        &brush
    );
}

int GameOptionCount() {
    return 2;
}

void DrawLaunchButton(Graphics& graphics, int width) {
    const float progress = GameInterfaceProgress();

    if (progress <= 0.0f) {
        return;
    }

    const BYTE alpha = static_cast<BYTE>(255.0f * progress);

    const REAL buttonWidth = GAME_COMBO_W;
    const REAL buttonHeight = 26.0f;
    const REAL buttonX = (width - buttonWidth) / 2.0f;
    const REAL buttonY = static_cast<REAL>(
        GAME_COMBO_Y + GAME_COMBO_H + LAUNCH_GAP
        );

    SolidBrush buttonBrush(
        g_launchHovered
        ? Color(alpha, 9, 131, 216)
        : Color(alpha, 41, 41, 45)
    );

    graphics.FillRectangle(
        &buttonBrush,
        buttonX,
        buttonY,
        buttonWidth,
        buttonHeight
    );

    std::wstring buttonText;

    if (g_downloadState == DownloadState::Downloading)
    {
        buttonText = L"Downloading...";
    }
    else if (g_downloadState == DownloadState::Unsupported)
    {
        buttonText = L"No version available";
    }
    else if (g_downloadState == DownloadState::Failed)
    {
        buttonText = L"Download failed";
    }
    else if (!IsDllPresent(g_selectedGame))
    {
        buttonText = L"Download";
    }
	else if (FindDetectedGame())
	{
		buttonText = L"Inject";
	}
    else
    {
        buttonText = L"Launch";
    }

    DrawCenteredText(
        graphics,
        buttonText,
        buttonX,
        buttonY,
        buttonWidth,
        buttonHeight,
        14.0f,
        Color(alpha, 255, 255, 255)
    );
}

Game GameAtIndex(int index) {
    return index == 0
        ? Game::GarrysMod
        : Game::TeamFortress2;
}

std::wstring GameOptionText(Game game) {
    return GameName(game);
}

void DrawGameCombo(
    Graphics& graphics,
    int width,
    int height
) {
    const float progress = GameInterfaceProgress();

    if (progress <= 0.0f) {
        return;
    }

    const BYTE alpha = static_cast<BYTE>(255.0f * progress);

    const GameProcess* detected = FindDetectedGame();

    if (detected) {
        const REAL x = static_cast<REAL>(GameComboX(g_window));
        const REAL y = static_cast<REAL>(GAME_COMBO_Y);

        SolidBrush background(Color(alpha, 31, 31, 35));
        graphics.FillRectangle(
            &background,
            x,
            y,
            static_cast<REAL>(GAME_COMBO_W),
            static_cast<REAL>(GAME_COMBO_H)
        );

        const std::wstring text =
            std::wstring(GameExe(detected->game)) +
            L" [" + std::to_wstring(detected->pid) + L"]";

        FontFamily family(L"Segoe UI");
        Font font(&family, 13.0f, FontStyleRegular, UnitPixel);
        SolidBrush textBrush(Color(alpha, 255, 255, 255));

        StringFormat format;
        format.SetAlignment(StringAlignmentNear);
        format.SetLineAlignment(StringAlignmentCenter);
        format.SetTrimming(StringTrimmingEllipsisCharacter);

        RectF textRect(
            x + 4.0f,
            y,
            static_cast<REAL>(GAME_COMBO_W) - 36.0f,
            static_cast<REAL>(GAME_COMBO_H)
        );

        graphics.DrawString(
            text.c_str(),
            -1,
            &font,
            textRect,
            &format,
            &textBrush
        );

        return;
    }

    const Color background(alpha, 31, 31, 35);
    const Color arrowBackground(alpha, 41, 41, 45);
    const Color hoverBackground(alpha, 9, 131, 216);
    const Color foreground(alpha, 255, 255, 255);
    const Color secondary(alpha, 165, 165, 175);

    const REAL x = static_cast<REAL>(GameComboX(g_window));
    const REAL y = static_cast<REAL>(GAME_COMBO_Y);
    const REAL w = static_cast<REAL>(GAME_COMBO_W);
    const REAL h = static_cast<REAL>(GAME_COMBO_H);
    const REAL arrowWidth = 24.0f;

    SolidBrush mainBrush(background);
    SolidBrush arrowBrush(arrowBackground);

    graphics.FillRectangle(&mainBrush, x, y, w, h);

    graphics.FillRectangle(
        &arrowBrush,
        x + w - arrowWidth,
        y,
        arrowWidth,
        h
    );

    Pen arrowPen(foreground, 1.8f);
    arrowPen.SetStartCap(LineCapRound);
    arrowPen.SetEndCap(LineCapRound);

    const REAL centerX = x + w - arrowWidth / 2.0f;
    const REAL centerY = y + h / 2.0f;

    if (g_gameDropdownOpen) {
        graphics.DrawLine(
            &arrowPen,
            centerX - 4.0f,
            centerY + 2.0f,
            centerX,
            centerY - 2.0f
        );

        graphics.DrawLine(
            &arrowPen,
            centerX,
            centerY - 2.0f,
            centerX + 4.0f,
            centerY + 2.0f
        );
    }
    else {
        graphics.DrawLine(
            &arrowPen,
            centerX - 4.0f,
            centerY - 2.0f,
            centerX,
            centerY + 2.0f
        );

        graphics.DrawLine(
            &arrowPen,
            centerX,
            centerY + 2.0f,
            centerX + 4.0f,
            centerY - 2.0f
        );
    }

    FontFamily family(L"Segoe UI");
    Font font(&family, 13.0f, FontStyleRegular, UnitPixel);
    SolidBrush textBrush(foreground);
    StringFormat format;
    format.SetAlignment(StringAlignmentNear);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);

    RectF textRect(
        x + 4.0f,
        y,
        w - arrowWidth - 20.0f,
        h
    );

    graphics.DrawString(
        GameOptionText(g_selectedGame).c_str(),
        -1,
        &font,
        textRect,
        &format,
        &textBrush
    );

    if (!g_gameDropdownOpen) {
        return;
    }

    const REAL listY = y + h;

    for (int i = 0; i < GameOptionCount(); ++i) {
        const REAL optionY =
            listY + static_cast<REAL>(i * GAME_OPTION_H);

        const Game game = GameAtIndex(i);

        SolidBrush optionBrush(
            g_gameHoveredIndex == i
            ? hoverBackground
            : background
        );

        graphics.FillRectangle(
            &optionBrush,
            x,
            optionY,
            w,
            static_cast<REAL>(GAME_OPTION_H)
        );

        DrawCenteredText(
            graphics,
            GameOptionText(game),
            x + 12.0f,
            optionY,
            w - 24.0f,
            static_cast<REAL>(GAME_OPTION_H),
            13.0f,
            foreground
        );
    }
}


void DrawReadyDecor(Graphics& graphics, int width, int height) {
    const REAL x = 24.0f;
    const REAL y = 43.0f;
    const REAL lineWidth = static_cast<REAL>(width) - 48.0f;

    const float time = static_cast<float>(
        GetTickCount64() % 2400
        ) / 2400.0f;

    const REAL center = time * (lineWidth + 100.0f) - 50.0f;
    const REAL sweepWidth = 100.0f;

    Pen basePen(Color(100, 9, 65, 130), 1.0f);
    graphics.DrawLine(&basePen, x, y, x + lineWidth, y);

    for (int i = 12; i >= 1; --i) {
        const REAL spread = static_cast<REAL>(i) * 2.0f;
        const BYTE alpha = static_cast<BYTE>(4 + (12 - i) * 2);

        Pen glowPen(Color(alpha, 0, 110, 255), 1.0f + spread * 0.12f);

        graphics.DrawLine(
            &glowPen,
            std::max(x, x + center - sweepWidth / 2.0f - spread),
            y,
            std::min(x + lineWidth, x + center + sweepWidth / 2.0f + spread),
            y
        );
    }

    for (int i = 0; i < 100; ++i) {
        const REAL t = static_cast<REAL>(i) / 99.0f;
        const REAL offset = (t - 0.5f) * sweepWidth;
        const REAL px = x + center + offset;

        if (px < x || px > x + lineWidth) {
            continue;
        }

        const REAL intensity = 1.0f - std::abs(t - 0.5f) * 2.0f;
        const BYTE alpha = static_cast<BYTE>(intensity * 245.0f);

        Pen shinePen(
            Color(alpha, 30, 150 + static_cast<int>(105.0f * intensity), 255),
            1.5f
        );

        graphics.DrawLine(&shinePen, px, y, px + 1.0f, y);
    }
}

BOOL IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(
        &ntAuthority,
        2,
        SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0,
        &adminGroup
    )) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    return isAdmin;
}

void DrawInterface(HDC target, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    BYTE contentAlpha = 255;

    if (g_state == AppState::ApprovedFadeOut) {
        contentAlpha = static_cast<BYTE>(
            255.0f * (1.0f - EaseOutCubic(AnimationProgress(400)))
            );
    }

    if (g_state == AppState::ApprovedFadeIn) {
        contentAlpha = static_cast<BYTE>(
            255.0f * EaseOutCubic(AnimationProgress(480))
            );
    }

    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);

    if (!buffer || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);

    {
        Graphics graphics(buffer);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        SolidBrush background(Color(255, 17, 17, 18));
        graphics.FillRectangle(&background, 0, 0, width, height);

        Pen border(Color(255, 78, 78, 78), 1.0f);
        graphics.DrawRectangle(&border, 0, 0, width - 1, height - 1);

        DrawCenteredText(
            graphics,
            L"nightware\u00B3 Loader",
            12.0f, 0.0f,
            static_cast<REAL>(width) - 24.0f,
            36.0f,
            13.0f,
            Color(255, 225, 225, 230)
        );

        DrawCenteredText(
            graphics,
            L"\u00D7",
            static_cast<REAL>(width) - 36.0f,
            0.0f,
            28.0f,
            36.0f,
            20.0f,
            g_closeHovered
            ? Color(255, 255, 75, 75)
            : Color(255, 190, 190, 195)
        );

        if (g_state == AppState::Checking ||
            g_state == AppState::ApprovedFadeOut) {
            const REAL diameter = 42.0f;
            const REAL x = (width - diameter) / 2.0f;
            const REAL y = (height - diameter) / 2.0f - 28.0f;

            Pen spinner(Color(contentAlpha, 9, 131, 216), 3.5f);
            spinner.SetStartCap(LineCapRound);
            spinner.SetEndCap(LineCapRound);

            graphics.DrawArc(
                &spinner,
                x,
                y,
                diameter,
                diameter,
                g_angle,
                180.0f
            );

            const int dots = static_cast<int>(
                GetTickCount64() / 400 % 3
                ) + 1;

            std::wstring status = L"Fetching API status";
            status.append(dots, L'.');

            DrawCenteredText(
                graphics,
                status,
                20.0f,
                y + diameter + 12.0f,
                static_cast<REAL>(width) - 40.0f,
                24.0f,
                13.0f,
                Color(contentAlpha, 220, 220, 225)
            );
        }

        if (g_state == AppState::Error) {
            const REAL size = 28.0f;
            const REAL x = (width - size) / 2.0f;
            const REAL y = (height - size) / 2.0f - 22.0f;

            Pen cross(Color(255, 244, 83, 91), 3.0f);
            cross.SetStartCap(LineCapRound);
            cross.SetEndCap(LineCapRound);

            graphics.DrawLine(&cross, x, y, x + size, y + size);
            graphics.DrawLine(&cross, x + size, y, x, y + size);

            std::wstring errorText = L"API unavailable";
            
            if (!IsRunningAsAdmin())
			{
				errorText = L"Please run the loader as administrator";
			}

            DrawCenteredText(
                graphics,
                errorText,
                20.0f,
                y + size + 12.0f,
                static_cast<REAL>(width) - 40.0f,
                25.0f,
                14.0f,
                Color(contentAlpha, 225, 225, 230)
            );
        }

        if (g_state == AppState::ApprovedFadeIn ||
            g_state == AppState::Ready) {
            const REAL buttonWidth = 64.0f;
            const REAL buttonHeight = 24.0f;
            const REAL buttonX = 12.0f;
            const REAL buttonY = static_cast<REAL>(height) - 36.0f;

            const BYTE alpha = g_state == AppState::Ready
                ? 255
                : static_cast<BYTE>(
                    255.0f * EaseOutCubic(AnimationProgress(450))
                    );

            const Color buttonColor = g_discordHovered
                ? Color(alpha, 9, 131, 216)
                : Color(alpha, 41, 41, 45);

            SolidBrush buttonBrush(buttonColor);
            Pen buttonBorder(Color(alpha, 78, 78, 78), 1.0f);

            RectF buttonRect(
                buttonX, buttonY, buttonWidth, buttonHeight
            );

            graphics.FillRectangle(&buttonBrush, buttonRect);
            //graphics.DrawRectangle(&buttonBorder, buttonRect);

            DrawCenteredText(
                graphics,
                L"Discord",
                buttonX,
                buttonY + 1.0f,
                buttonWidth,
                buttonHeight,
                14.0f,
                Color(alpha, 255, 255, 255)
            );
        }

        if (g_state == AppState::ApprovedFadeIn ||
            g_state == AppState::Ready) {
            const BYTE alpha = g_state == AppState::Ready
                ? 255
                : static_cast<BYTE>(
                    255.0f * EaseOutCubic(AnimationProgress(480))
                    );

            if (g_state == AppState::ApprovedFadeIn ||
                g_state == AppState::Ready) {
                DrawReadyDecor(graphics, width, height);
                DrawLaunchButton(graphics, width);
                DrawGameCombo(graphics, width, height);
            }
        }
    }

    BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);

    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
}

LRESULT CALLBACK WindowProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) {
    if (message == WM_NCHITTEST) {
        POINT point{
            GET_X_LPARAM(lparam),
            GET_Y_LPARAM(lparam)
        };

        ScreenToClient(window, &point);

        RECT client{};
        GetClientRect(window, &client);

        if (point.x >= client.right - 40 &&
            point.y >= 0 &&
            point.y < 36) {
            return HTCLIENT;
        }

        if ((g_state == AppState::ApprovedFadeIn ||
            g_state == AppState::Ready) &&
            point.x >= 16 &&
            point.x < 148 &&
            point.y >= client.bottom - 46 &&
            point.y < client.bottom - 16) {
            return HTCLIENT;
        }

        if (g_state == AppState::Ready) {
            const int listBottom =
                GAME_COMBO_Y + GAME_COMBO_H +
                (g_gameDropdownOpen
                    ? GameOptionCount() * GAME_OPTION_H
                    : 0);

            if (point.x >= GameComboX(window) &&
                point.x < GameComboX(window) + GAME_COMBO_W &&
                point.y >= GAME_COMBO_Y &&
                point.y < listBottom) {
                return HTCLIENT;
            }
        }

        if (g_state == AppState::Ready ||
            g_state == AppState::ApprovedFadeIn) {
            const RECT launchRect = GetLaunchButtonRect(window);

            if (IsPointInsideRect(point, launchRect)) {
                return HTCLIENT;
            }
        }

        return HTCAPTION;
    }

    if (message == WM_LBUTTONUP) {
        POINT point{
            GET_X_LPARAM(lparam),
            GET_Y_LPARAM(lparam)
        };

        RECT client{};
        GetClientRect(window, &client);

        const bool insideCombo =
            point.x >= GameComboX(window) &&
            point.x < GameComboX(window) + GAME_COMBO_W &&
            point.y >= GAME_COMBO_Y &&
            point.y < GAME_COMBO_Y + GAME_COMBO_H;

        const int listTop = GAME_COMBO_Y + GAME_COMBO_H;

        const bool insideList =
            g_gameDropdownOpen &&
            point.x >= GameComboX(window) &&
            point.x < GameComboX(window) + GAME_COMBO_W &&
            point.y >= listTop &&
            point.y < listTop + GameOptionCount() * GAME_OPTION_H;

        if (insideCombo) {
            g_gameDropdownOpen = !g_gameDropdownOpen;
            g_gameHoveredIndex = -1;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        if (insideList) {
            const int index =
                (point.y - listTop) / GAME_OPTION_H;

            if (index >= 0 && index < GameOptionCount()) {
                g_selectedGame = GameAtIndex(index);
                g_downloadState = IsDllPresent(g_selectedGame)
                    ? DownloadState::Completed
                    : DownloadState::Idle;
                g_gameDropdownOpen = false;
                g_gameHoveredIndex = -1;

                InvalidateRect(window, nullptr, FALSE);
            }

            return 0;
        }

        const RECT launchRect = GetLaunchButtonRect(window);


        if (g_state == AppState::Ready &&
            IsPointInsideRect(point, launchRect))
        {
            RefreshGameList();

            if (IsDllUpdateAvailable(g_selectedGame))
            {
                g_downloadState = DownloadState::Downloading;
                InvalidateRect(window, nullptr, FALSE);

                const bool success = DownloadDll(g_selectedGame);

                g_downloadState = success
                    ? DownloadState::Completed
                    : DownloadState::Failed;

                InvalidateRect(window, nullptr, FALSE);

                if (!success)
                    return 0;
            }

            LaunchGame(g_selectedGame);
            return 0;
        }


        if (g_gameDropdownOpen) {
            g_gameDropdownOpen = false;
            g_gameHoveredIndex = -1;
            InvalidateRect(window, nullptr, FALSE);
        }

        if (point.y >= 0 && point.y < 36 &&
            point.x >= client.right - 40) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        }

        if ((g_state == AppState::ApprovedFadeIn ||
            g_state == AppState::Ready) &&
            point.x >= 16 &&
            point.x < 148 &&
            point.y >= client.bottom - 46 &&
            point.y < client.bottom - 16) {
            ShellExecuteW(
                nullptr,
                L"open",
                DISCORD_URL,
                nullptr,
                nullptr,
                SW_SHOWNORMAL
            );
            return 0;
        }
    }

    switch (message) {
    case WM_CREATE:
        SetTimer(window, TIMER_ID, TIMER_INTERVAL, nullptr);
        StartApiCheck(window);
        return 0;

    case WM_TIMER:
        if (wparam == TIMER_ID) {
            const auto now = std::chrono::steady_clock::now();

            if (g_state == AppState::Checking ||
                g_state == AppState::ApprovedFadeOut) {
                static auto previous = now;

                const float delta = std::chrono::duration<float>(
                    now - previous
                ).count();

                previous = now;
                g_angle = std::fmod(g_angle + 270.0f * delta, 360.0f);
            }

            if (g_state == AppState::ApprovedFadeOut &&
                AnimationProgress(400) >= 1.0f) {
                g_state = AppState::ApprovedWait;
                g_animationStart = now;
            }
            else if (g_state == AppState::ApprovedWait &&
                AnimationProgress(250) >= 1.0f) {
                g_state = AppState::ApprovedFadeIn;
                g_animationStart = now;
            }
            else if (g_state == AppState::ApprovedFadeIn &&
                AnimationProgress(480) >= 1.0f) {
                g_state = AppState::Ready;
            }

            if (now - g_lastProcessScan >= std::chrono::seconds(1)) {
                g_lastProcessScan = now;
                RefreshGameList();
            }

            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;

    case WM_API_RESULT:
        if (g_state == AppState::Checking) {
            if (!IsRunningAsAdmin()) {
				g_state = AppState::Error;
				KillTimer(window, TIMER_ID);
				InvalidateRect(window, nullptr, FALSE);
				return 0;
            }
            if (wparam) {
                g_state = AppState::ApprovedFadeOut;
                g_animationStart = std::chrono::steady_clock::now();
            }
            else {
                g_state = AppState::Error;
                KillTimer(window, TIMER_ID);
            }

            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);

        RECT client{};
        GetClientRect(window, &client);
        DrawInterface(dc, client);

        EndPaint(window, &paint);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        KillTimer(window, TIMER_ID);

        if (g_worker.joinable()) {
            g_worker.join();
        }

        PostQuitMessage(0);
        return 0;

    case WM_MOUSEMOVE: {
        POINT point{
            GET_X_LPARAM(lparam),
            GET_Y_LPARAM(lparam)
        };

        RECT client{};
        GetClientRect(window, &client);

        const bool hovered =
            point.x >= client.right - 40 &&
            point.y >= 0 &&
            point.y < 36;

        if (hovered != g_closeHovered) {
            g_closeHovered = hovered;
            InvalidateRect(window, nullptr, FALSE);
        }

        const bool discordHovered =
            (g_state == AppState::ApprovedFadeIn ||
                g_state == AppState::Ready) &&
            point.x >= 12 &&
            point.x < 76 &&
            point.y >= client.bottom - 36 &&
            point.y < client.bottom - 12;

        if (discordHovered != g_discordHovered) {
            g_discordHovered = discordHovered;
            InvalidateRect(window, nullptr, FALSE);
        }

        const RECT launchRect = GetLaunchButtonRect(window);

        const bool launchHovered =
            (g_state == AppState::ApprovedFadeIn ||
                g_state == AppState::Ready) &&
            IsPointInsideRect(point, launchRect);

        if (launchHovered != g_launchHovered) {
            g_launchHovered = launchHovered;
            InvalidateRect(window, nullptr, FALSE);
        }

        int hoveredIndex = -1;

        if (g_state == AppState::Ready &&
            g_gameDropdownOpen &&
            point.x >= GameComboX(window) &&
            point.x < GameComboX(window) + GAME_COMBO_W &&
            point.y >= GAME_COMBO_Y + GAME_COMBO_H &&
            point.y < GAME_COMBO_Y + GAME_COMBO_H +
            GameOptionCount() * GAME_OPTION_H) {
            hoveredIndex =
                (point.y - GAME_COMBO_Y - GAME_COMBO_H) /
                GAME_OPTION_H;
        }

        if (hoveredIndex != g_gameHoveredIndex) {
            g_gameHoveredIndex = hoveredIndex;
            InvalidateRect(window, nullptr, FALSE);
        }

        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = window;
        TrackMouseEvent(&track);

        return 0;
    }

    case WM_MOUSELEAVE:
        g_closeHovered = false;
        g_discordHovered = false;
        g_gameHoveredIndex = -1;
        g_launchHovered = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int show_command
) {
    g_api_url = L"https://status.modularcontent.dev/api/status";

    GdiplusStartupInput startup_input{};

    if (GdiplusStartup(&g_gdiplus_token, &startup_input, nullptr)
        != Ok) {
        return 1;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    window_class.hIconSm = static_cast<HICON>(
        LoadImageW(
            instance,
            MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON,
            16,
            16,
            LR_DEFAULTCOLOR
        )
    );
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = L"Nightware3LoaderWindow";

    if (!RegisterClassExW(&window_class)) {
        GdiplusShutdown(g_gdiplus_token);
        return 1;
    }

    RECT rect{ 0, 0, 620, 360 };

    if (!AdjustWindowRect(
        &rect,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        FALSE)) {
        GdiplusShutdown(g_gdiplus_token);
        return 1;
    }

    const int windowWidth = rect.right - rect.left;
    const int windowHeight = rect.bottom - rect.top;

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    const int windowX = (screenWidth - windowWidth) / 2;
    const int windowY = (screenHeight - windowHeight) / 2;

    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW,
        window_class.lpszClassName,
        L"nightware\u00B3 Loader",
        WS_POPUP | WS_VISIBLE,
        windowX,
        windowY,
        windowWidth,
        windowHeight,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!window) {
        GdiplusShutdown(g_gdiplus_token);
        return 1;
    }

    g_window = window;

    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};

    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_worker.joinable()) {
        g_worker.join();
    }

    GdiplusShutdown(g_gdiplus_token);
    return static_cast<int>(message.wParam);
}