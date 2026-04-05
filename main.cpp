#include <windows.h>
#include <string>
#include <dwmapi.h>
#include <math.h>
#include <wlanapi.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <bluetoothapis.h>
#include <powrprof.h>
#include <winhttp.h>
#include <thread>
#include <regex>
#include <sstream>


#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>

#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")

using namespace Gdiplus;
using namespace winrt;
using namespace Windows::Media::Control;

// 常量定义
#define MARGIN_TOP              24
#define COLLAPSED_WIDTH_RATIO   0.12f
#define EXPANDED_WIDTH_RATIO    COLLAPSED_WIDTH_RATIO
#define HEIGHT_WIDTH_RATIO      0.113f
#define ANIMATION_DURATION      300
#define TIMER_INTERVAL          2
#define WINDOW_ALPHA            255
#define TIMER_CHECK_WIFI        1001
#define CORNER_RADIUS           26
#define WINDOW_PADDING          3   // 背景比控件大n个像素
#define WM_UPDATE_LYRICS (WM_APP + 101)   // 更新歌词消息
#define TIMER_INFO_UPDATE       1002   // 信息更新定时器
#define INFO_UPDATE_INTERVAL    1000   // wifi检测间隔

#define BUTTON_RADIUS_RATIO     0.0045f
#define TIME_FONT_SIZE_RATIO    0.006f
#define BUTTON_LEFT_RATIO       0.004f
#define BUTTON_SPACING_RATIO    0.004f

#define DRAG_THRESHOLD_PX 5
#define LONG_PRESS_TIME_MS 200

#define WAVE_BAR_COUNT 4
#define WAVE_UPDATE_INTERVAL 50 

#define WM_TRAYICON (WM_APP + 100)
#define ID_TRAY_AUTOSTART       1002
#define ID_TRAY_MOUSE_TRANSPARENT 1003   // 鼠标穿透菜单项

//颜色主题
struct ThemeColors {
    Color background;   // 背景色
    Color text;         // 时间/信息文字颜色
    Color buttonNormal; // 按钮默认颜色
    Color buttonHover;  // 按钮悬停颜色
    Color buttonActive; // 按钮激活颜色
};

static const ThemeColors DarkTheme = {
    Color(100, 0, 0, 0),        // 黑色背景
    Color(255, 255, 255, 255),  // 白色文字
    Color(100, 0, 0, 0),     // 深灰按钮
    Color(100, 10, 10, 10),
    Color(120, 64, 150, 64)
};

static const ThemeColors LightTheme = {
    Color(100, 255, 255, 255),  // 白色背景
    Color(255, 0, 0, 0),        // 黑色文字
    Color(100, 255, 255, 255),  // 浅灰按钮
    Color(100, 245, 245, 245),
    Color(120, 191, 255, 191)
};

// 前向声明 
static void OnWifiButtonClick();
static void OnBluetoothButtonClick();
static void OnEmptyButtonClick();
bool IsWifiConnected();
// 开机自启动函数声明
static bool IsAutoStartEnabled();
static void SetAutoStart(bool enable);

static FontFamily* g_pFontFamily;
static FontFamily* g_pEmojiFontFamily;

static BOOL g_isDarkTheme = TRUE;   // 默认主题
static const ThemeColors* g_pCurrentTheme = &DarkTheme; //主题指针

static std::wstring g_lyricsDisplay;      // 当前要显示的歌词行
static bool g_lyricsFetching = false;     // 防止重复请求

// 按钮类
class CircleButton {
public:
    CircleButton(const wchar_t* emoji, void (*onClick)())
        : m_onClick(onClick), m_isActive(false), m_isHover(false), m_isPressed(false), m_animHover(0.0f) {
        wcsncpy_s(m_emoji, 8, emoji, _TRUNCATE);
    }

    void SetActive(bool active) { m_isActive = active; }
    void Update() {
    }

    void Draw(Graphics& graphics, int centerX, int centerY, int radius) {
        const ThemeColors& theme = *g_pCurrentTheme;

        // 背景色
        Color bgColor = m_isActive ? theme.buttonActive : theme.background;
        SolidBrush bgBrush(bgColor);
        graphics.FillEllipse(&bgBrush, centerX - radius, centerY - radius, radius * 2, radius * 2);

        // 图标
        if (wcslen(m_emoji) > 0) {
            int emojiSize = (int)(radius * 1.2f);
            Font emojiFont(g_pEmojiFontFamily, (REAL)emojiSize, FontStyleRegular, UnitPixel);
            SolidBrush iconBrush(g_isDarkTheme ? Color(255, 255, 255, 255) : Color(255, 0, 0, 0));
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            format.SetLineAlignment(StringAlignmentCenter);
            RectF textRect((REAL)(centerX - radius), (REAL)(centerY - radius),
                (REAL)(radius * 2), (REAL)(radius * 2));
            graphics.DrawString(m_emoji, -1, &emojiFont, textRect, &format, &iconBrush);
        }
    }

