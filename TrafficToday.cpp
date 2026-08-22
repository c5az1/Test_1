#include <windows.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include "PluginInterface.h"

// ============================================================
// Global state
// ============================================================

static HINSTANCE g_hInstance = nullptr;
static HWND g_hWindow = nullptr;

static unsigned long long g_todayUpload = 0;
static unsigned long long g_todayDownload = 0;

static std::wstring g_configFile;
static std::wstring g_hostConfigDir;

// ============================================================
// Constants
// ============================================================

static constexpr UINT TIMER_UPDATE = 1;
static constexpr UINT TIMER_INTERVAL = 1000;

// Compact window size
static constexpr int WINDOW_WIDTH = 280;
static constexpr int WINDOW_HEIGHT = 125;

static const wchar_t WINDOW_CLASS[] =
    L"TrafficTodayWindowClass";

// ============================================================
// Forward declaration
// ============================================================

class CTrafficTodayPlugin;

// ============================================================
// DPI
// ============================================================

static int GetWindowDPI()
{
    if (g_hWindow != nullptr)
    {
        UINT dpi = GetDpiForWindow(g_hWindow);

        if (dpi != 0)
            return static_cast<int>(dpi);
    }

    HDC hdc = GetDC(nullptr);

    if (hdc == nullptr)
        return 96;

    int dpi = GetDeviceCaps(
        hdc,
        LOGPIXELSX);

    ReleaseDC(
        nullptr,
        hdc);

    return dpi > 0 ? dpi : 96;
}

static int Scale(int value)
{
    return MulDiv(
        value,
        GetWindowDPI(),
        96);
}

// ============================================================
// Format bytes
//
// 1 KB = 1000 B
// 1 MB = 1000 KB
// 1 GB = 1000 MB
// 1 TB = 1000 GB
// ============================================================

static std::wstring FormatBytesDecimal(
    unsigned long long bytes)
{
    constexpr double KB = 1000.0;
    constexpr double MB = 1000000.0;
    constexpr double GB = 1000000000.0;
    constexpr double TB = 1000000000000.0;

    double value =
        static_cast<double>(bytes);

    std::wstringstream ss;

    ss << std::fixed;

    if (value >= TB)
    {
        ss << std::setprecision(2)
           << value / TB
           << L" TB";
    }
    else if (value >= GB)
    {
        ss << std::setprecision(2)
           << value / GB
           << L" GB";
    }
    else if (value >= MB)
    {
        ss << std::setprecision(2)
           << value / MB
           << L" MB";
    }
    else if (value >= KB)
    {
        ss << std::setprecision(2)
           << value / KB
           << L" KB";
    }
    else
    {
        ss << bytes
           << L" B";
    }

    return ss.str();
}

// ============================================================
// Configuration
// ============================================================

static void BuildConfigPath(
    const wchar_t* configDir)
{
    if (configDir == nullptr)
        return;

    g_hostConfigDir = configDir;

    if (!g_hostConfigDir.empty() &&
        g_hostConfigDir.back() != L'\\')
    {
        g_hostConfigDir += L'\\';
    }

    g_configFile =
        g_hostConfigDir +
        L"TrafficToday.ini";
}

// ============================================================
// Get history_traffic.dat path
// ============================================================

static std::wstring GetHistoryTrafficPath()
{
    // Preferred:
    // Same directory supplied by TrafficMonitor
    if (!g_hostConfigDir.empty())
    {
        std::wstring path =
            g_hostConfigDir +
            L"history_traffic.dat";

        if (GetFileAttributesW(
                path.c_str()) !=
            INVALID_FILE_ATTRIBUTES)
        {
            return path;
        }
    }

    // Fallback:
    // %APPDATA%\TrafficMonitor\history_traffic.dat
    wchar_t appDataPath[MAX_PATH]{};

    DWORD appDataLen =
        GetEnvironmentVariableW(
            L"APPDATA",
            appDataPath,
            static_cast<DWORD>(
                _countof(appDataPath)));

    if (appDataLen > 0 &&
        appDataLen < _countof(appDataPath))
    {
        std::wstring path =
            std::wstring(appDataPath) +
            L"\\TrafficMonitor\\history_traffic.dat";

        if (GetFileAttributesW(
                path.c_str()) !=
            INVALID_FILE_ATTRIBUTES)
        {
            return path;
        }
    }

    return std::wstring();
}