    bool HandleMouse(UINT msg, int mouseX, int mouseY, int centerX, int centerY, int radius) {
        bool inside = ((mouseX - centerX) * (mouseX - centerX) + (mouseY - centerY) * (mouseY - centerY)) <= (radius * radius);
        switch (msg) {
        case WM_MOUSEMOVE:
            if (inside)
                SetCursor(LoadCursor(NULL, IDC_HAND));
            else
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            return false;   
        case WM_LBUTTONDOWN:
            if (inside) { m_isPressed = true; return true; }
            break;
        case WM_LBUTTONUP:
            if (m_isPressed && inside && m_onClick) m_onClick();
            m_isPressed = false;
            if (inside) return true; 
            break;
        }
        return false;
    }

private:
    wchar_t m_emoji[8];
    void (*m_onClick)();
    bool m_isActive, m_isHover, m_isPressed;
    float m_animHover;
};

//全局变量
static HWND      g_hWnd = NULL;
static BOOL      g_isExpanded = FALSE;
static BOOL      g_isAnimating = FALSE;
static DWORD     g_animStartTime = 0;
static int       g_startW, g_startH;
static int       g_targetW, g_targetH;
static int       g_startX, g_startY;
static int       g_centerX;
static int       g_screenW = 0;
static int       g_collapsedW = 0;
static int       g_expandedW = 0;
static int       g_collapsedH = 0;
static int       g_expandedH = 0;

static int       g_fixedBtnRadius = 0;
static int       g_fixedTimeFontSize = 0;
static int       g_fixedBtnMargin = 0;
static int       g_fixedBtnSpacing = 0;
static int       g_fixedBtnCenterY = 0;
static int       g_fixedTimeCenterY = 0;

static HDC       g_hMemDC = NULL;
static HBITMAP   g_hBitmap = NULL;
static void* g_pBits = NULL;
static int       g_winWidth = 0, g_winHeight = 0;

static WCHAR     g_timeStr[16] = L"";
static DWORD     g_lastTimeUpdate = 0;

enum ButtonId {
    BTN_BLUETOOTH,
    BTN_WIFI,
    BTN_EMPTY1,
    BTN_EMPTY2,
    BTN_COUNT
};

static BOOL g_pressedOnButton = FALSE;   // 是否按在按钮上

static WCHAR   g_wifiInfo[64] = L"🌏未连接到网络";
static WCHAR   g_bluetoothInfo[64] = L"🎧未连接到设备";
static WCHAR   g_batteryInfo[64] = L"🔋目前电量--%";
static int     g_batteryPercent = -1;
static BOOL    g_isOnBattery = FALSE;

static BOOL g_isDragging = FALSE;
static BOOL g_longPressTriggered = FALSE;
static POINT g_dragStart = { 0, 0 };
static DWORD g_dragStartTime = 0;

static CircleButton* g_buttons[BTN_COUNT] = { nullptr };

static ULONG_PTR g_gdiplusToken = 0;

static GlobalSystemMediaTransportControlsSessionManager g_sessionManager = nullptr;
static event_token g_sessionAddedToken;
static GlobalSystemMediaTransportControlsSession g_currentSession = nullptr;
static event_token g_mediaPropertiesChangedToken;
static event_token g_playbackInfoChangedToken;

static std::wstring g_musicTitle;
static std::wstring g_musicArtist;
static bool g_hasMusic = false;
static bool g_isPlaying = false;
static float g_waveAmplitudes[WAVE_BAR_COUNT] = { 0.3f, 0.5f, 0.7f, 0.4f };
static float g_wavePhase = 0.0f;
static DWORD g_lastWaveUpdate = 0;

static NOTIFYICONDATAW g_nid = {};
static HMENU g_hTrayMenu = NULL;

static BOOL g_isMuted = FALSE;   // 静音状态
static BOOL g_isMouseTransparent = FALSE;   // 鼠标穿透状态


// 检查当前是否已设置开机自启动
static bool IsAutoStartEnabled() {
    HKEY hKey;
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {

        WCHAR value[MAX_PATH];
        DWORD valueLen = sizeof(value);
        DWORD type;


        LONG result = RegQueryValueExW(hKey, L"DynamicIsland", NULL, &type,
            (LPBYTE)value, &valueLen);
        RegCloseKey(hKey);

        if (result == ERROR_SUCCESS && type == REG_SZ) {
            return (wcscmp(value, exePath) == 0);
        }
    }
    return false;
}

// 设置/取消开机自启动
static void SetAutoStart(bool enable) {
    HKEY hKey;
    WCHAR exePath[MAX_PATH];
    DWORD pathLen = GetModuleFileNameW(NULL, exePath, MAX_PATH);

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {

        if (enable) {
            // 添加启动项DynamicIsland
            RegSetValueExW(hKey, L"DynamicIsland", 0, REG_SZ,
                (const BYTE*)exePath, (pathLen + 1) * sizeof(WCHAR));
        }
        else {
            RegDeleteValueW(hKey, L"DynamicIsland");
        }
        RegCloseKey(hKey);
    }
}

// 设置鼠标穿透状态
static void SetMouseTransparent(BOOL enable) {
    LONG_PTR exStyle = GetWindowLongPtr(g_hWnd, GWL_EXSTYLE);
    if (enable) {
        exStyle |= WS_EX_TRANSPARENT;
    }
    else {
        exStyle &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongPtr(g_hWnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(g_hWnd, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    g_isMouseTransparent = enable;
}

static inline float GetCornerAlpha(int x, int y, int w, int h, int r) {
    int dx = 0, dy = 0;
    if (x < r) dx = r - x;
    else if (x > w - r - 1) dx = x - (w - r - 1);
    if (y < r) dy = r - y;
    else if (y > h - r - 1) dy = y - (h - r - 1);

    if (dx == 0 && dy == 0) return 1.0f;
    float dist = sqrtf((float)dx * dx + (float)dy * dy);
    if (dist >= r) return 0.0f;
    float t = 1.0f - dist / r;
    return t;//* t * (3.0f - 2.0f * t); // smoothstep
}

static int GetScreenWidth() { return GetSystemMetrics(SM_CXSCREEN); }

static void UpdateWindowSizes() {
    g_screenW = GetScreenWidth();
    g_collapsedW = (int)(g_screenW * COLLAPSED_WIDTH_RATIO) + WINDOW_PADDING * 2;
    g_expandedW = (int)(g_screenW * EXPANDED_WIDTH_RATIO) + WINDOW_PADDING * 2;
    if (g_collapsedW < 60) g_collapsedW = 60;
    if (g_expandedW < 60) g_expandedW = 60;

    g_collapsedH = (int)(g_collapsedW * HEIGHT_WIDTH_RATIO) + WINDOW_PADDING * 2;
    if (g_collapsedH < 18) g_collapsedH = 18;
    g_expandedH = g_collapsedH * 2;
}

static void GetTopCenterPosition(int width, int height, int* pX, int* pY) {
    *pX = (g_screenW - width) / 2;
    *pY = MARGIN_TOP;
}

static void UpdateTimeString() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfW(g_timeStr, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
}

void UpdateWifiInfo() {
    if (IsWifiConnected()) {
        wcscpy_s(g_wifiInfo, L"🌏已连接到网络");
    }
    else {
        wcscpy_s(g_wifiInfo, L"🌏未连接到网络");
    }
}

void OnLockButtonClick() {
    LockWorkStation();
}

void OnMuteButtonClick() {
    g_isMuted = !g_isMuted;

    keybd_event(VK_VOLUME_MUTE, 0, KEYEVENTF_EXTENDEDKEY, 0);
    keybd_event(VK_VOLUME_MUTE, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);

    if (g_buttons[BTN_EMPTY1]) {
        g_buttons[BTN_EMPTY1]->SetActive(g_isMuted);
        InvalidateRect(g_hWnd, NULL, FALSE);
    }
}

static void SetSystemTheme(BOOL isDark) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        DWORD value = isDark ? 0 : 1;
        RegSetValueExW(hKey, L"AppsUseLightTheme", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);

        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 100, NULL);
    }
}

void OnThemeToggleClick() {
    g_isDarkTheme = !g_isDarkTheme;
    g_pCurrentTheme = g_isDarkTheme ? &DarkTheme : &LightTheme;
    //SetSystemTheme(g_isDarkTheme);
    InvalidateRect(g_hWnd, NULL, FALSE);
}

static BOOL GetSystemThemeIsDark() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 1;
        DWORD size = sizeof(value);
        if (RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return (value == 0);
        }
        RegCloseKey(hKey);
    }
    return FALSE;
}

void UpdateBluetoothInfo() {
    BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams = { sizeof(searchParams) };
    searchParams.fReturnAuthenticated = TRUE;
    searchParams.fReturnRemembered = TRUE;
    searchParams.fReturnConnected = TRUE;
    searchParams.fReturnUnknown = FALSE;
    searchParams.fIssueInquiry = FALSE;
    searchParams.cTimeoutMultiplier = 0;

    BLUETOOTH_DEVICE_INFO deviceInfo = { sizeof(deviceInfo) };
    HBLUETOOTH_DEVICE_FIND hFind = BluetoothFindFirstDevice(&searchParams, &deviceInfo);
    if (hFind) {
        wcscpy_s(g_bluetoothInfo, L"🎧");
        wcsncat_s(g_bluetoothInfo, deviceInfo.szName, _TRUNCATE);
        BluetoothFindDeviceClose(hFind);
    }
    else {
        wcscpy_s(g_bluetoothInfo, L"🎧未连接到设备");
    }
}

void UpdateBatteryInfo() {
    SYSTEM_POWER_STATUS status;
    if (GetSystemPowerStatus(&status)) {
        if (status.BatteryFlag == 128) {
            wcscpy_s(g_batteryInfo, L"🔋目前电量100%");
            g_batteryPercent = -1;
        }
        else {
            g_batteryPercent = status.BatteryLifePercent;
            if (g_batteryPercent <= 100) {
                wsprintfW(g_batteryInfo, L"🔋目前电量%d%%", g_batteryPercent);
            }
            else {
                wcscpy_s(g_batteryInfo, L"🔋目前电量--%");
            }
            g_isOnBattery = (status.ACLineStatus == 0);
            if (g_isOnBattery) {
                wcscat_s(g_batteryInfo, L" ⚠️");
            }
        }
    }
    else {
        wcscpy_s(g_batteryInfo, L"🔋未知");
    }
}

void UpdateInfoPanel() {
    UpdateWifiInfo();
    UpdateBluetoothInfo();
    UpdateBatteryInfo();

    if (g_buttons[BTN_WIFI]) {
        bool connected = IsWifiConnected();
        g_buttons[BTN_WIFI]->SetActive(connected);
    }

    InvalidateRect(g_hWnd, NULL, FALSE);
}