// ============================================================
// Read file as Unicode text
// ============================================================

static bool ReadFileAsWideText(
    const std::wstring& path,
    std::wstring& outText)
{
    HANDLE hFile =
        CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ |
            FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};

    if (!GetFileSizeEx(
            hFile,
            &size) ||
        size.QuadPart <= 0 ||
        size.QuadPart >
            64LL * 1024 * 1024)
    {
        CloseHandle(hFile);
        return false;
    }

    std::string raw(
        static_cast<size_t>(
            size.QuadPart),
        '\0');

    DWORD bytesRead = 0;

    BOOL ok =
        ReadFile(
            hFile,
            &raw[0],
            static_cast<DWORD>(
                raw.size()),
            &bytesRead,
            nullptr);

    CloseHandle(hFile);

    if (!ok || bytesRead == 0)
        return false;

    raw.resize(bytesRead);

    // --------------------------------------------------------
    // UTF-16 LE BOM
    // --------------------------------------------------------

    if (raw.size() >= 2 &&
        static_cast<unsigned char>(
            raw[0]) == 0xFF &&
        static_cast<unsigned char>(
            raw[1]) == 0xFE)
    {
        const wchar_t* wideStart =
            reinterpret_cast<const wchar_t*>(
                raw.data() + 2);

        size_t wideChars =
            (raw.size() - 2) /
            sizeof(wchar_t);

        outText.assign(
            wideStart,
            wideChars);

        return true;
    }

    // --------------------------------------------------------
    // UTF-8
    // --------------------------------------------------------

    size_t offset = 0;

    if (raw.size() >= 3 &&
        static_cast<unsigned char>(
            raw[0]) == 0xEF &&
        static_cast<unsigned char>(
            raw[1]) == 0xBB &&
        static_cast<unsigned char>(
            raw[2]) == 0xBF)
    {
        offset = 3;
    }

    int wideLen =
        MultiByteToWideChar(
            CP_UTF8,
            0,
            raw.data() + offset,
            static_cast<int>(
                raw.size() - offset),
            nullptr,
            0);

    if (wideLen <= 0)
    {
        // ANSI fallback
        wideLen =
            MultiByteToWideChar(
                CP_ACP,
                0,
                raw.data() + offset,
                static_cast<int>(
                    raw.size() - offset),
                nullptr,
                0);

        if (wideLen <= 0)
            return false;

        outText.resize(
            static_cast<size_t>(
                wideLen));

        MultiByteToWideChar(
            CP_ACP,
            0,
            raw.data() + offset,
            static_cast<int>(
                raw.size() - offset),
            &outText[0],
            wideLen);

        return true;
    }

    outText.resize(
        static_cast<size_t>(
            wideLen));

    MultiByteToWideChar(
        CP_UTF8,
        0,
        raw.data() + offset,
        static_cast<int>(
            raw.size() - offset),
        &outText[0],
        wideLen);

    return true;
}

// ============================================================
// Read today's traffic from history_traffic.dat
//
// Format:
//
// lines: "2"
// 2026/08/21 24226/326812
// 2026/08/20 31541/285832
//
// Values are KB:
//
// upload / download
// ============================================================

static bool ReadTodayTrafficFromHistoryFile(
    unsigned long long& outUploadBytes,
    unsigned long long& outDownloadBytes)
{
    outUploadBytes = 0;
    outDownloadBytes = 0;

    std::wstring path =
        GetHistoryTrafficPath();

    if (path.empty())
        return false;

    std::wstring fileText;

    if (!ReadFileAsWideText(
            path,
            fileText))
    {
        return false;
    }

    SYSTEMTIME now{};

    GetLocalTime(&now);

    std::wistringstream fileStream(
        fileText);

    std::wstring line;

    while (std::getline(
        fileStream,
        line))
    {
        if (!line.empty() &&
            line.back() == L'\r')
        {
            line.pop_back();
        }

        if (line.empty())
            continue;

        std::wistringstream lineStream(
            line);

        std::wstring dateToken;
        std::wstring trafficToken;

        if (!(lineStream >>
              dateToken >>
              trafficToken))
        {
            continue;
        }

        size_t slashPos =
            trafficToken.find(L'/');

        if (slashPos ==
            std::wstring::npos)
        {
            continue;
        }

        double uploadKb =
            _wtof(
                trafficToken
                    .substr(
                        0,
                        slashPos)
                    .c_str());

        double downloadKb =
            _wtof(
                trafficToken
                    .substr(
                        slashPos + 1)
                    .c_str());

        if (uploadKb < 0.0)
            uploadKb = 0.0;

        if (downloadKb < 0.0)
            downloadKb = 0.0;

        // Normalize date separators
        std::wstring normalizedDate =
            dateToken;

        std::replace(
            normalizedDate.begin(),
            normalizedDate.end(),
            L'-',
            L'/');

        int year = 0;
        int month = 0;
        int day = 0;

        bool matched = false;

        if (swscanf_s(
                normalizedDate.c_str(),
                L"%d/%d/%d",
                &year,
                &month,
                &day) == 3)
        {
            matched =
                year == now.wYear &&
                month == now.wMonth &&
                day == now.wDay;
        }

        if (!matched)
            continue;

        outUploadBytes =
            static_cast<unsigned long long>(
                uploadKb * 1000.0 + 0.5);

        outDownloadBytes =
            static_cast<unsigned long long>(
                downloadKb * 1000.0 + 0.5);

        return true;
    }

    return false;
}

// ============================================================
// Refresh today's traffic
// ============================================================

static void RefreshTodayTraffic()
{
    unsigned long long upload = 0;
    unsigned long long download = 0;

    if (ReadTodayTrafficFromHistoryFile(
            upload,
            download))
    {
        g_todayUpload = upload;
        g_todayDownload = download;
    }
    else
    {
        g_todayUpload = 0;
        g_todayDownload = 0;
    }

    if (g_hWindow != nullptr)
    {
        InvalidateRect(
            g_hWindow,
            nullptr,
            FALSE);
    }
}

// ============================================================
// Save window position
// ============================================================

static void SaveWindowPosition()
{
    if (g_hWindow == nullptr)
        return;

    if (g_configFile.empty())
        return;

    RECT rc{};

    if (!GetWindowRect(
            g_hWindow,
            &rc))
    {
        return;
    }

    wchar_t buffer[32]{};

    _snwprintf_s(
        buffer,
        _countof(buffer),
        _TRUNCATE,
        L"%ld",
        rc.left);

    WritePrivateProfileStringW(
        L"Window",
        L"X",
        buffer,
        g_configFile.c_str());

    _snwprintf_s(
        buffer,
        _countof(buffer),
        _TRUNCATE,
        L"%ld",
        rc.top);

    WritePrivateProfileStringW(
        L"Window",
        L"Y",
        buffer,
        g_configFile.c_str());
}

// ============================================================
// Load window position
// ============================================================

static bool LoadWindowPosition(
    int& x,
    int& y)
{
    if (g_configFile.empty())
        return false;

    wchar_t buffer[32]{};

    GetPrivateProfileStringW(
        L"Window",
        L"X",
        L"",
        buffer,
        _countof(buffer),
        g_configFile.c_str());

    if (buffer[0] == L'\0')
        return false;

    x = _wtoi(buffer);

    GetPrivateProfileStringW(
        L"Window",
        L"Y",
        L"",
        buffer,
        _countof(buffer),
        g_configFile.c_str());

    if (buffer[0] == L'\0')
        return false;

    y = _wtoi(buffer);

    return true;
}

// ============================================================
// Check whether saved position is visible
// ============================================================

static bool IsPositionVisible(
    int x,
    int y)
{
    POINT pt{
        x,
        y
    };

    return MonitorFromPoint(
        pt,
        MONITOR_DEFAULTTONULL) != nullptr;
}

// ============================================================
// Create UI font
// ============================================================

static HFONT CreateUIFont(
    int height,
    int weight)
{
    int scaledHeight =
        Scale(height);

    return CreateFontW(
        -scaledHeight,
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH |
        FF_DONTCARE,
        L"Segoe UI");
}

// ============================================================
// Draw background
// ============================================================