static void InitFixedUISizes() {
    g_fixedBtnRadius = (int)(g_screenW * BUTTON_RADIUS_RATIO);
    if (g_fixedBtnRadius < 6) g_fixedBtnRadius = 6;
    if (g_fixedBtnRadius > 20) g_fixedBtnRadius = 20;

    g_fixedTimeFontSize = (int)(g_screenW * TIME_FONT_SIZE_RATIO);
    if (g_fixedTimeFontSize < 12) g_fixedTimeFontSize = 12;
    if (g_fixedTimeFontSize > 36) g_fixedTimeFontSize = 36;

    g_fixedBtnMargin = (int)(g_screenW * BUTTON_LEFT_RATIO);
    if (g_fixedBtnMargin < 4) g_fixedBtnMargin = 4;

    g_fixedBtnSpacing = (int)(g_screenW * BUTTON_SPACING_RATIO);
    if (g_fixedBtnSpacing < 3) g_fixedBtnSpacing = 3;

    g_fixedBtnCenterY = g_collapsedH / 2;
    g_fixedTimeCenterY = g_collapsedH / 2;

    int contentHeight = g_collapsedH - WINDOW_PADDING * 2;
    g_fixedBtnCenterY = WINDOW_PADDING + contentHeight / 2;
    g_fixedTimeCenterY = g_fixedBtnCenterY;
}

static int GetButtonCenterX(int windowWidth, ButtonId id) {
    int radius = g_fixedBtnRadius;
    int margin = g_fixedBtnMargin + WINDOW_PADDING;
    int spacing = g_fixedBtnSpacing;

    switch (id) {
    case BTN_BLUETOOTH: return margin + radius + (2 * radius + spacing);
    case BTN_WIFI:       return margin + radius; 
    case BTN_EMPTY2:     return windowWidth - margin - radius;
    case BTN_EMPTY1:     return (windowWidth - margin - radius) - (2 * radius + spacing);
    default:             return 0;
    }
}

static void StartAnimation(BOOL expand) {
    if (g_isAnimating && g_isExpanded == expand) return;
    RECT rc;
    GetWindowRect(g_hWnd, &rc);
    g_startX = rc.left;
    g_startY = rc.top;
    g_startW = rc.right - rc.left;
    g_startH = rc.bottom - rc.top;
    g_centerX = rc.left + g_startW / 2;
    g_targetW = expand ? g_expandedW : g_collapsedW;
    g_targetH = expand ? g_expandedH : g_collapsedH;
    g_isAnimating = TRUE;
    g_animStartTime = GetTickCount();
    g_isExpanded = expand;
}

static BOOL UpdateAnimation() {
    if (!g_isAnimating) return FALSE;
    DWORD now = GetTickCount();
    float t = (float)(now - g_animStartTime) / ANIMATION_DURATION;
    if (t >= 1.0f) { t = 1.0f; g_isAnimating = FALSE; }
    float eased = 1.0f - (1.0f - t) * (1.0f - t);
    int curW = (int)(g_startW + (g_targetW - g_startW) * eased);
    int curH = (int)(g_startH + (g_targetH - g_startH) * eased);
    int newX = g_centerX - curW / 2;
    SetWindowPos(g_hWnd, HWND_TOPMOST, newX, g_startY, curW, curH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hWnd, NULL, TRUE);
    return g_isAnimating;
}

static void CheckMouseHover() {
    if (g_isAnimating) return;
    POINT pt;
    GetCursorPos(&pt);
    RECT rc;
    GetWindowRect(g_hWnd, &rc);
    if (!g_isExpanded) rc.bottom += 20;
    BOOL inside = PtInRect(&rc, pt);
    if (inside && !g_isExpanded) StartAnimation(TRUE);
    else if (!inside && g_isExpanded) StartAnimation(FALSE);
}

static void DrawMusicInCollapsed(Graphics& graphics, int width, int height) {
    const ThemeColors& theme = *g_pCurrentTheme;

    int usableW = width - 2 * WINDOW_PADDING;
    int usableH = height - 2 * WINDOW_PADDING;
    int startX = WINDOW_PADDING;
    int startY = WINDOW_PADDING;

    int waveAreaWidth = 50;          // 音频条区域宽度
    int barWidth = 4;
    int barSpacing = 3;
    int totalBarsWidth = WAVE_BAR_COUNT * barWidth + (WAVE_BAR_COUNT - 1) * barSpacing;
    int barStartX = startX + (waveAreaWidth - totalBarsWidth) / 2;
    int barCenterY = startY + usableH / 2;

    for (int i = 0; i < WAVE_BAR_COUNT; i++) {
        int barHeight = (int)(6 + g_waveAmplitudes[i] * 14); 
        if (barHeight > usableH - 4) barHeight = usableH - 4;
        int barTop = barCenterY - barHeight / 2;
        RectF barRect((REAL)(barStartX + i * (barWidth + barSpacing)),
            (REAL)barTop,
            (REAL)barWidth,
            (REAL)barHeight);
        SolidBrush barBrush(theme.text);
        graphics.FillRectangle(&barBrush, barRect);
    }

    int titleStartX = startX + waveAreaWidth + 5;
    int titleMaxWidth = usableW - waveAreaWidth - 5;
    if (titleMaxWidth < 40) titleMaxWidth = 40;

    std::wstring displayTitle = g_musicTitle.empty() ? L"🎵 正在播放" : g_musicTitle;
    int titleFontSize = (int)(g_fixedTimeFontSize * 0.75f);
    if (titleFontSize < 10) titleFontSize = 10;
    if (titleFontSize > 28) titleFontSize = 28;
    Font titleFont(g_pFontFamily, (REAL)titleFontSize, FontStyleBold, UnitPixel);
    SolidBrush textBrush(theme.text);

    StringFormat format;
    format.SetAlignment(StringAlignmentNear);
    format.SetLineAlignment(StringAlignmentCenter);
    RectF titleRect((REAL)titleStartX, (REAL)startY, (REAL)titleMaxWidth, (REAL)usableH);
    graphics.DrawString(displayTitle.c_str(), -1, &titleFont, titleRect, &format, &textBrush);
}