static void DrawWindowBackground(
    HDC hdc,
    const RECT& rc)
{
    HBRUSH background =
        CreateSolidBrush(
            RGB(29, 29, 29));

    HPEN border =
        CreatePen(
            PS_SOLID,
            Scale(1),
            RGB(70, 70, 70));

    HGDIOBJ oldBrush =
        SelectObject(
            hdc,
            background);

    HGDIOBJ oldPen =
        SelectObject(
            hdc,
            border);

    RoundRect(
        hdc,
        rc.left,
        rc.top,
        rc.right,
        rc.bottom,
        Scale(8),
        Scale(8));

    SelectObject(
        hdc,
        oldBrush);

    SelectObject(
        hdc,
        oldPen);

    DeleteObject(background);
    DeleteObject(border);
}

// ============================================================
// Draw window contents
// ============================================================

static void PaintTrafficWindow(
    HDC hdc,
    const RECT& client)
{
    DrawWindowBackground(
        hdc,
        client);

    SetBkMode(
        hdc,
        TRANSPARENT);

    // --------------------------------------------------------
    // Fonts
    // --------------------------------------------------------

    HFONT valueFont =
        CreateUIFont(
            14,
            FW_SEMIBOLD);

    HFONT trafficFont =
        CreateUIFont(
            12,
            FW_NORMAL);

    // --------------------------------------------------------
    // Total
    // --------------------------------------------------------

    unsigned long long total =
        g_todayUpload +
        g_todayDownload;

    std::wstring totalText =
        L"Today Usage: " +
        FormatBytesDecimal(
            total);

    HFONT oldFont =
        static_cast<HFONT>(
            SelectObject(
                hdc,
                valueFont));

    SetTextColor(
        hdc,
        RGB(245, 245, 245));

    RECT totalRect{
        Scale(14),
        Scale(10),
        client.right - Scale(14),
        Scale(38)
    };

    DrawTextW(
        hdc,
        totalText.c_str(),
        -1,
        &totalRect,
        DT_LEFT |
        DT_SINGLELINE |
        DT_VCENTER);

    // --------------------------------------------------------
    // Separator
    // --------------------------------------------------------

    HPEN separatorPen =
        CreatePen(
            PS_SOLID,
            Scale(1),
            RGB(55, 55, 55));

    HPEN oldPen =
        static_cast<HPEN>(
            SelectObject(
                hdc,
                separatorPen));

    MoveToEx(
        hdc,
        Scale(12),
        Scale(43),
        nullptr);

    LineTo(
        hdc,
        client.right - Scale(12),
        Scale(43));

    SelectObject(
        hdc,
        oldPen);

    DeleteObject(
        separatorPen);

    // --------------------------------------------------------
    // Upload
    // --------------------------------------------------------

    SelectObject(
        hdc,
        trafficFont);

    SetTextColor(
        hdc,
        RGB(220, 90, 90));

    RECT uploadIconRect{
        Scale(14),
        Scale(50),
        Scale(30),
        Scale(68)
    };

    DrawTextW(
        hdc,
        L"\u25B2",
        -1,
        &uploadIconRect,
        DT_LEFT |
        DT_SINGLELINE |
        DT_VCENTER);

    SetTextColor(
        hdc,
        RGB(205, 205, 205));

    std::wstring uploadText =
        L"Upload: " +
        FormatBytesDecimal(
            g_todayUpload);

    RECT uploadRect{
        Scale(32),
        Scale(50),
        client.right - Scale(14),
        Scale(68)
    };

    DrawTextW(
        hdc,
        uploadText.c_str(),
        -1,
        &uploadRect,
        DT_LEFT |
        DT_SINGLELINE |
        DT_VCENTER);

    // --------------------------------------------------------
    // Download
    // --------------------------------------------------------

    SetTextColor(
        hdc,
        RGB(90, 190, 110));

    RECT downloadIconRect{
        Scale(14),
        Scale(70),
        Scale(30),
        Scale(88)
    };

    DrawTextW(
        hdc,
        L"\u25BC",
        -1,
        &downloadIconRect,
        DT_LEFT |
        DT_SINGLELINE |
        DT_VCENTER);

    SetTextColor(
        hdc,
        RGB(205, 205, 205));

    std::wstring downloadText =
        L"Download: " +
        FormatBytesDecimal(
            g_todayDownload);

    RECT downloadRect{
        Scale(32),
        Scale(70),
        client.right - Scale(14),
        Scale(88)
    };

    DrawTextW(
        hdc,
        downloadText.c_str(),
        -1,
        &downloadRect,
        DT_LEFT |
        DT_SINGLELINE |
        DT_VCENTER);

    // --------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------

    SelectObject(
        hdc,
        oldFont);

    DeleteObject(valueFont);
    DeleteObject(trafficFont);
}