static void DrawContentToBitmap(int width, int height) {
    memset(g_pBits, 0, width * height * 4);
    Graphics graphics(g_hMemDC);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    const ThemeColors& theme = *g_pCurrentTheme;

    // 背景
    SolidBrush backgroundBrush(theme.background);
    graphics.FillRectangle(&backgroundBrush, Rect(0, 0, width, height));

    // 显示音乐信息 
    bool showMusic = (!g_isExpanded && g_hasMusic && g_isPlaying);
    if (showMusic) {
        DrawMusicInCollapsed(graphics, width, height);
    }
    else {
        // 原始绘制逻辑
        Font timeFont(g_pFontFamily, (REAL)g_fixedTimeFontSize, FontStyleBold, UnitPixel);
        SolidBrush textBrush(theme.text);
        RectF bounds;
        graphics.MeasureString(g_timeStr, -1, &timeFont, PointF(0, 0), &bounds);
        float textHeight = bounds.Height;
        float textY = g_fixedTimeCenterY - textHeight / 2.0f;
        StringFormat format;
        format.SetAlignment(StringAlignmentCenter);
        PointF origin((float)width / 2.0f, textY);
        graphics.DrawString(g_timeStr, -1, &timeFont, origin, &format, &textBrush);

        // 按钮
        for (int i = 0; i < BTN_COUNT; i++) {
            if (g_buttons[i]) {
                int centerX = GetButtonCenterX(width, (ButtonId)i);
                g_buttons[i]->Draw(graphics, centerX, g_fixedBtnCenterY, g_fixedBtnRadius);
            }
        }

        // 展开信息
        if (g_isExpanded) {
            int infoStartY = g_collapsedH;
            int infoAreaHeight = height - infoStartY;
            int lineHeight = (int)(g_fixedTimeFontSize * 1.2f);
            int textY = infoStartY + (infoAreaHeight - lineHeight) / 2;

            Font infoFont(g_pEmojiFontFamily, (REAL)(g_fixedTimeFontSize * 0.7f), FontStyleBold, UnitPixel);
            SolidBrush infoBrush(theme.text);  // 与时间文字颜色一致

            StringFormat centerFormat;
            centerFormat.SetAlignment(StringAlignmentCenter);
            centerFormat.SetLineAlignment(StringAlignmentCenter);

            const WCHAR* infoTexts[3] = { g_wifiInfo, g_bluetoothInfo, g_batteryInfo };
            int itemWidth = width / 3;

            for (int i = 0; i < 3; i++) {
                int x = i * itemWidth;
                RectF itemRect((REAL)x, (REAL)textY, (REAL)itemWidth, (REAL)lineHeight);
                graphics.DrawString(infoTexts[i], -1, &infoFont, itemRect, &centerFormat, &infoBrush);
            }
        }
    }
}

static void ApplyCornerAlpha(int width, int height, int radius) {
    struct Pixel { BYTE b, g, r, a; };
    Pixel* pixels = (Pixel*)g_pBits;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            bool inside = true;
            int dx = 0, dy = 0;
            if (x < radius && y < radius) {
                dx = radius - x;
                dy = radius - y;
                if (dx * dx + dy * dy > radius * radius)
                    inside = false;
            }
            else if (x >= width - radius && y < radius) {
                dx = x - (width - radius);
                dy = radius - y;
                if (dx * dx + dy * dy > radius * radius)
                    inside = false;
            }
            else if (x < radius && y >= height - radius) {
                dx = radius - x;
                dy = y - (height - radius);
                if (dx * dx + dy * dy > radius * radius)
                    inside = false;
            }
            else if (x >= width - radius && y >= height - radius) {
                dx = x - (width - radius);
                dy = y - (height - radius);
                if (dx * dx + dy * dy > radius * radius)
                    inside = false;
            }

            if (!inside) {
                Pixel& p = pixels[y * width + x];
                p.r = 0;
                p.g = 0;
                p.b = 0;
                p.a = 0;
            }
        }
    }
}

static void ResizeBitmap(int width, int height) {
    if (g_hBitmap) DeleteObject(g_hBitmap);
    if (g_hMemDC) DeleteDC(g_hMemDC);
    g_hMemDC = CreateCompatibleDC(NULL);
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g_hBitmap = CreateDIBSection(g_hMemDC, &bmi, DIB_RGB_COLORS, &g_pBits, NULL, 0);
    SelectObject(g_hMemDC, g_hBitmap);
    g_winWidth = width;
    g_winHeight = height;
}