// ============================================================
// Window procedure
// ============================================================

static LRESULT CALLBACK TrafficWindowProc(
    HWND hWnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        SetTimer(
            hWnd,
            TIMER_UPDATE,
            TIMER_INTERVAL,
            nullptr);

        RefreshTodayTraffic();

        return 0;
    }

    case WM_TIMER:
    {
        RefreshTodayTraffic();

        return 0;
    }

    case WM_ERASEBKGND:
    {
        return 1;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};

        HDC hdc =
            BeginPaint(
                hWnd,
                &ps);

        RECT rc{};

        GetClientRect(
            hWnd,
            &rc);

        PaintTrafficWindow(
            hdc,
            rc);

        EndPaint(
            hWnd,
            &ps);

        return 0;
    }

    case WM_MOVE:
    {
        SaveWindowPosition();

        return 0;
    }

    case WM_CLOSE:
    {
        SaveWindowPosition();

        DestroyWindow(
            hWnd);

        return 0;
    }

    case WM_DESTROY:
    {
        KillTimer(
            hWnd,
            TIMER_UPDATE);

        g_hWindow = nullptr;

        return 0;
    }
    }

    return DefWindowProcW(
        hWnd,
        message,
        wParam,
        lParam);
}

// ============================================================
// Register window class
// ============================================================

static bool RegisterTrafficWindowClass()
{
    static bool registered = false;

    if (registered)
        return true;

    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(WNDCLASSEXW);

    wc.style =
        CS_HREDRAW |
        CS_VREDRAW;

    wc.lpfnWndProc =
        TrafficWindowProc;

    wc.hInstance =
        g_hInstance;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            MAKEINTRESOURCEW(IDC_ARROW));

    wc.hbrBackground =
        nullptr;

    wc.lpszClassName =
        WINDOW_CLASS;

    if (!RegisterClassExW(&wc))
    {
        if (GetLastError() !=
            ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
    }

    registered = true;

    return true;
}

// ============================================================
// Show Traffic Today window
// ============================================================

static void ShowTrafficWindow()
{
    if (g_hWindow != nullptr)
    {
        if (!IsWindow(g_hWindow))
        {
            g_hWindow = nullptr;
        }
        else
        {
            ShowWindow(
                g_hWindow,
                SW_SHOWNORMAL);

            SetForegroundWindow(
                g_hWindow);

            return;
        }
    }

    if (!RegisterTrafficWindowClass())
        return;

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;

    if (!LoadWindowPosition(
            x,
            y) ||
        !IsPositionVisible(
            x,
            y))
    {
        x = CW_USEDEFAULT;
        y = CW_USEDEFAULT;
    }

    g_hWindow =
        CreateWindowExW(
            WS_EX_TOOLWINDOW,

            WINDOW_CLASS,

            L"Traffic Today",

            WS_OVERLAPPED |
            WS_CAPTION |
            WS_SYSMENU |
            WS_MINIMIZEBOX,

            x,
            y,

            Scale(WINDOW_WIDTH),
            Scale(WINDOW_HEIGHT),

            nullptr,
            nullptr,
            g_hInstance,
            nullptr);

    if (g_hWindow == nullptr)
        return;

    ShowWindow(
        g_hWindow,
        SW_SHOWNORMAL);

    UpdateWindow(
        g_hWindow);

    SetForegroundWindow(
        g_hWindow);
}

// ============================================================
// Plugin item
// ============================================================

class CTrafficTodayItem :
    public IPluginItem
{
private:

    CTrafficTodayPlugin* m_pPlugin;

public:

    explicit CTrafficTodayItem(
        CTrafficTodayPlugin* pPlugin)
        : m_pPlugin(pPlugin)
    {
    }

    const wchar_t* GetItemName()
        const override;

    const wchar_t* GetItemId()
        const override;

    const wchar_t* GetItemLableText()
        const override;

    const wchar_t* GetItemValueText()
        const override;

    const wchar_t* GetItemValueSampleText()
        const override;

    int OnMouseEvent(
        MouseEventType type,
        int x,
        int y,
        void* data,
        int flags) override;
};

// ============================================================
// Main plugin
// ============================================================

class CTrafficTodayPlugin :
    public ITMPlugin
{
private:

    CTrafficTodayItem m_item;

public:

    CTrafficTodayPlugin()
        : m_item(this)
    {
    }

    void OnInitialize(
        ITrafficMonitor*) override
    {
    }

    IPluginItem* GetItem(
        int index) override
    {
        if (index == 0)
            return &m_item;

        return nullptr;
    }

    void DataRequired() override
    {
        // Data is read directly from history_traffic.dat.
    }

    unsigned long long GetTodayTotal()
        const
    {
        return
            g_todayUpload +
            g_todayDownload;
    }

    // --------------------------------------------------------
    // Plugin command
    // --------------------------------------------------------

    int GetCommandCount() override
    {
        return 1;
    }

    const wchar_t* GetCommandName(
        int command_index) override
    {
        if (command_index == 0)
            return L"Open Traffic Today";

        return nullptr;
    }

    void OnPluginCommand(
        int command_index,
        void*,
        void*) override
    {
        if (command_index == 0)
        {
            ShowTrafficWindow();
        }
    }

    int IsCommandChecked(
        int command_index) override
    {
        if (command_index != 0)
            return 0;

        return
            g_hWindow != nullptr &&
            IsWindow(g_hWindow) &&
            IsWindowVisible(g_hWindow);
    }

    // --------------------------------------------------------
    // Plugin information
    // --------------------------------------------------------

    const wchar_t* GetInfo(
        PluginInfoIndex index) override
    {
        switch (index)
        {
        case TMI_NAME:
            return L"Traffic Today";

        case TMI_DESCRIPTION:
            return L"Standalone window for today's network traffic";

        case TMI_AUTHOR:
            return L"Custom Plugin";

        case TMI_COPYRIGHT:
            return L"Copyright (c) 2026";

        case TMI_VERSION:
            return L"1.1";

        case TMI_URL:
            return L"";

        default:
            return L"";
        }
    }

    // --------------------------------------------------------
    // Extended information
    // --------------------------------------------------------

    void OnExtenedInfo(
        ExtendedInfoIndex index,
        const wchar_t* data) override
    {
        if (index == EI_CONFIG_DIR)
        {
            BuildConfigPath(data);
        }
    }
};

// ============================================================
// CTrafficTodayItem implementations
// ============================================================

const wchar_t*
CTrafficTodayItem::GetItemName()
    const
{
    return L"Today Usage";
}

const wchar_t*
CTrafficTodayItem::GetItemId()
    const
{
    return L"TodayUsage";
}

const wchar_t*
CTrafficTodayItem::GetItemLableText()
    const
{
    return L"Today: ";
}

const wchar_t*
CTrafficTodayItem::GetItemValueText()
    const
{
    static std::wstring value;

    if (m_pPlugin == nullptr)
        return L"0 B";

    unsigned long long total =
        m_pPlugin->GetTodayTotal();

    value =
        FormatBytesDecimal(total);

    return value.c_str();
}

const wchar_t*
CTrafficTodayItem::GetItemValueSampleText()
    const
{
    return L"999.99 GB";
}

int
CTrafficTodayItem::OnMouseEvent(
    MouseEventType type,
    int,
    int,
    void*,
    int)
{
    if (type == MT_DBCLICKED)
    {
        ShowTrafficWindow();

        return 1;
    }

    return 0;
}

// ============================================================
// DLL export
// ============================================================

extern "C"
__declspec(dllexport)
ITMPlugin* TMPluginGetInstance()
{
    static CTrafficTodayPlugin plugin;

    return &plugin;
}

// ============================================================
// DLL entry
// ============================================================

BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD ul_reason_for_call,
    LPVOID)
{
    if (ul_reason_for_call ==
        DLL_PROCESS_ATTACH)
    {
        g_hInstance = hModule;

        DisableThreadLibraryCalls(
            hModule);
    }

    return TRUE;
}