static void UpdateLayeredWindowContent() {
    HDC hdcScreen = GetDC(NULL);
    POINT ptSrc = { 0, 0 };
    SIZE sz = { g_winWidth, g_winHeight };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hWnd, hdcScreen, NULL, &sz, g_hMemDC, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(NULL, hdcScreen);
}

bool IsWifiConnected() {
    HANDLE hClient = NULL;
    DWORD dwMaxClient = 2, dwCurVersion = 0;
    DWORD dwResult = WlanOpenHandle(dwMaxClient, NULL, &dwCurVersion, &hClient);
    if (dwResult != ERROR_SUCCESS) return false;

    PWLAN_INTERFACE_INFO_LIST pIfList = NULL;
    dwResult = WlanEnumInterfaces(hClient, NULL, &pIfList);
    if (dwResult != ERROR_SUCCESS) {
        WlanCloseHandle(hClient, NULL);
        return false;
    }

    bool isConnected = false;
    for (DWORD i = 0; i < pIfList->dwNumberOfItems; i++) {
        PWLAN_INTERFACE_INFO pIfInfo = &pIfList->InterfaceInfo[i];
        PWLAN_AVAILABLE_NETWORK_LIST pNetworkList = NULL;
        dwResult = WlanGetAvailableNetworkList(hClient, &pIfInfo->InterfaceGuid, 0, NULL, &pNetworkList);
        if (dwResult == ERROR_SUCCESS && pNetworkList) {
            for (DWORD j = 0; j < pNetworkList->dwNumberOfItems; j++) {
                if (pNetworkList->Network[j].dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) {
                    isConnected = true;
                    break;
                }
            }
            WlanFreeMemory(pNetworkList);
        }
        if (isConnected) break;
    }
    WlanFreeMemory(pIfList);
    WlanCloseHandle(hClient, NULL);
    return isConnected;
}

void OpenWifiSettings() { ShellExecuteW(NULL, L"open", L"ms-settings:network-wifi", NULL, NULL, SW_SHOWNORMAL); }
void OpenBluetoothSettings() { ShellExecuteW(NULL, L"open", L"ms-settings:bluetooth", NULL, NULL, SW_SHOWNORMAL); }
void OnWifiButtonClick() { OpenWifiSettings(); UpdateInfoPanel(); }
void OnBluetoothButtonClick() { OpenBluetoothSettings(); }
void OnEmptyButtonClick() {}

void UpdateWifiButtonState() {
    if (g_buttons[BTN_WIFI]) {
        g_buttons[BTN_WIFI]->SetActive(IsWifiConnected());
        InvalidateRect(g_hWnd, NULL, FALSE);
    }
}

// SMTC 初始化与清理
void InitSMTC() {
    try {
        auto async = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        if (!async) return;
        async.Completed([&](auto&& asyncInfo, auto&&) {
            try {
                g_sessionManager = asyncInfo.GetResults();
                if (!g_sessionManager) return;

                g_currentSession = g_sessionManager.GetCurrentSession();
                auto updateMediaInfo = [&]() {
                    if (g_currentSession) {
                        try {
                            auto props = g_currentSession.TryGetMediaPropertiesAsync().get();
                            if (props) {
                                g_musicTitle = props.Title();
                                g_musicArtist = props.Artist();
                                g_hasMusic = !g_musicTitle.empty();
                            }
                            auto playbackInfo = g_currentSession.GetPlaybackInfo();
                            g_isPlaying = (playbackInfo.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                        }
                        catch (...) {
                            g_hasMusic = false;
                            g_isPlaying = false;
                        }
                    }
                    else {
                        g_hasMusic = false;
                        g_isPlaying = false;
                    }
                    InvalidateRect(g_hWnd, NULL, FALSE);
                    };

                if (g_currentSession) {
                    updateMediaInfo();
                    g_mediaPropertiesChangedToken = g_currentSession.MediaPropertiesChanged([&](auto&&, auto&&) {
                        updateMediaInfo();
                        });
                    g_playbackInfoChangedToken = g_currentSession.PlaybackInfoChanged([&](auto&&, auto&&) {
                        updateMediaInfo();
                        });
                }

                g_sessionAddedToken = g_sessionManager.SessionsChanged([&](auto&&, auto&&) {
                    g_currentSession = g_sessionManager.GetCurrentSession();
                    updateMediaInfo();
                    });
            }
            catch (...) {
                g_sessionManager = nullptr;
            }
            });
    }
    catch (...) {
        g_sessionManager = nullptr;
    }
}

void CleanupSMTC() {
    if (g_currentSession) {
        try {
            if (g_mediaPropertiesChangedToken.value)
                g_currentSession.MediaPropertiesChanged(g_mediaPropertiesChangedToken);
            if (g_playbackInfoChangedToken.value)
                g_currentSession.PlaybackInfoChanged(g_playbackInfoChangedToken);
        }
        catch (...) {}
        g_currentSession = nullptr;
    }
    if (g_sessionManager) {
        try {
            if (g_sessionAddedToken.value)
                g_sessionManager.SessionsChanged(g_sessionAddedToken);
        }
        catch (...) {}
        g_sessionManager = nullptr;
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SetTimer(hWnd, 1, TIMER_INTERVAL, NULL);
        SetTimer(hWnd, TIMER_INFO_UPDATE, INFO_UPDATE_INTERVAL, NULL);
        UpdateTimeString();
        g_lastTimeUpdate = GetTickCount();

        g_buttons[BTN_BLUETOOTH] = new CircleButton(L"🎧", OnBluetoothButtonClick);
        g_buttons[BTN_WIFI] = new CircleButton(L"🌏", OnWifiButtonClick);
        g_buttons[BTN_EMPTY1] = new CircleButton(L"🔇", OnMuteButtonClick);
        g_buttons[BTN_EMPTY2] = new CircleButton(L"🌓", OnThemeToggleClick);
        g_buttons[BTN_WIFI]->SetActive(IsWifiConnected());
        g_buttons[BTN_EMPTY1]->SetActive(g_isMuted);

        UpdateWindowSizes();
        InitFixedUISizes();
        ResizeBitmap(g_collapsedW, g_collapsedH);

        InitSMTC();

        // 初始化托盘图标
        g_nid.cbSize = sizeof(NOTIFYICONDATAW);
        g_nid.hWnd = hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wcscpy_s(g_nid.szTip, L"动态岛设置");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        // 创建右键菜单
        g_hTrayMenu = CreatePopupMenu();
        {
            bool autoStart = IsAutoStartEnabled();
            AppendMenuW(g_hTrayMenu, MF_STRING | (autoStart ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_AUTOSTART, L"开机自启动(&S)");
            AppendMenuW(g_hTrayMenu, MF_STRING | (g_isMouseTransparent ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_MOUSE_TRANSPARENT, L"鼠标穿透(&T)");
        }
        AppendMenuW(g_hTrayMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(g_hTrayMenu, MF_STRING, 1001, L"退出程序(&X)");

        return 0;
    }
    case WM_TIMER:
        if (wParam == 1) {
            DWORD now = GetTickCount();
            if (now - g_lastTimeUpdate >= 1000) {
                UpdateTimeString();
                g_lastTimeUpdate = now;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            for (int i = 0; i < BTN_COUNT; i++) if (g_buttons[i]) g_buttons[i]->Update();
            UpdateAnimation();
            CheckMouseHover();

            if (!g_isExpanded && g_hasMusic && g_isPlaying) {
                if (now - g_lastWaveUpdate >= WAVE_UPDATE_INTERVAL) {
                    g_lastWaveUpdate = now;
                    g_wavePhase += 0.2f;
                    for (int i = 0; i < WAVE_BAR_COUNT; i++) {
                        g_waveAmplitudes[i] = 0.3f + 0.5f * (0.5f + 0.5f * sinf(g_wavePhase + i * 0.8f));
                    }
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
        }
        else if (wParam == TIMER_INFO_UPDATE) {
            UpdateInfoPanel();
        }

        {
            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = rc.right, h = rc.bottom;
            if (w != g_winWidth || h != g_winHeight) ResizeBitmap(w, h);
            DrawContentToBitmap(w, h);
            ApplyCornerAlpha(w, h, CORNER_RADIUS);
            UpdateLayeredWindowContent();
        }
        return 0;

    case WM_SIZE:
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;

    case WM_DISPLAYCHANGE:
        UpdateWindowSizes();
        InitFixedUISizes();
        {
            int newW = g_isExpanded ? g_expandedW : g_collapsedW;
            int newH = g_isExpanded ? g_expandedH : g_collapsedH;
            int newX, newY;
            GetTopCenterPosition(newW, newH, &newX, &newY);
            SetWindowPos(hWnd, HWND_TOPMOST, newX, newY, newW, newH, SWP_NOACTIVATE);
        }
        return 0;

    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);
        int width = rc.right;
        bool needRedraw = false;
        for (int i = 0; i < BTN_COUNT; i++) {
            if (g_buttons[i]) {
                int centerX = GetButtonCenterX(width, (ButtonId)i);
                if (g_buttons[i]->HandleMouse(msg, pt.x, pt.y, centerX, g_fixedBtnCenterY, g_fixedBtnRadius))
                    needRedraw = true;
            }
        }
        if (needRedraw) InvalidateRect(hWnd, NULL, FALSE);

        if (!g_isAnimating && (GetKeyState(VK_LBUTTON) & 0x8000)) {
            if (g_pressedOnButton) {
                return 0;
            }
            if (!g_longPressTriggered && !g_isDragging) {
                DWORD now = GetTickCount();
                if (now - g_dragStartTime >= LONG_PRESS_TIME_MS) {
                    POINT current;
                    GetCursorPos(&current);
                    int dx = abs(current.x - g_dragStart.x);
                    int dy = abs(current.y - g_dragStart.y);
                    if (dx > DRAG_THRESHOLD_PX || dy > DRAG_THRESHOLD_PX) {
                        g_longPressTriggered = TRUE;
                        g_isDragging = TRUE;
                        SetCursor(LoadCursor(NULL, IDC_SIZEALL));
                    }
                }
            }
            if (g_isDragging) {
                POINT current;
                GetCursorPos(&current);
                int deltaX = current.x - g_dragStart.x;
                if (deltaX != 0) {
                    RECT rcWnd;
                    GetWindowRect(hWnd, &rcWnd);
                    int newX = rcWnd.left + deltaX;
                    SetWindowPos(hWnd, HWND_TOPMOST, newX, rcWnd.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                    g_dragStart = current;
                }
                return 0;
            }
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);
        int width = rc.right;
        bool handled = false;
        for (int i = 0; i < BTN_COUNT; i++) {
            if (g_buttons[i]) {
                int centerX = GetButtonCenterX(width, (ButtonId)i);
                if (g_buttons[i]->HandleMouse(msg, pt.x, pt.y, centerX, g_fixedBtnCenterY, g_fixedBtnRadius))
                    handled = true;
            }
        }
        if (handled) {
            g_pressedOnButton = TRUE;          // 记录按在了按钮上
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }

        if (!g_isAnimating) {
            g_dragStart = pt;
            ClientToScreen(hWnd, &g_dragStart);
            g_dragStartTime = GetTickCount();
            g_longPressTriggered = FALSE;
            g_isDragging = FALSE;
            SetCapture(hWnd);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rc;
        g_pressedOnButton = FALSE;
        GetClientRect(hWnd, &rc);
        int width = rc.right;
        bool handled = false;
        for (int i = 0; i < BTN_COUNT; i++) {
            if (g_buttons[i]) {
                int centerX = GetButtonCenterX(width, (ButtonId)i);
                if (g_buttons[i]->HandleMouse(msg, pt.x, pt.y, centerX, g_fixedBtnCenterY, g_fixedBtnRadius))
                    handled = true;
            }
        }
        if (handled) {
            InvalidateRect(hWnd, NULL, FALSE);
            ReleaseCapture();
            g_isDragging = FALSE;
            g_longPressTriggered = FALSE;
            return 0;
        }
        if (g_isDragging) {
            g_isDragging = FALSE;
            g_longPressTriggered = FALSE;
            ReleaseCapture();
            return 0;
        }
        ReleaseCapture();
        g_isDragging = FALSE;
        g_longPressTriggered = FALSE;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        KillTimer(hWnd, 1);
        for (int i = 0; i < BTN_COUNT; i++) delete g_buttons[i];
        DeleteObject(g_hBitmap);
        DeleteDC(g_hMemDC);
        CleanupSMTC();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_hTrayMenu) DestroyMenu(g_hTrayMenu);
        PostQuitMessage(0);
        return 0;

    case WM_SETTINGCHANGE:
        if (lParam && wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0) {
            g_isDarkTheme = GetSystemThemeIsDark();
            g_pCurrentTheme = g_isDarkTheme ? &DarkTheme : &LightTheme;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            TrackPopupMenu(g_hTrayMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            PostMessage(hWnd, WM_NULL, 0, 0);
        }
        break;

    case WM_COMMAND: {
        if (LOWORD(wParam) == 1001) {
            PostQuitMessage(0);
        }
        else if (LOWORD(wParam) == ID_TRAY_AUTOSTART) {
            bool currentlyEnabled = IsAutoStartEnabled();
            SetAutoStart(!currentlyEnabled);

            CheckMenuItem(g_hTrayMenu, ID_TRAY_AUTOSTART,
                !currentlyEnabled ? MF_CHECKED : MF_UNCHECKED);

            WCHAR msg[256];
            wsprintfW(msg, L"已%s开机自启动", !currentlyEnabled ? L"启用" : L"取消");
            g_nid.uFlags = NIF_INFO;
            wcscpy_s(g_nid.szInfoTitle, L"DynamicIsland");
            wcscpy_s(g_nid.szInfo, msg);
            g_nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        }
        else if (LOWORD(wParam) == ID_TRAY_MOUSE_TRANSPARENT) {
            BOOL newState = !g_isMouseTransparent;
            SetMouseTransparent(newState);
            CheckMenuItem(g_hTrayMenu, ID_TRAY_MOUSE_TRANSPARENT,
                newState ? MF_CHECKED : MF_UNCHECKED);
        }
        break;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    SetProcessDPIAware();

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    g_pFontFamily = new FontFamily(L"Segoe UI");
    g_pEmojiFontFamily = new FontFamily(L"Segoe UI Emoji");
    if (!g_pFontFamily->IsAvailable()) {
        delete g_pFontFamily;
        g_pFontFamily = new FontFamily(L"Arial");
    }
    if (!g_pEmojiFontFamily->IsAvailable()) {
        delete g_pEmojiFontFamily;
        g_pEmojiFontFamily = new FontFamily(L"Segoe UI");
    }

    UpdateWindowSizes();
    InitFixedUISizes();

    g_isDarkTheme = GetSystemThemeIsDark();
    g_pCurrentTheme = g_isDarkTheme ? &DarkTheme : &LightTheme;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"DynamicIslandWiFi";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassExW(&wc);

    int x, y;
    GetTopCenterPosition(g_collapsedW, g_collapsedH, &x, &y);
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"DynamicIslandWiFi",
        L"DynamicIsland",
        WS_POPUP,
        x, y, g_collapsedW, g_collapsedH,
        NULL, NULL, hInst, NULL
    );
    if (!hwnd) return 1;

    g_hWnd = hwnd;
    SetMouseTransparent(FALSE);

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    delete g_pFontFamily;
    delete g_pEmojiFontFamily;
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}