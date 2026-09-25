#define UNICODE
#define _UNICODE

/**************************************************************************************************
 * System libs
 **************************************************************************************************/
#include <windows.h>
#include <windowsx.h>
#include <Shlobj.h>
#include <wincodec.h>
#include <Tlhelp32.h>
#include <CommCtrl.h>
#include <strsafe.h>
#include <dwmapi.h>
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")
#include <winhttp.h>
#pragma comment(lib, "Winhttp.lib")
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

 /**************************************************************************************************
  * Standard libs
  **************************************************************************************************/
#include <cstdio>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <mutex>
#include <deque>

#include "resource.h" // for version info
#include "stacky_config.h" // hidden per-folder .stacky-config (Configuration window)
#include "stacky_config_ui.h" // advanced Configuration window (RunStackyConfigWindow)

#include <wingdi.h>
#pragma comment(lib, "Msimg32.lib")


  /**************************************************************************************************
   * Simple types and constants
   **************************************************************************************************/
typedef wchar_t                 Char;
typedef unsigned char           Byte;
typedef __time64_t              Time;
typedef std::wstring            String;
typedef std::vector<String>     StringList;

const String CACHE_FILE_NAME = L"!stacky.cache";
const String STACKY_EXEC_NAME = L"stacky-plus.exe";
const Char* STACKY_WINDOW_NAME = L"stacky";
const Char* STACKY_POPUP_CLASS = L"stacky_popup";
const Char* STACKY_GRID_CLASS  = L"stacky_grid";
const Char* STACKY_TIP_CLASS   = L"stacky_tip";
const Char* DIR_SEP = L"\\";
const String SUBMENU_SUFFIX = L".submenu";
const String SUBMENU_MINI_SUFFIX = L".submenu-mini";
const int NORMAL_ICON_PX = 32;
const int MINI_ICON_PX = 16;
const String DESKTOP_INI = L"desktop.ini";
const String FAVICON_FOLDER = L"favicon-icon-web";
const DWORD CACHE_VERSION = 14; // Increment this when cache format changes

// Cached system color-mode (light/dark) and accent color, used as the
// "system" default theme (--light-mode / --dark-mode override this).
// Read once and cached; refreshed only when a WM_SETTINGCHANGE /
// WM_DWMCOLORIZATIONCOLORCHANGED notification is received (see PopupWndProc
// and GridWndProc), never polled.
struct SystemThemeCache {
	bool     initialized = false;
	bool     dark        = false;
	COLORREF accent      = RGB(0, 120, 215); // fallback Windows blue
};
static SystemThemeCache g_sysTheme;

static bool ReadSystemAppsUseLightTheme() {
	HKEY hKey;
	DWORD value = 1, size = sizeof(value);
	if (RegOpenKeyEx(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		RegQueryValueEx(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&value, &size);
		RegCloseKey(hKey);
	}
	return value != 0; // non-zero = light
}

static COLORREF ReadSystemAccentColor() {
	DWORD colorizationColor = 0; BOOL opaque = FALSE;
	if (SUCCEEDED(DwmGetColorizationColor(&colorizationColor, &opaque))) {
		BYTE r = (BYTE)((colorizationColor >> 16) & 0xFF);
		BYTE g = (BYTE)((colorizationColor >> 8) & 0xFF);
		BYTE b = (BYTE)(colorizationColor & 0xFF);
		return RGB(r, g, b);
	}
	return RGB(0, 120, 215);
}

static void RefreshSystemThemeCache() {
	g_sysTheme.dark        = !ReadSystemAppsUseLightTheme();
	g_sysTheme.accent      = ReadSystemAccentColor();
	g_sysTheme.initialized = true;
}

static const SystemThemeCache& GetSystemTheme() {
	if (!g_sysTheme.initialized) RefreshSystemThemeCache();
	return g_sysTheme;
}

// Global hook handle for menu window creation
HHOOK g_hMenuHook = nullptr;
HWND g_lastMenuWindow = nullptr;
int g_desiredMenuX = 0;
int g_desiredMenuY = 0;
int g_desiredMenuHeight = 0;
bool g_isFirstMenu = true;

// Forward declarations
struct App;
LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK GridWndProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK TipWndProc  (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void RestoreFocusToRootMenuWindow(HWND owner);
bool IsDescendantMenuWindow(HWND ancestor, HWND candidate);

// Grid display modes
enum GridMode {
	GRID_NONE    = 0,  // standard list popup
	GRID_ICON    = 1,  // NN cols, icons only, tooltip immediately
	GRID_NAME    = 2,  // NN cols, icons + truncated name below, tooltip 300 ms
	GRID_CASCADE = 3,  // 2 cols, icons only, submenus open as 1-col grid
	GRID_CASCADE_NAME = 4,  // 2 cols, icons + truncated name below, submenus open as 1-col grid with names
	GRID_F2 = 5,  // row-based: items with submenu on row 1, others on row 2; submenus open upward
	GRID_F2_NAME = 6,  // same as GRID_F2, with truncated name below each icon
	GRID_NAME_RIGHT = 10, // iconmenu-NN-name-right: NN cols, name to the right of the icon, default text size, no gap

	// --singlesubmenu mode: used ONLY for the grid window opened for a .submenu folder
	// (never for the root menu, and these submenus never have child submenus).
	GRID_SS_ICON       = 7,  // variant 1 (".icononly-NN"): plain NN-column icon grid, no names
	GRID_SS_NAME_RIGHT = 8,  // variant 2 (".NN-name-right"): NN columns, name to the right of the icon
	GRID_SS_NAME_BELOW = 9,  // variant 3 (".NN-name-below"): NN columns, name below the icon (1 line)
};

// Overall menu color-theme mode.
enum ThemeMode {
	THEME_SYSTEM = 0, // follow the system color mode/accent (cached, event-driven)
	THEME_LIGHT  = 1, // --light-mode: force current light appearance
	THEME_DARK   = 2, // --dark-mode: force current dark appearance
};

// Timer IDs used by PopupWndProc
static const UINT_PTR POPUP_TIMER_CLOSE_CHILD = 10;  // grace period before destroying child submenu
static const UINT_PTR POPUP_TIMER_OPEN_CHILD  = 11;  // debounce delay before opening a hover-child submenu
static const UINT     POPUP_OPEN_CHILD_DELAY_MS      = 80; // default "linger" delay when sweeping across rows
static const UINT     POPUP_OPEN_CHILD_FAST_DELAY_MS = 12; // near-instant delay when the cursor is clearly heading into the submenu
// Velocity threshold (pixels/ms) used to tell a deliberate move toward the
// submenu arrow apart from a fast vertical sweep across unrelated rows: if
// the cursor's vertical speed is below this and/or horizontal speed exceeds
// vertical speed, the row is opened almost immediately instead of waiting
// out the full debounce (mirrors the direction heuristic classic Win32/
// Explorer menus use for hover-opening submenus).
static const double   POPUP_SUB_OPEN_VY_THRESHOLD = 0.35;

// State passed as lpCreateParams when creating each popup window.
struct PopupState {
	App*   app;
	String prefix;            // empty for root, "FolderName.submenu\\" for submenus
	HWND   parentHwnd = nullptr; // non-null for hover-opened child popups
	HWND   childHwnd  = nullptr; // currently open hover-child popup
	int    hotSubIdx  = -1;      // cache item index of the hover-child
	int    hotItem    = -1;      // hovered row index, replaces g_hotItem
	bool   closePending = false; // WM_MOUSELEAVE fired but grace period active
	// Debounces opening a hover-child submenu: creating/destroying a
	// top-level popup window on every WM_MOUSEMOVE that crosses a submenu
	// row is expensive enough (window creation + DWM composition) that
	// quickly sweeping the mouse across a list with several submenus makes
	// the highlight rectangle appear to visually "split" - the side near
	// the submenu arrow lags behind the rest of the row while Windows
	// finishes creating/tearing down the child popup. Instead of opening
	// immediately on hover, a short timer is armed; the child popup is only
	// actually created if the cursor still sits on the same submenu row
	// once the timer fires.
	int    pendingSubIdx = -1;     // submenu row index a timer is currently pending for (-1 = none)
	String pendingSubPrefix;       // submenu prefix matching pendingSubIdx
	int    pendingSubY    = 0;     // row top (client y) captured when the timer was armed

	// Tracks the previous WM_MOUSEMOVE point/time so the hover logic can tell
	// a deliberate move toward a submenu arrow (mostly horizontal, unhurried)
	// apart from a fast vertical sweep across several rows, and shorten the
	// debounce delay accordingly (see POPUP_SUB_OPEN_VY_THRESHOLD).
	POINT  lastMovePt  = { -1, -1 };
	DWORD  lastMoveTick = 0;
};

// Hook procedure to intercept menu window creation
LRESULT CALLBACK MenuWindowHook(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HCBT_CREATEWND) {
		CBT_CREATEWND* pCreateWnd = (CBT_CREATEWND*)lParam;
		if (pCreateWnd && pCreateWnd->lpcs) {
			if ((pCreateWnd->lpcs->style & WS_POPUP) &&
				(pCreateWnd->lpcs->dwExStyle & WS_EX_TOOLWINDOW)) {
				HWND hMenuWindow = (HWND)wParam;
				g_lastMenuWindow = hMenuWindow;

				const DWORD DWMWA_CORNER_PREFERENCE = 33;
				const DWORD DWMWCP_ROUND = 2;
				DWORD cornerPreference = DWMWCP_ROUND;
				::DwmSetWindowAttribute(hMenuWindow, DWMWA_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
			}
		}
	}
	else if (nCode == HCBT_ACTIVATE) {
		HWND hWnd = (HWND)wParam;
		if (hWnd == g_lastMenuWindow && (g_desiredMenuX != 0 || g_desiredMenuY != 0)) {
			::SetWindowPos(hWnd, HWND_TOP, g_desiredMenuX, g_desiredMenuY, 0, 0,
				SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
		}
	}
	return CallNextHookEx(g_hMenuHook, nCode, wParam, lParam);
}

enum {
	WM_BASE = WM_USER + 100,
	WM_OPEN_TARGET_FOLDER = WM_BASE + 1,
	WM_MENU_ITEM = WM_BASE + 2,
	WM_OPEN_LOCATION = WM_BASE + 3,

	// Right-click context menu (submenu-folder icons), used by PopupWndProc/GridWndProc.
	WM_CTX_OPEN_SUBFOLDER     = WM_BASE + 10,
	WM_CTX_OPEN_MENU_FOLDER   = WM_BASE + 11,
	WM_CTX_OPEN_STACKY_FOLDER = WM_BASE + 12,
	WM_CTX_CLOSE_MENU         = WM_BASE + 13,

	// Right-click context menu (shortcut icons), used by PopupWndProc/GridWndProc.
	WM_CTX_OPEN_SHORTCUT_LOCATION = WM_BASE + 14,
	WM_CTX_RUN_AS_ADMIN           = WM_BASE + 15,
	WM_CTX_OPEN_SETTINGS          = WM_BASE + 16,

	APP_EXIT_DELAY = 3 * 1000,

	ERR_PATH_MISSING = 401,
	ERR_PATH_INVALID = 402,
	ERR_PARAM_UNKNOWN = 403,
};


/**************************************************************************************************
 * COM init (once)
 **************************************************************************************************/
struct ComInit {
	HRESULT hr;
	ComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
	~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};


/**************************************************************************************************
 * The meat
 **************************************************************************************************/

struct Util {

	static String rtrim(const String& target, const String& trim) {
		size_t cutoff_pos = target.size() - trim.size();
		return target.rfind(trim) == cutoff_pos ? target.substr(0, cutoff_pos) : target;
	}
	static String ltrim(const String& target, const String& trim) {
		size_t cutoff_pos = trim.size();
		return target.find(trim) != String::npos ? target.substr(cutoff_pos) : target;
	}
	static String trim(const String& target, const String& trim) {
		return rtrim(ltrim(target, trim), trim);
	}
	// Remove a leading sort-prefix of the form %NN% (two digits between %).
	// e.g. "%01%My App" -> "My App", "%12%Folder" -> "Folder".
	// Items without the prefix are returned unchanged.
	static String StripSortPrefix(const String& s) {
		if (s.size() >= 5 &&
			s[0] == L'%' &&
			iswdigit(s[1]) &&
			iswdigit(s[2]) &&
			s[3] == L'%')
		{
			return s.substr(4);
		}
		return s;
	}
	static String quote(const String& target) {
		return L"\"" + target + L"\"";
	}
	static bool ends_with(const String& target, const String& ending)
	{
		if (target.length() >= ending.length())
		{
			return (0 == target.compare(target.length() - ending.length(), ending.length(), ending));
		}
		else
		{
			return false;
		}
	}
	// Returns true if `leaf` ends with ".icononly-NN" where NN is 1-2 digits,
	// and removes that entire trailing token from `leaf` (in place on success).
	// Returns false and leaves `leaf` unchanged otherwise.
	static bool StripIconOnlyNN(String& leaf) {
		const String pfx = L".icononly-";
		if (leaf.size() < pfx.size() + 1) return false;
		size_t d = leaf.size();
		while (d > 0 && leaf[d - 1] >= L'0' && leaf[d - 1] <= L'9') --d;
		size_t digitCount = leaf.size() - d;
		if (digitCount < 1 || digitCount > 2) return false;
		if (d < pfx.size() || leaf.compare(d - pfx.size(), pfx.size(), pfx) != 0) return false;
		leaf.erase(d - pfx.size());
		return true;
	}
	// Strips a trailing ".NN-name-right" or ".NN-name-below" (NN = 1-2 digits).
	static bool StripNNNameLayout(String& leaf, bool below) {
		const String suf = below ? String(L"-name-below") : String(L"-name-right");
		if (leaf.size() < suf.size() + 2) return false; // ".N" + suffix
		if (!ends_with(leaf, suf)) return false;
		size_t before = leaf.size() - suf.size(); // index of the '-' that starts the suffix
		size_t d = before;
		while (d > 0 && leaf[d - 1] >= L'0' && leaf[d - 1] <= L'9') --d;
		size_t digitCount = before - d;
		if (digitCount < 1 || digitCount > 2 || d == 0 || leaf[d - 1] != L'.') return false;
		leaf.erase(d - 1);
		return true;
	}
	// --singlesubmenu layout token at the end of a folder leaf name.
	enum SSLayoutKind { SS_LAYOUT_NONE = 0, SS_LAYOUT_ICON = 1, SS_LAYOUT_NAME_RIGHT = 2, SS_LAYOUT_NAME_BELOW = 3 };
	struct SSLayout {
		SSLayoutKind kind = SS_LAYOUT_NONE;
		int cols = 3;
	};
	static SSLayout ParseSSLayout(const String& leaf) {
		SSLayout r;
		if (ends_with(leaf, L"-name-right")) {
			size_t before = leaf.size() - 11;
			size_t d = before;
			while (d > 0 && leaf[d - 1] >= L'0' && leaf[d - 1] <= L'9') --d;
			size_t digitCount = before - d;
			if (digitCount >= 1 && digitCount <= 2 && d > 0 && leaf[d - 1] == L'.') {
				long n = wcstol(leaf.c_str() + d, nullptr, 10);
				if (n >= 1 && n <= 99) { r.kind = SS_LAYOUT_NAME_RIGHT; r.cols = (int)n; return r; }
			}
			return r;
		}
		if (ends_with(leaf, L"-name-below")) {
			size_t before = leaf.size() - 11;
			size_t d = before;
			while (d > 0 && leaf[d - 1] >= L'0' && leaf[d - 1] <= L'9') --d;
			size_t digitCount = before - d;
			if (digitCount >= 1 && digitCount <= 2 && d > 0 && leaf[d - 1] == L'.') {
				long n = wcstol(leaf.c_str() + d, nullptr, 10);
				if (n >= 1 && n <= 99) { r.kind = SS_LAYOUT_NAME_BELOW; r.cols = (int)n; return r; }
			}
			return r;
		}
		size_t d = leaf.size();
		while (d > 0 && leaf[d - 1] >= L'0' && leaf[d - 1] <= L'9') --d;
		size_t digitCount = leaf.size() - d;
		const String pfx = L".icononly-";
		if (digitCount >= 1 && digitCount <= 2 && d >= pfx.size() &&
			leaf.compare(d - pfx.size(), pfx.size(), pfx) == 0) {
			long n = wcstol(leaf.c_str() + d, nullptr, 10);
			if (n >= 1 && n <= 99) { r.kind = SS_LAYOUT_ICON; r.cols = (int)n; return r; }
		}
		return r;
	}
	// Returns the last path segment of `name` (after the last DIR_SEP), i.e. the
	// actual folder/file leaf name. For nested items, Cache::Item::name stores the
	// FULL relative path (e.g. "A.submenu\\B.submenu"), so the .submenu suffix
	// helpers below must only ever inspect the leaf segment, never the whole path
	// (otherwise a `.find(".submenu")` on an ancestor segment could shadow the
	// real, deeper .submenu terminator).
	static String LastPathSegment(const String& name) {
		size_t pos = name.rfind(DIR_SEP);
		if (pos == String::npos) return name;
		return name.substr(pos + String(DIR_SEP).size());
	}
	// Strips ".submenu" / ".submenu-mini" and --singlesubmenu layout tokens
	// (.icononly-NN, .NN-name-right, .NN-name-below, .mini) so the display name
	// is the clean folder name. Only the last path segment is considered.
	static String StripSubmenuSuffix(const String& target) {
		String leaf = LastPathSegment(target);
		String prefixPath = target.substr(0, target.size() - leaf.size());
		
		// Legacy: .submenu-mini
		if (ends_with(leaf, SUBMENU_MINI_SUFFIX)) return prefixPath + rtrim(leaf, SUBMENU_MINI_SUFFIX);
		// .submenu optionally followed by layout variants
		size_t pos = leaf.find(SUBMENU_SUFFIX);
		if (pos != String::npos) {
			size_t afterPos = pos + SUBMENU_SUFFIX.size();
			if (afterPos == leaf.size() || leaf[afterPos] == L'-' || leaf[afterPos] == L'.')
				return prefixPath + leaf.substr(0, pos);
		}
		// Plain folder: strip .icononly-NN / .NN-name-right / .NN-name-below / .mini
		String s = leaf;
		while (true) {
			if (StripNNNameLayout(s, false)) continue;
			if (StripNNNameLayout(s, true)) continue;
			if (StripIconOnlyNN(s)) continue;
			if (ends_with(s, L".mini")) {
				s = s.substr(0, s.size() - 5);
				continue;
			}
			break;
		}
		if (s == leaf) return target;
		return prefixPath + s;
	}
	// True if `name` is a submenu folder: ".submenu" / ".submenu-mini" (default
	// menu mode), or a --singlesubmenu layout token without ".submenu"
	// (.icononly-NN, .NN-name-right, .NN-name-below), or a trailing ".mini".
	static bool IsSubmenuFolderName(const String& name) {
		String leaf = LastPathSegment(name);
		if (ends_with(leaf, SUBMENU_MINI_SUFFIX)) return true;
		size_t pos = leaf.find(SUBMENU_SUFFIX);
		if (pos != String::npos) {
			size_t afterPos = pos + SUBMENU_SUFFIX.size();
			if (afterPos == leaf.size() || leaf[afterPos] == L'-' || leaf[afterPos] == L'.')
				return true;
		}
		if (ParseSSLayout(leaf).kind != SS_LAYOUT_NONE) return true;
		if (ends_with(leaf, L".mini")) return true;
		return false;
	}
	// True if the folder name carries a "mini" token, either as the exact
	// legacy terminator (".submenu-mini") or combined with a --singlesubmenu
	// layout-variant suffix (e.g. ".submenu.mini.4-name-right"), or as a
	// standalone ".mini" token in a plain folder name (e.g. "Apps.mini").
	// Only the last path segment is checked (see IsSubmenuFolderName).
	static bool IsMiniSubmenuFolderName(const String& name) {
		String leaf = LastPathSegment(name);
		// Legacy: .submenu-mini or .submenu.mini / .submenu.mini-NN / .submenu.mini.NN
		size_t pos = leaf.find(SUBMENU_SUFFIX);
		if (pos != String::npos) {
			String rest = leaf.substr(pos + SUBMENU_SUFFIX.size());
			if (!rest.empty() && (rest[0] == L'-' || rest[0] == L'.')) rest.erase(0, 1);
			if (rest.compare(0, 4, L"mini") == 0 &&
				(rest.size() == 4 || rest[4] == L'-' || rest[4] == L'.')) return true;
		}
		// Plain folder: .mini token at end or followed by .NN / -name-* variants.
		// The ".mini" token is always dot-prefixed, so the character right before
		// the dot is simply the end of the base folder name (or of a previous
		// token) - it does not need to be '.' or '-' itself, only the character
		// AFTER the "mini" word needs to be a valid boundary ('-', '.', or end).
		if (ends_with(leaf, L".mini")) return true;
		size_t dotPos = leaf.find(L".mini");
		while (dotPos != String::npos) {
			size_t after = dotPos + 5;
			if (after == leaf.size() || leaf[after] == L'-' || leaf[after] == L'.') return true;
			dotPos = leaf.find(L".mini", dotPos + 1);
		}
		return false;
	}

	// Automatic hidden submenu detection: a folder that isn't explicitly named
	// as a submenu (no .submenu / .submenu-mini / --singlesubmenu layout token)
	// is still treated as a submenu if it contains at least one shortcut/item
	// or at least one nested folder. The name on disk is never modified; this
	// only affects in-memory classification used to build menus.
	static bool FolderHasShortcutOrSubfolder(const String& folder_path) {
		String search_path = rtrim(folder_path, DIR_SEP) + DIR_SEP + L"*";
		WIN32_FIND_DATA ffd = { 0 };
		HANDLE hfind = ::FindFirstFile(search_path.c_str(), &ffd);
		if (hfind == INVALID_HANDLE_VALUE) return false;
		bool found = false;
		do {
			String filename = ffd.cFileName;
			if (filename == L"." || filename == L".." || (ffd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) ||
				ends_with(filename, L".ignore") || filename == DESKTOP_INI)
				continue;
			if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				found = true;
				break;
			}
			if (ends_with(filename, L".lnk") || ends_with(filename, L".url") ||
				ends_with(filename, L".exe") || ends_with(filename, L".bat") ||
				ends_with(filename, L".cmd") || ends_with(filename, L".vbs") ||
				ends_with(filename, L".ico")) {
				found = true;
				break;
			}
		} while (::FindNextFile(hfind, &ffd) != 0);
		::FindClose(hfind);
		return found;
	}

	static void kill_other_stackies() {
		PROCESSENTRY32 entry = { 0 };
		entry.dwSize = sizeof(PROCESSENTRY32);
		BOOL found = false;
		HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPALL, 0);
		do {
			found = ::Process32Next(snapshot, &entry);
			if (entry.th32ProcessID != ::GetCurrentProcessId() && entry.szExeFile == STACKY_EXEC_NAME) {
				// PROCESS_TERMINATE is the least privilege needed here (and
				// is enough even for processes not owned by the current
				// user session in the common case), so prefer it over
				// PROCESS_ALL_ACCESS, which can fail to open with access
				// denied in situations where terminating would have
				// succeeded just fine (e.g. right after the Configuration
				// window - itself another stacky-plus.exe instance - closes
				// and the OS hasn't fully released the process object yet).
				HANDLE hOtherStacky = ::OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
				if (hOtherStacky) {
					::TerminateProcess(hOtherStacky, 0);
					::CloseHandle(hOtherStacky);
				}
				// If OpenProcess/TerminateProcess still fails (process may
				// have already exited on its own between the snapshot and
				// this call, or be in the middle of exiting), silently
				// ignore it instead of interrupting the user with an error
				// message box: it's harmless, since a stale/exiting other
				// instance doesn't prevent this one from opening its menu.
			}
		} while (found);
		::CloseHandle(snapshot);
	}
	static Time get_modified(const String& file_path) {
		struct _stat buf;
		return _wstat(file_path.c_str(), &buf) ? 0 : buf.st_mtime;
	}
	static int parse_cmd_line(const String& cmd_line, String& stack_path, String& opts) {
		stack_path = cmd_line;
		opts = L"";

		if (stack_path.size() < 1) {
			return ERR_PATH_MISSING;
		}

		// Check for options (arguments starting with --)
		size_t option_pos = stack_path.find(L" --");
		if (option_pos != String::npos) {
			opts = stack_path.substr(option_pos + 1); // Keep the space for trimming
			stack_path = stack_path.substr(0, option_pos);
		}

		DWORD attrs = ::GetFileAttributes(trim(stack_path, L"\"").c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			return ERR_PATH_INVALID;
		}
		return 0;
	}
	static void msgt(const String& title, const wchar_t* format, ...) {
		static Char msgBuf[4096] = { 0 };

		va_list arglist;
		va_start(arglist, format);
		vswprintf(msgBuf, format, arglist);
		va_end(arglist);

		::MessageBox(0, msgBuf, title.c_str(), MB_OK | MB_ICONINFORMATION);
	}

	static void msg(const wchar_t* format, ...) {
		static Char msgBuf[4096] = { 0 };

		va_list arglist;
		va_start(arglist, format);
		vswprintf(msgBuf, format, arglist);
		va_end(arglist);

		::MessageBox(0, msgBuf, L"Stacky", MB_OK | MB_ICONINFORMATION);
	}

	static HRESULT ResolveShortcut(HWND hwnd, LPCTSTR lpszLinkFile, LPTSTR lpszPath, int iPathBufferSize)
	{
		if (lpszPath == NULL)
			return E_INVALIDARG;

		*lpszPath = 0;

		// Get a pointer to the IShellLink interface. It is assumed that CoInitialize
		// has already been called.
		IShellLink* psl = NULL;
		HRESULT hres = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID*)&psl);
		if (SUCCEEDED(hres))
		{
			// Get a pointer to the IPersistFile interface.
			IPersistFile* ppf = NULL;
			hres = psl->QueryInterface(IID_IPersistFile, (void**)&ppf);
			if (SUCCEEDED(hres))
			{
				// Add code here to check return value from MultiByteWideChar
				// for success.

				// Load the shortcut.
#ifdef _UNICODE
				hres = ppf->Load(lpszLinkFile, STGM_READ);
#else
				WCHAR wsz[MAX_PATH] = { 0 };
				// Ensure that the string is Unicode.
				MultiByteToWideChar(CP_ACP, 0, lpszLinkFile, -1, wsz, MAX_PATH);
				hres = ppf->Load(wsz, STGM_READ);
#endif

				if (SUCCEEDED(hres))
				{
					// Resolve the link.
					hres = psl->Resolve(hwnd, 0);

					if (SUCCEEDED(hres))
					{
						// Get the path to the link target.
						TCHAR szGotPath[MAX_PATH] = { 0 };
						hres = psl->GetPath(szGotPath, _countof(szGotPath), NULL, SLGP_SHORTPATH);

						if (SUCCEEDED(hres))
						{
							hres = StringCbCopy(lpszPath, iPathBufferSize, szGotPath);
						}
					}
				}

				// Release the pointer to the IPersistFile interface.
				ppf->Release();
			}

			// Release the pointer to the IShellLink interface.
			psl->Release();
		}
		return hres;
	}

	// Read icon path from desktop.ini file
	static String ReadIconFromDesktopIni(const String& folder_path) {
		String desktop_ini_path = folder_path + DIR_SEP + DESKTOP_INI;
		DWORD attrs = ::GetFileAttributes(desktop_ini_path.c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			return L"";
		}

		Char icon_file[MAX_PATH] = { 0 };
		Char icon_resource[MAX_PATH] = { 0 };

		// Read IconFile from desktop.ini
		::GetPrivateProfileString(L".ShellClassInfo", L"IconFile", L"", icon_file, MAX_PATH, desktop_ini_path.c_str());
		::GetPrivateProfileString(L".ShellClassInfo", L"IconResource", L"", icon_resource, MAX_PATH, desktop_ini_path.c_str());

		// Prefer IconResource over IconFile
		String icon_path = icon_resource[0] != 0 ? icon_resource : icon_file;

		if (icon_path.empty()) {
			return L"";
		}

		// If path is relative, make it absolute relative to the folder
		if (icon_path.find(L":") == String::npos && icon_path[0] != L'\\') {
			icon_path = folder_path + DIR_SEP + icon_path;
		}

		return icon_path;
	}

	// Get monitor from cursor position
	static HMONITOR GetMonitorFromCursor() {
		POINT pt;
		::GetCursorPos(&pt);
		return ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	}

	// Get work area for a specific monitor
	static RECT GetWorkAreaForMonitor(HMONITOR hMonitor) {
		MONITORINFO mi = { sizeof(MONITORINFO) };
		::GetMonitorInfo(hMonitor, &mi);
		return mi.rcWork;
	}

	// Set rounded corners on window (Windows 11 style)
	static void SetWindowRoundedCorners(HWND hwnd) {
		if (!hwnd) return;

		// DWMWA_CORNER_PREFERENCE: 2 = DWMWCP_ROUND (Windows 11+)
		// This will gracefully fail on older Windows versions
		const DWORD DWMWA_CORNER_PREFERENCE = 33;
		const DWORD DWMWCP_ROUND = 2;

		DWORD cornerPreference = DWMWCP_ROUND;
		::DwmSetWindowAttribute(hwnd, DWMWA_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
	}

	// Get taskbar position and dimensions (always returns the PRIMARY monitor's
	// taskbar window). Kept for callers that explicitly want the primary
	// taskbar; multi-monitor-aware code should use GetTaskbarRectForMonitor().
	static RECT GetTaskbarRect() {
		HWND hTaskbar = ::FindWindow(L"Shell_TrayWnd", nullptr);
		RECT taskbarRect = { 0, 0, 0, 0 };

		if (hTaskbar) {
			::GetWindowRect(hTaskbar, &taskbarRect);
		}
		else {
			// Fallback: use SystemParametersInfo to get screen area
			RECT screenRect;
			::SystemParametersInfo(SPI_GETWORKAREA, 0, &screenRect, 0);

			// Assume taskbar is at bottom if not found
			taskbarRect.left = screenRect.left;
			taskbarRect.right = screenRect.right;
			taskbarRect.top = screenRect.bottom;
			taskbarRect.bottom = screenRect.bottom + 40; // Approximate taskbar height
		}

		return taskbarRect;
	}

	struct FindTaskbarOnMonitorCtx {
		HMONITOR target;
		RECT     result;
		bool     found;
	};

	static BOOL CALLBACK EnumTaskbarWindowsProc(HWND hwnd, LPARAM lParam) {
		auto* ctx = (FindTaskbarOnMonitorCtx*)lParam;
		wchar_t cls[64] = { 0 };
		GetClassName(hwnd, cls, _countof(cls));
		if (wcscmp(cls, L"Shell_TrayWnd") != 0 && wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0)
			return TRUE; // keep enumerating

		RECT wr; GetWindowRect(hwnd, &wr);
		HMONITOR mon = ::MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
		if (mon == ctx->target) {
			ctx->result = wr;
			ctx->found = true;
			return FALSE; // stop, found the taskbar on the target monitor
		}
		return TRUE;
	}

	// Get the taskbar rect for a SPECIFIC monitor (needed for multi-monitor
	// setups, where each monitor can have its own taskbar window
	// ("Shell_TrayWnd" for the primary monitor, "Shell_SecondaryTrayWnd" for
	// secondary monitors). Falls back to the primary taskbar / work-area-edge
	// approximation if no taskbar window is found on that monitor.
	static RECT GetTaskbarRectForMonitor(HMONITOR hMonitor) {
		FindTaskbarOnMonitorCtx ctx{ hMonitor, {0,0,0,0}, false };
		::EnumWindows(EnumTaskbarWindowsProc, (LPARAM)&ctx);
		if (ctx.found) return ctx.result;

		// Fallback: approximate using the work area / monitor rect of that monitor
		// (assume taskbar sits along the edge where the work area shrinks).
		MONITORINFO mi{ sizeof(mi) };
		::GetMonitorInfo(hMonitor, &mi);
		RECT wa = mi.rcWork, mr = mi.rcMonitor;
		RECT taskbarRect = { mr.left, mr.bottom, mr.right, mr.bottom };
		if (wa.bottom < mr.bottom)      taskbarRect = { mr.left, wa.bottom, mr.right, mr.bottom };
		else if (wa.top > mr.top)       taskbarRect = { mr.left, mr.top, mr.right, wa.top };
		else if (wa.left > mr.left)     taskbarRect = { mr.left, mr.top, wa.left, mr.bottom };
		else if (wa.right < mr.right)   taskbarRect = { wa.right, mr.top, mr.right, mr.bottom };
		return taskbarRect;
	}

	// Determine taskbar edge (where it's positioned)
	// Returns: 1=bottom, 2=top, 3=left, 4=right, 0=not found
	static int GetTaskbarEdge() {
		RECT taskbarRect = GetTaskbarRect();
		if (taskbarRect.left == 0 && taskbarRect.top == 0 && taskbarRect.right == 0 && taskbarRect.bottom == 0) {
			return 0; // Taskbar not found
		}

		HMONITOR hMonitor = GetMonitorFromCursor();
		RECT workArea = GetWorkAreaForMonitor(hMonitor);
		MONITORINFO mi = { sizeof(MONITORINFO) };
		::GetMonitorInfo(hMonitor, &mi);
		RECT monitorRect = mi.rcMonitor;

		// Check which edge the taskbar is on
		// Bottom: taskbar bottom is at monitor bottom, or work area bottom is above monitor bottom
		if (taskbarRect.bottom == monitorRect.bottom || workArea.bottom < monitorRect.bottom) {
			return 1;
		}
		// Top: taskbar top is at monitor top
		if (taskbarRect.top == monitorRect.top || workArea.top > monitorRect.top) {
			return 2;
		}
		// Left: taskbar left is at monitor left
		if (taskbarRect.left == monitorRect.left || workArea.left > monitorRect.left) {
			return 3;
		}
		// Right: taskbar right is at monitor right
		if (taskbarRect.right == monitorRect.right || workArea.right < monitorRect.right) {
			return 4;
		}

		return 1; // Default to bottom
	}

	// Extract URL from a .url file (INI-style [InternetShortcut] URL=...)
	static String GetUrlFromUrlFile(const String& file_path) {
		Char url[2048] = { 0 };
		::GetPrivateProfileString(L"InternetShortcut", L"URL", L"", url, 2048, file_path.c_str());
		return url;
	}

	// Check if a .url file has a custom icon set (IconFile= key)
	static bool HasCustomIconInUrlFile(const String& file_path) {
		Char icon_file[MAX_PATH] = { 0 };
		::GetPrivateProfileString(L"InternetShortcut", L"IconFile", L"", icon_file, MAX_PATH, file_path.c_str());
		return icon_file[0] != 0;
	}

	// Extract URL from a .lnk that targets a web URL (via IShellLink arguments/description or InternetShortcut target)
	// Returns empty string if target is not a web URL.
	static String GetUrlFromLnk(const String& file_path) {
		IShellLink* psl = nullptr;
		if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, (void**)&psl)))
			return L"";

		IPersistFile* ppf = nullptr;
		String url;
		if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
			if (SUCCEEDED(ppf->Load(file_path.c_str(), STGM_READ))) {
				// Try getting the path � for internet shortcuts it will be a URL
				WCHAR path[MAX_PATH] = { 0 };
				psl->GetPath(path, MAX_PATH, nullptr, 0);
				String s = path;
				if (s.size() > 7 && (s.substr(0,7) == L"http://" || s.substr(0,8) == L"https://")) {
					url = s;
				}
				// Some .lnk files store web URL in description
				if (url.empty()) {
					WCHAR desc[2048] = { 0 };
					psl->GetDescription(desc, 2048);
					String d = desc;
					if (d.size() > 7 && (d.substr(0,7) == L"http://" || d.substr(0,8) == L"https://")) {
						url = d;
					}
				}
				// Also try arguments (rare but possible)
				if (url.empty()) {
					WCHAR args[2048] = { 0 };
					psl->GetArguments(args, 2048);
					String a = args;
					if (a.size() > 7 && (a.substr(0,7) == L"http://" || a.substr(0,8) == L"https://")) {
						url = a;
					}
				}
				// If path is a .url file, recurse into it
				if (url.empty() && s.size() > 4 && Util::ends_with(s, L".url")) {
					url = GetUrlFromUrlFile(s);
				}
			}
			ppf->Release();
		}
		psl->Release();
		return url;
	}

	// Check if a .lnk has a custom icon set (GetIconLocation returns non-empty path)
	static bool HasCustomIconInLnk(const String& file_path) {
		IShellLink* psl = nullptr;
		if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, (void**)&psl)))
			return false;
		IPersistFile* ppf = nullptr;
		bool has_icon = false;
		if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
			if (SUCCEEDED(ppf->Load(file_path.c_str(), STGM_READ))) {
				WCHAR icon_path[MAX_PATH] = { 0 };
				int   icon_idx = 0;
				psl->GetIconLocation(icon_path, MAX_PATH, &icon_idx);
				has_icon = icon_path[0] != 0;
			}
			ppf->Release();
		}
		psl->Release();
		return has_icon;
	}

	// Determine if a shortcut item (by file_path) is a web shortcut and return its URL.
	// Returns empty string if not a web shortcut.
	static String GetWebUrl(const String& file_path) {
		if (ends_with(file_path, L".url")) {
			return GetUrlFromUrlFile(file_path);
		}
		if (ends_with(file_path, L".lnk")) {
			return GetUrlFromLnk(file_path);
		}
		return L"";
	}

	// Check if the shortcut already has a custom (user-defined) icon that takes priority.
	static bool HasCustomIcon(const String& file_path) {
		if (ends_with(file_path, L".url"))
			return HasCustomIconInUrlFile(file_path);
		if (ends_with(file_path, L".lnk"))
			return HasCustomIconInLnk(file_path);
		return false;
	}

	// Build the path of the favicon cache file for a given item.
	// item_name is the relative path (e.g. "MySearch.url" or "Sub.submenu\\MySearch.url")
	static String GetFaviconCachePath(const String& base_dir, const String& item_name) {
		// Use just the filename (last component) as the .ico file name
		size_t sep = item_name.rfind(DIR_SEP[0]);
		String leaf = (sep == String::npos) ? item_name : item_name.substr(sep + 1);
		return base_dir + FAVICON_FOLDER + DIR_SEP + leaf + L".ico";
	}

	// Extract host from a URL string (e.g. "https://www.example.com/path" -> "www.example.com")
	static String HostFromUrl(const String& url) {
		size_t start = String::npos;
		if (url.size() > 8 && url.substr(0, 8) == L"https://") start = 8;
		else if (url.size() > 7 && url.substr(0, 7) == L"http://")  start = 7;
		if (start == String::npos) return L"";
		size_t end = url.find(L'/', start);
		return (end == String::npos) ? url.substr(start) : url.substr(start, end - start);
	}

	// Make an absolute URL from an href found in a page and the page's own URL.
	static String MakeAbsoluteUrl(const String& href, const String& page_url) {
		if (href.empty()) return L"";
		// Already absolute
		if (href.size() > 7 && (href.substr(0, 7) == L"http://" || (href.size() > 8 && href.substr(0, 8) == L"https://")))
			return href;
		// Protocol-relative
		if (href.size() >= 2 && href[0] == L'/' && href[1] == L'/') {
			bool https = (page_url.size() > 8 && page_url.substr(0, 8) == L"https://");
			return (https ? L"https:" : L"http:") + href;
		}
		// Find scheme://host in page_url
		size_t scheme_end = page_url.find(L"://");
		if (scheme_end == String::npos) return href;
		size_t host_start = scheme_end + 3;
		size_t path_start = page_url.find(L'/', host_start);
		String origin = (path_start == String::npos) ? page_url : page_url.substr(0, path_start);
		// Root-relative
		if (href[0] == L'/') return origin + href;
		// Relative to current path directory
		String base = (path_start == String::npos) ? page_url + L"/" :
			page_url.substr(0, page_url.rfind(L'/') + 1);
		return base + href;
	}

	// Download bytes from a URL using an existing WinHTTP session. Returns empty on failure.
	static std::vector<Byte> WinHttpFetch(HINTERNET hSession, const String& fetch_url, DWORD max_bytes = 0) {
		// Parse URL components
		URL_COMPONENTS uc = {};
		uc.dwStructSize = sizeof(uc);
		WCHAR scheme[16] = {}, host[256] = {}, path_buf[2048] = {};
		uc.lpszScheme    = scheme;    uc.dwSchemeLength    = 16;
		uc.lpszHostName  = host;      uc.dwHostNameLength  = 256;
		uc.lpszUrlPath   = path_buf;  uc.dwUrlPathLength   = 2048;
		if (!::WinHttpCrackUrl(fetch_url.c_str(), 0, 0, &uc)) return {};

		HINTERNET hConn = ::WinHttpConnect(hSession, host, uc.nPort, 0);
		if (!hConn) return {};

		bool is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
		DWORD req_flags = is_https ? WINHTTP_FLAG_SECURE : 0;
		// path_buf already contains path+query from WinHttpCrackUrl
		HINTERNET hReq = ::WinHttpOpenRequest(hConn, L"GET", path_buf,
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
		if (!hReq) { ::WinHttpCloseHandle(hConn); return {}; }

		// Ignore certificate errors (favicon is non-critical)
		DWORD sec_opt = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
						SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
		::WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sec_opt, sizeof(sec_opt));

		// Follow redirects automatically
		DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
		::WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

		std::vector<Byte> data;
		if (::WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			::WinHttpReceiveResponse(hReq, nullptr)) {
			DWORD status = 0, sz = sizeof(status);
			::WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
			if (status == 200) {
				DWORD avail = 0;
				while (::WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
					if (max_bytes && data.size() + avail > max_bytes) avail = (DWORD)(max_bytes - data.size());
					size_t off = data.size();
					data.resize(off + avail);
					DWORD read = 0;
					::WinHttpReadData(hReq, data.data() + off, avail, &read);
					data.resize(off + read);
					if (max_bytes && data.size() >= max_bytes) break;
				}
			}
		}
		::WinHttpCloseHandle(hReq);
		::WinHttpCloseHandle(hConn);
		return data;
	}

	// Parse HTML bytes and return the first favicon URL found in <link rel="icon"> tags.
	// Returns empty string if not found.
	static String ExtractFaviconFromHtml(const std::vector<Byte>& html, const String& page_url) {
		// Work in narrow strings for simple ASCII-safe tag parsing
		// (favicon URLs in <link> are always ASCII-safe)
		std::string s(html.begin(), html.end());
		// Lower-case copy for attribute searching
		std::string lo = s;
		for (auto& c : lo) c = (char)::tolower((unsigned char)c);

		size_t pos = 0;
		while (pos < lo.size()) {
			size_t tag_start = lo.find("<link", pos);
			if (tag_start == std::string::npos) break;
			size_t tag_end = lo.find('>', tag_start);
			if (tag_end == std::string::npos) break;
			std::string tag_lo = lo.substr(tag_start, tag_end - tag_start + 1);
			std::string tag_orig = s.substr(tag_start, tag_end - tag_start + 1);

			// Must contain rel="...icon..."
			size_t rel_p = tag_lo.find("rel=");
			if (rel_p != std::string::npos) {
				size_t rv = rel_p + 4;
				char delim = (tag_lo[rv] == '"' || tag_lo[rv] == '\'') ? tag_lo[rv++] : ' ';
				size_t re = tag_lo.find(delim, rv);
				if (re == std::string::npos) re = tag_lo.find('>', rv);
				std::string rel_val = tag_lo.substr(rv, re - rv);
				if (rel_val.find("icon") != std::string::npos) {
					// Extract href from original (preserve case)
					size_t href_p = tag_lo.find("href=");
					if (href_p != std::string::npos) {
						size_t hv = href_p + 5;
						char hd = (tag_lo[hv] == '"' || tag_lo[hv] == '\'') ? tag_lo[hv++] : ' ';
						size_t he = tag_lo.find(hd, hv);
						if (he == std::string::npos) he = tag_lo.find('>', hv);
						std::string href_n = tag_orig.substr(hv, he - hv);
						// Trim whitespace
						while (!href_n.empty() && (href_n.front() == ' ' || href_n.front() == '\t')) href_n.erase(href_n.begin());
						while (!href_n.empty() && (href_n.back()  == ' ' || href_n.back()  == '\t')) href_n.pop_back();
						if (!href_n.empty()) {
							String href(href_n.begin(), href_n.end());
							return MakeAbsoluteUrl(href, page_url);
						}
					}
				}
			}
			pos = tag_end + 1;
		}
		return L"";
	}

	// Download favicon.ico from host and save to dest_path (creates dirs as needed).
	// Strategy: (1) try /favicon.ico, (2) fetch page HTML and parse <link rel="icon">.
	// Returns true on success.
	static bool DownloadAndSaveFavicon(const String& page_url, const String& dest_path, const String& base_dir) {
		if (HostFromUrl(page_url).empty()) return false;

		// Ensure favicon-icon-web directory exists and is hidden
		String favicon_dir = base_dir + FAVICON_FOLDER;
		if (!::CreateDirectory(favicon_dir.c_str(), nullptr)) {
			if (::GetLastError() != ERROR_ALREADY_EXISTS) return false;
		}
		::SetFileAttributes(favicon_dir.c_str(), FILE_ATTRIBUTE_HIDDEN);

		HINTERNET hSession = ::WinHttpOpen(L"Stacky/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!hSession) return false;

		// Set a reasonable timeout (10 s)
		DWORD timeout = 10000;
		::WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT,      &timeout, sizeof(timeout));
		::WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT,      &timeout, sizeof(timeout));
		::WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT,         &timeout, sizeof(timeout));
		::WinHttpSetOption(hSession, WINHTTP_OPTION_RESOLVE_TIMEOUT,      &timeout, sizeof(timeout));

		auto SaveData = [&](const std::vector<Byte>& data) -> bool {
			if (data.empty()) return false;
			HANDLE hf = ::CreateFile(dest_path.c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hf == INVALID_HANDLE_VALUE) return false;
			DWORD written = 0;
			::WriteFile(hf, data.data(), (DWORD)data.size(), &written, nullptr);
			::CloseHandle(hf);
			return written == (DWORD)data.size() && written > 0;
		};

		bool ok = false;

		// --- Strategy 1: try /favicon.ico on the same host ---
		String host = HostFromUrl(page_url);
		bool is_https = (page_url.size() > 8 && page_url.substr(0, 8) == L"https://");
		String ico_url = (is_https ? L"https://" : L"http://") + host + L"/favicon.ico";
		{
			auto data = WinHttpFetch(hSession, ico_url);
			// Validate: must be at least 4 bytes and look like ICO (0000 0100) or PNG signature
			bool valid = data.size() >= 4 &&
				((data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0) ||  // ICO
				 (data[0] == 0x89 && data[1] == 0x50));                              // PNG
			if (valid) ok = SaveData(data);
		}

		// --- Strategy 2: fetch page HTML and parse <link rel="icon"> ---
		if (!ok) {
			// Only download first 64 KB of HTML (enough to find <head> tags)
			auto html = WinHttpFetch(hSession, page_url, 65536);
			if (!html.empty()) {
				String favicon_url = ExtractFaviconFromHtml(html, page_url);
				if (!favicon_url.empty()) {
					auto data = WinHttpFetch(hSession, favicon_url);
					if (!data.empty()) ok = SaveData(data);
				}
			}
		}

		::WinHttpCloseHandle(hSession);
		return ok;
	}

	// Apply a downloaded favicon .ico as the visible icon of the underlying
	// shortcut file (.url or .lnk) so File Explorer also shows it, not just
	// the Stacky menu/submenu. For .url files this sets IconFile/IconIndex
	// in the [InternetShortcut] section; for .lnk files it uses IShellLink's
	// SetIconLocation and re-saves the link via IPersistFile.
	static void ApplyIconToShortcutFile(const String& file_path, const String& icon_path) {
		if (ends_with(file_path, L".url")) {
			::WritePrivateProfileString(L"InternetShortcut", L"IconFile", icon_path.c_str(), file_path.c_str());
			::WritePrivateProfileString(L"InternetShortcut", L"IconIndex", L"0", file_path.c_str());
		} else if (ends_with(file_path, L".lnk")) {
			IShellLink* psl = nullptr;
			if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, (void**)&psl))) {
				IPersistFile* ppf = nullptr;
				if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
					if (SUCCEEDED(ppf->Load(file_path.c_str(), STGM_READWRITE))) {
						psl->SetIconLocation(icon_path.c_str(), 0);
						ppf->Save(file_path.c_str(), TRUE);
					}
					ppf->Release();
				}
				psl->Release();
			}
		}
		// Notify Explorer so it refreshes the icon for this specific file
		::SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATH, file_path.c_str(), nullptr);
	}

	struct FaviconThreadParams {
		String url;
		String dest_path;
		String base_dir;
		String cache_path;    // !stacky.cache path to delete so it rebuilds next time
		String target_path;   // full path of the .url/.lnk shortcut this favicon belongs to
	};

	static DWORD WINAPI FaviconThreadProc(LPVOID param) {
		FaviconThreadParams* p = (FaviconThreadParams*)param;
		// Sleep briefly so the launched page starts loading
		::Sleep(2000);
		bool saved = DownloadAndSaveFavicon(p->url, p->dest_path, p->base_dir);
		if (saved) {
			// Also apply the favicon to the shortcut file itself so it shows
			// up in File Explorer, not just in the Stacky menu/submenu.
			if (!p->target_path.empty()) {
				ApplyIconToShortcutFile(p->target_path, p->dest_path);
			}
			// Delete cache so next launch rebuilds with favicon icon
			::DeleteFile(p->cache_path.c_str());
		}
		delete p;
		return 0;
	}

	// Launch favicon download in background thread.
	static void TriggerFaviconDownloadAsync(const String& base_dir, const String& item_name,
											 const String& url, const String& cache_file_path,
											 const String& target_path = L"") {
		// Don't re-download if favicon already cached
		String dest = GetFaviconCachePath(base_dir, item_name);
		if (::GetFileAttributes(dest.c_str()) != INVALID_FILE_ATTRIBUTES)
			return;  // already have it

		FaviconThreadParams* p = new FaviconThreadParams();
		p->url         = url;
		p->dest_path   = dest;
		p->base_dir    = base_dir;
		p->cache_path  = cache_file_path;
		p->target_path = target_path;
		HANDLE hThread = ::CreateThread(nullptr, 0, FaviconThreadProc, p, 0, nullptr);
		if (hThread) ::CloseHandle(hThread);
		else delete p;
	}

	// ------------------------------------------------------------------------------------------
	// Recent files MRU: tracks, per resolved application, the last documents launched through
	// Stacky-plus itself (independent of Windows' own Recent-items privacy setting). All access
	// goes through an in-memory map guarded by a mutex; disk persistence happens on a background
	// thread so neither launching a shortcut nor opening the context menu ever blocks on I/O.
	// ------------------------------------------------------------------------------------------
	static std::mutex& RecentFilesMutex() {
		static std::mutex m;
		return m;
	}
	static std::unordered_map<String, std::deque<String>>& RecentFilesMap() {
		static std::unordered_map<String, std::deque<String>> m;
		return m;
	}
	static bool& RecentFilesLoaded() {
		static bool loaded = false;
		return loaded;
	}
	static String RecentFilesStorePath() {
		return GetExeFolder() + L"\\!stacky.recent";
	}

	// Must be called with RecentFilesMutex() held.
	static void LoadRecentFilesLocked() {
		FILE* f = _wfopen(RecentFilesStorePath().c_str(), L"rb");
		if (!f) return;
		fseek(f, 0, SEEK_END);
		long file_size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (file_size <= 0) { fclose(f); return; }
		std::vector<Byte> data(file_size);
		fread(data.data(), 1, file_size, f);
		fclose(f);

		size_t pos = 0;
		while (pos + sizeof(Char) <= data.size()) {
			String key = (const Char*)(data.data() + pos);
			pos += (key.size() + 1) * sizeof(Char);
			if (pos + sizeof(Byte) > data.size()) break;
			Byte count = data[pos];
			pos += sizeof(Byte);
			std::deque<String> files;
			for (Byte i = 0; i < count && pos + sizeof(Char) <= data.size(); ++i) {
				String file_path = (const Char*)(data.data() + pos);
				pos += (file_path.size() + 1) * sizeof(Char);
				files.push_back(file_path);
			}
			RecentFilesMap()[key] = files;
		}
	}
	static void EnsureRecentFilesLoaded() {
		std::lock_guard<std::mutex> lock(RecentFilesMutex());
		if (RecentFilesLoaded()) return;
		RecentFilesLoaded() = true;
		LoadRecentFilesLocked();
	}

	static void AppendStringToByteBuffer(std::vector<Byte>& buf, const String& s) {
		const Byte* bytes = (const Byte*)s.c_str();
		size_t byte_len = (s.size() + 1) * sizeof(Char);
		buf.insert(buf.end(), bytes, bytes + byte_len);
	}

	struct RecentFilesSaveParams {
		std::vector<Byte> data;
		String path;
	};
	static DWORD WINAPI RecentFilesSaveThreadProc(LPVOID param) {
		RecentFilesSaveParams* p = (RecentFilesSaveParams*)param;
		FILE* f = _wfopen(p->path.c_str(), L"wb");
		if (f) {
			if (!p->data.empty()) fwrite(p->data.data(), 1, p->data.size(), f);
			fclose(f);
		}
		delete p;
		return 0;
	}
	static void SaveRecentFilesAsync() {
		RecentFilesSaveParams* p = new RecentFilesSaveParams();
		p->path = RecentFilesStorePath();
		{
			std::lock_guard<std::mutex> lock(RecentFilesMutex());
			for (auto& kv : RecentFilesMap()) {
				AppendStringToByteBuffer(p->data, kv.first);
				Byte count = (Byte)(kv.second.size() > 255 ? 255 : kv.second.size());
				p->data.push_back(count);
				Byte i = 0;
				for (auto& file_path : kv.second) {
					if (i++ >= count) break;
					AppendStringToByteBuffer(p->data, file_path);
				}
			}
		}
		HANDLE hThread = ::CreateThread(nullptr, 0, RecentFilesSaveThreadProc, p, 0, nullptr);
		if (hThread) ::CloseHandle(hThread);
		else delete p;
	}

	// Records that "target_path" (a document, not the app itself) was just launched, filed
	// under the application that Windows would use to open it by default. cmd is either a
	// .lnk shortcut or a plain file path, exactly as passed to ShellExecute.
	static void RegisterRecentLaunch(const String& cmd) {
		TCHAR resolved[MAX_PATH] = { 0 };
		HRESULT hr = ResolveShortcut(NULL, cmd.c_str(), resolved, _countof(resolved));
		String target = (SUCCEEDED(hr) && resolved[0]) ? String(resolved) : cmd;

		size_t dot = target.find_last_of(L'.');
		if (dot == String::npos) return;
		String ext = target.substr(dot);
		for (auto& c : ext) c = (Char)towlower(c);
		if (ext == L".exe") return; // launching the app itself, not a document: nothing to record

		WCHAR assoc_path[MAX_PATH] = { 0 };
		DWORD assoc_len = _countof(assoc_path);
		if (FAILED(::AssocQueryString(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE,
				ext.c_str(), nullptr, assoc_path, &assoc_len)))
			return;

		String app_key = assoc_path;
		for (auto& c : app_key) c = (Char)towlower(c);

		EnsureRecentFilesLoaded();
		{
			std::lock_guard<std::mutex> lock(RecentFilesMutex());
			auto& dq = RecentFilesMap()[app_key];
			for (auto it = dq.begin(); it != dq.end(); ++it) {
				if (_wcsicmp(it->c_str(), target.c_str()) == 0) { dq.erase(it); break; }
			}
			dq.push_front(target);
			while (dq.size() > 5) dq.pop_back();
		}
		SaveRecentFilesAsync();
	}

	// Return up to max_count most recently used documents launched through Stacky-plus whose
	// default handler application matches app_exe_path. Reads only the in-memory cache (loaded
	// once, lazily) so it never touches disk when the context menu is opened.
	static std::vector<String> GetRecentFilesForApp(const String& app_exe_path, size_t max_count = 5) {
		std::vector<String> result;
		if (app_exe_path.empty()) return result;

		String key = app_exe_path;
		for (auto& c : key) c = (Char)towlower(c);

		EnsureRecentFilesLoaded();
		std::lock_guard<std::mutex> lock(RecentFilesMutex());
		auto it = RecentFilesMap().find(key);
		if (it == RecentFilesMap().end()) return result;
		for (auto& f : it->second) {
			result.push_back(f);
			if (result.size() >= max_count) break;
		}
		return result;
	}

	// Return the folder that contains the currently running executable (no trailing slash).
	static String GetExeFolder() {
		Char path[MAX_PATH] = { 0 };
		::GetModuleFileName(nullptr, path, MAX_PATH);
		String p = path;
		size_t pos = p.find_last_of(L"\\/");
		return (pos == String::npos) ? p : p.substr(0, pos);
	}

	// True if the current user's UI language is Spanish (any variant: es-ES, es-MX, etc.).
	static bool IsSpanishUILanguage() {
		WCHAR name[LOCALE_NAME_MAX_LENGTH] = { 0 };
		if (::GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
			return _wcsnicmp(name, L"es", 2) == 0;
		}
		LANGID lang = ::GetUserDefaultUILanguage();
		return PRIMARYLANGID(lang) == LANG_SPANISH;
	}

	// Return the two-letter ISO 639-1 language code of the current user's UI language
	// (e.g. L"en", L"es", L"fr"...), lowercase. Falls back to L"en" on failure.
	static String GetUILanguageCode() {
		WCHAR name[LOCALE_NAME_MAX_LENGTH] = { 0 };
		if (::GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0 && name[0] && name[1]) {
			WCHAR code[3] = { (WCHAR)towlower(name[0]), (WCHAR)towlower(name[1]), 0 };
			return String(code);
		}
		return L"en";
	}

	// Undocumented uxtheme.dll ordinals used to switch classic (non-owner-drawn)
	// popup menus to dark mode, matching the rest of the app's dark-mode styling.
	// This is the same mechanism used by Explorer and other apps for dark context menus.
	static void EnableDarkContextMenu(bool enable) {
		static HMODULE hUxtheme = ::LoadLibraryW(L"uxtheme.dll");
		if (!hUxtheme) return;

		typedef void (WINAPI* SetPreferredAppModeFn)(int mode);
		typedef void (WINAPI* FlushMenuThemesFn)();

		static auto setPreferredAppMode = (SetPreferredAppModeFn)::GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));
		static auto flushMenuThemes     = (FlushMenuThemesFn)::GetProcAddress(hUxtheme, MAKEINTRESOURCEA(136));

		if (setPreferredAppMode) setPreferredAppMode(enable ? 2 /*ForceDark*/ : 0 /*Default*/);
		if (flushMenuThemes) flushMenuThemes();
	}

	// Extensible translation table for the submenu right-click context menu.
	// Add new languages here as additional rows (2-letter ISO 639-1 code + 3 strings).
	// English is the default/fallback for any language not listed.
	struct SubmenuCtxMenuStrings {
		const wchar_t* lang;
		const wchar_t* open_subfolder;
		const wchar_t* open_menu_folder;
		const wchar_t* open_stacky_folder;
		const wchar_t* close_menu;
		const wchar_t* open_shortcut_location;
		const wchar_t* run_as_admin;
		const wchar_t* settings;
	};

	static const SubmenuCtxMenuStrings& GetSubmenuCtxMenuStrings() {
		static const SubmenuCtxMenuStrings table[] = {
			{ L"en", L"Open subfolder",           L"Open main menu folder",              L"Open Stacky-plus.exe folder",        L"Close this context menu", L"Open shortcut location", L"Run as administrator", L"Settings" },
			{ L"es", L"Abrir subcarpeta",         L"Abrir carpeta del men\u00FA principal", L"Abrir carpeta de Stacky-plus.exe", L"Cerrar este men\u00FA contextual", L"Abrir ubicaci\u00F3n del acceso directo", L"Ejecutar como administrador", L"Configuraci\u00F3n" },
			{ L"pt", L"Abrir subpasta",           L"Abrir pasta do menu principal",      L"Abrir pasta do Stacky-plus.exe",     L"Fechar este menu de contexto", L"Abrir local do atalho", L"Executar como administrador", L"Configura\u00E7\u00E3o" },
			{ L"fr", L"Ouvrir le sous-dossier",   L"Ouvrir le dossier du menu principal",L"Ouvrir le dossier de Stacky-plus.exe", L"Fermer ce menu contextuel", L"Ouvrir l'emplacement du raccourci", L"Ex\u00E9cuter en tant qu'administrateur", L"Param\u00E8tres" },
			{ L"de", L"Unterordner \u00F6ffnen",  L"Hauptmen\u00FC-Ordner \u00F6ffnen",  L"Stacky-plus.exe-Ordner \u00F6ffnen", L"Dieses Kontextmen\u00FC schlie\u00DFen", L"Verkn\u00FCpfungsspeicherort \u00F6ffnen", L"Als Administrator ausf\u00FChren", L"Einstellungen" },
			{ L"it", L"Apri sottocartella",       L"Apri cartella del menu principale",  L"Apri cartella di Stacky-plus.exe",   L"Chiudi questo menu contestuale", L"Apri percorso collegamento", L"Esegui come amministratore", L"Impostazioni" },
			{ L"pl", L"Otw\u00F3rz podfolder",    L"Otw\u00F3rz folder menu g\u0142\u00F3wnego", L"Otw\u00F3rz folder Stacky-plus.exe", L"Zamknij to menu kontekstowe", L"Otw\u00F3rz lokalizacj\u0119 skr\u00F3tu", L"Uruchom jako administrator", L"Ustawienia" },
			{ L"ru", L"\u041E\u0442\u043A\u0440\u044B\u0442\u044C \u043F\u043E\u0434\u043F\u0430\u043F\u043A\u0443", L"\u041E\u0442\u043A\u0440\u044B\u0442\u044C \u043F\u0430\u043F\u043A\u0443 \u0433\u043B\u0430\u0432\u043D\u043E\u0433\u043E \u043C\u0435\u043D\u044E", L"\u041E\u0442\u043A\u0440\u044B\u0442\u044C \u043F\u0430\u043F\u043A\u0443 Stacky-plus.exe", L"\u0417\u0430\u043A\u0440\u044B\u0442\u044C \u044D\u0442\u043E \u043A\u043E\u043D\u0442\u0435\u043A\u0441\u0442\u043D\u043E\u0435 \u043C\u0435\u043D\u044E", L"\u041E\u0442\u043A\u0440\u044B\u0442\u044C \u043C\u0435\u0441\u0442\u043E\u043F\u043E\u043B\u043E\u0436\u0435\u043D\u0438\u0435 \u044F\u0440\u043B\u044B\u043A\u0430", L"\u0417\u0430\u043F\u0443\u0441\u0442\u0438\u0442\u044C \u043E\u0442 \u0438\u043C\u0435\u043D\u0438 \u0430\u0434\u043C\u0438\u043D\u0438\u0441\u0442\u0440\u0430\u0442\u043E\u0440\u0430", L"\u041F\u0430\u0440\u0430\u043C\u0435\u0442\u0440\u044B" },
			{ L"zh", L"\u6253\u5F00\u5B50\u6587\u4EF6\u5939", L"\u6253\u5F00\u4E3B\u83DC\u5355\u6587\u4EF6\u5939", L"\u6253\u5F00 Stacky-plus.exe \u6587\u4EF6\u5939", L"\u5173\u95ED\u6B64\u5FEB\u6377\u83DC\u5355", L"\u6253\u5F00\u5FEB\u6377\u65B9\u5F0F\u4F4D\u7F6E", L"\u4EE5\u7BA1\u7406\u5458\u8EAB\u4EFD\u8FD0\u884C", L"\u8BBE\u7F6E" },
			{ L"ja", L"\u30B5\u30D6\u30D5\u30A9\u30EB\u30C0\u30FC\u3092\u958B\u304F", L"\u30E1\u30A4\u30F3\u30E1\u30CB\u30E5\u30FC\u30D5\u30A9\u30EB\u30C0\u30FC\u3092\u958B\u304F", L"Stacky-plus.exe\u306E\u30D5\u30A9\u30EB\u30C0\u30FC\u3092\u958B\u304F", L"\u3053\u306E\u30B3\u30F3\u30C6\u30AD\u30B9\u30C8\u30E1\u30CB\u30E5\u30FC\u3092\u9589\u3058\u308B", L"\u30B7\u30E7\u30FC\u30C8\u30AB\u30C3\u30C8\u306E\u4FDD\u5B58\u5834\u6240\u3092\u958B\u304F", L"\u7BA1\u7406\u8005\u3068\u3057\u3066\u5B9F\u884C", L"\u8A2D\u5B9A" },
		};
		String code = GetUILanguageCode();
		for (const auto& row : table) {
			if (code == row.lang) return row;
		}
		return table[0]; // English fallback
	}

	// Convert an HICON into a top-down 32bpp premultiplied-alpha DIB section of the
	// given size (square). Caller owns the returned HBITMAP (DeleteObject when done).
	static HBITMAP CreateArgbBitmapFromIcon(HICON hIcon, int size) {
		if (!hIcon) return nullptr;
		static IWICImagingFactory* img_factory = nullptr;
		if (!img_factory) {
			if (!SUCCEEDED(::CoInitialize(0)) || !SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory1, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&img_factory)))) {
				return nullptr;
			}
		}

		HBITMAP result = nullptr;
		IWICBitmap* pBitmap = nullptr;
		if (SUCCEEDED(img_factory->CreateBitmapFromHICON(hIcon, &pBitmap))) {
			IWICBitmapScaler* pScaler = nullptr;
			if (SUCCEEDED(img_factory->CreateBitmapScaler(&pScaler)) &&
				SUCCEEDED(pScaler->Initialize(pBitmap, size, size, WICBitmapInterpolationModeFant))) {
				IWICFormatConverter* pConverter = nullptr;
				if (SUCCEEDED(img_factory->CreateFormatConverter(&pConverter))) {
					if (SUCCEEDED(pConverter->Initialize(pScaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom))) {
						BITMAPINFO bmi = { 0 };
						bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
						bmi.bmiHeader.biWidth       = size;
						bmi.bmiHeader.biHeight      = -size; // top-down
						bmi.bmiHeader.biPlanes      = 1;
						bmi.bmiHeader.biBitCount    = 32;
						bmi.bmiHeader.biCompression = BI_RGB;
						void* bits = nullptr;
						result = ::CreateDIBSection(GetDC(0), &bmi, DIB_RGB_COLORS, &bits, 0, 0);
						if (result && bits) {
							UINT stride = size * 4;
							if (FAILED(pConverter->CopyPixels(nullptr, stride, stride * size, (BYTE*)bits))) {
								::DeleteObject(result);
								result = nullptr;
							}
						}
					}
					pConverter->Release();
				}
				pScaler->Release();
			}
			pBitmap->Release();
		}
		return result;
	}

	// Yellow folder bitmap (used for "open menu folder" context-menu entry).
	static HBITMAP CreateStockFolderBitmap(int size) {
		SHSTOCKICONINFO sii = { sizeof(sii) };
		if (FAILED(::SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON, &sii))) return nullptr;
		HBITMAP bmp = CreateArgbBitmapFromIcon(sii.hIcon, size);
		::DestroyIcon(sii.hIcon);
		return bmp;
	}

	// Open-folder bitmap (used for "open shortcut location" context-menu entry).
	static HBITMAP CreateOpenFolderBitmap(int size) {
		SHSTOCKICONINFO sii = { sizeof(sii) };
		if (FAILED(::SHGetStockIconInfo(SIID_FOLDEROPEN, SHGSI_ICON, &sii))) {
			// Fall back to the closed folder icon if the open-folder stock icon is unavailable.
			if (FAILED(::SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON, &sii))) return nullptr;
		}
		HBITMAP bmp = CreateArgbBitmapFromIcon(sii.hIcon, size);
		::DestroyIcon(sii.hIcon);
		return bmp;
	}

	// Two overlapping yellow folder glyphs suggesting a folder+subfolder tree
	// (used for "open subfolder" context-menu entry).
	static HBITMAP CreateFolderTreeBitmap(int size) {
		SHSTOCKICONINFO sii = { sizeof(sii) };
		if (FAILED(::SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON, &sii))) return nullptr;

		int smallSz = (size * 11) / 16;
		if (smallSz < 4) smallSz = 4;
		HBITMAP smallBmp = CreateArgbBitmapFromIcon(sii.hIcon, smallSz);
		::DestroyIcon(sii.hIcon);
		if (!smallBmp) return nullptr;

		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth       = size;
		bmi.bmiHeader.biHeight      = -size;
		bmi.bmiHeader.biPlanes      = 1;
		bmi.bmiHeader.biBitCount    = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		void* destBits = nullptr;
		HBITMAP dest = ::CreateDIBSection(GetDC(0), &bmi, DIB_RGB_COLORS, &destBits, 0, 0);
		if (dest && destBits) {
			memset(destBits, 0, (size_t)size * size * 4);

			HDC memDC = CreateCompatibleDC(nullptr);
			HDC srcDC = CreateCompatibleDC(nullptr);
			HGDIOBJ oldMem = SelectObject(memDC, dest);
			HGDIOBJ oldSrc = SelectObject(srcDC, smallBmp);

			BLENDFUNCTION bf{}; bf.BlendOp = AC_SRC_OVER; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
			// back folder (upper-left)
			AlphaBlend(memDC, 0, 0, smallSz, smallSz, srcDC, 0, 0, smallSz, smallSz, bf);
			// front folder (lower-right), overlapping to suggest hierarchy
			int off = size - smallSz;
			AlphaBlend(memDC, off, off, smallSz, smallSz, srcDC, 0, 0, smallSz, smallSz, bf);

			SelectObject(memDC, oldMem);
			SelectObject(srcDC, oldSrc);
			DeleteDC(memDC);
			DeleteDC(srcDC);
		}
		DeleteObject(smallBmp);
		return dest;
	}

	// Stacky-plus.exe's own icon (used for "open Stacky-plus folder" context-menu entry).
	static HBITMAP CreateStackyExeBitmap(int size) {
		HICON hIcon = (HICON)::LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(STACKY_ICON_ID), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
		if (!hIcon) return nullptr;
		HBITMAP bmp = CreateArgbBitmapFromIcon(hIcon, size);
		::DestroyIcon(hIcon);
		return bmp;
	}

	// Red "X" cross bitmap (used for "close this context menu" entry).
	static HBITMAP CreateRedCrossBitmap(int size) {
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth       = size;
		bmi.bmiHeader.biHeight      = -size; // top-down
		bmi.bmiHeader.biPlanes      = 1;
		bmi.bmiHeader.biBitCount    = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		BYTE* bits = nullptr;
		HBITMAP dest = ::CreateDIBSection(GetDC(0), &bmi, DIB_RGB_COLORS, (void**)&bits, 0, 0);
		if (!dest || !bits) return dest;
		memset(bits, 0, (size_t)size * size * 4);

		HDC memDC = CreateCompatibleDC(nullptr);
		HGDIOBJ oldBmp = SelectObject(memDC, dest);

		int penW = max(1, size / 8);
		HPEN pen = CreatePen(PS_SOLID, penW, RGB(200, 0, 0));
		HGDIOBJ oldPen = SelectObject(memDC, pen);

		int m = (int)(size * 0.22);
		MoveToEx(memDC, m, m, nullptr);
		LineTo(memDC, size - m, size - m);
		MoveToEx(memDC, size - m, m, nullptr);
		LineTo(memDC, m, size - m);

		SelectObject(memDC, oldPen);
		DeleteObject(pen);
		SelectObject(memDC, oldBmp);
		DeleteDC(memDC);

		// GDI line drawing does not set the alpha channel; derive alpha from
		// how far each pixel's color is from transparent black so the cross
		// composites correctly via AlphaBlend/premultiplied-alpha menu bitmaps.
		size_t pixelCount = (size_t)size * size;
		for (size_t i = 0; i < pixelCount; ++i) {
			BYTE* px = bits + i * 4; // B, G, R, A
			BYTE maxc = max(px[0], max(px[1], px[2]));
			BYTE alpha = maxc;
			if (alpha == 0) continue;
			px[0] = (BYTE)((int)px[0] * alpha / 255);
			px[1] = (BYTE)((int)px[1] * alpha / 255);
			px[2] = (BYTE)((int)px[2] * alpha / 255);
			px[3] = alpha;
		}

		return dest;
	}

	// Build a 16x16-style shield icon (scaled to `size`) using the same UAC shield
	// stock icon Windows shows for "Run as administrator" elevation prompts.
	static HBITMAP CreateShieldBitmap(int size) {
		SHSTOCKICONINFO sii = { sizeof(sii) };
		if (FAILED(::SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON, &sii))) return nullptr;
		HBITMAP bmp = CreateArgbBitmapFromIcon(sii.hIcon, size);
		::DestroyIcon(sii.hIcon);
		return bmp;
	}

	// Gear/settings icon, used for the "Settings" / "Configuraci\u00F3n" context-menu
	// entry that opens the Configuration window. Uses SHGetStockIconInfo with a
	// stable SIID_* stock-icon id instead of a fixed shell32.dll icon index:
	// numeric indexes inside shell32.dll are not guaranteed to be stable across
	// Windows versions/builds, so the same index can resolve to a different
	// icon on another PC. SIID_SETTINGS (Windows 10 1809+) is preferred; older
	// systems fall back to SIID_SOFTWARE, which SHGetStockIconInfo supports
	// since Windows Vista.
	static HBITMAP CreateSettingsGearBitmap_UnusedStock(int size) {
		SHSTOCKICONINFO sii = { sizeof(sii) };
		if (FAILED(::SHGetStockIconInfo(SIID_SETTINGS, SHGSI_ICON, &sii))) {
			sii = SHSTOCKICONINFO{ sizeof(sii) };
			if (FAILED(::SHGetStockIconInfo(SIID_SOFTWARE, SHGSI_ICON, &sii))) return nullptr;
		}
		HBITMAP bmp = CreateArgbBitmapFromIcon(sii.hIcon, size);
		::DestroyIcon(sii.hIcon);
		return bmp;
	}

	// Gear/settings icon, used for the "Settings" / "Configuración" context-menu
	// entry that opens the Configuration window. Drawn procedurally with GDI
	// (same approach as CreateRedCrossBitmap) instead of relying on a shell
	// stock icon: fixed shell32.dll icon indexes are not stable across Windows
	// versions/builds (a given index can resolve to a different icon on
	// another PC), and SHGetStockIconInfo(SIID_SETTINGS, ...) is not reliably
	// available/renderable on every Windows build either, which previously
	// left the menu entry without a visible icon. Drawing the gear ourselves
	// guarantees it always renders identically everywhere.
	static HBITMAP CreateSettingsGearBitmap(int size) {
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth       = size;
		bmi.bmiHeader.biHeight      = -size; // top-down
		bmi.bmiHeader.biPlanes      = 1;
		bmi.bmiHeader.biBitCount    = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		BYTE* bits = nullptr;
		HBITMAP dest = ::CreateDIBSection(GetDC(0), &bmi, DIB_RGB_COLORS, (void**)&bits, 0, 0);
		if (!dest || !bits) return dest;
		memset(bits, 0, (size_t)size * size * 4);

		HDC memDC = CreateCompatibleDC(nullptr);
		HGDIOBJ oldBmp = SelectObject(memDC, dest);

		HBRUSH brush = CreateSolidBrush(RGB(90, 90, 90));
		HGDIOBJ oldBrush = SelectObject(memDC, brush);
		HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
		HGDIOBJ oldPen = SelectObject(memDC, pen);

		POINT center{ size / 2, size / 2 };
		double outerR = size * 0.46;
		double toothR = size * 0.40; // radius where teeth tips reach
		const int toothCount = 8;

		// Build a gear-shaped polygon by alternating between the body radius
		// and the tooth-tip radius around the circle.
		const int pointsPerTooth = 4;
		const int totalPoints = toothCount * pointsPerTooth;
		POINT poly[totalPoints];
		for (int i = 0; i < totalPoints; ++i) {
			double angle = (2.0 * 3.14159265358979 * i) / totalPoints;
			int phase = i % pointsPerTooth;
			double r = (phase == 1 || phase == 2) ? toothR : outerR;
			poly[i].x = center.x + (LONG)(r * cos(angle));
			poly[i].y = center.y + (LONG)(r * sin(angle));
		}
		Polygon(memDC, poly, totalPoints);

		// Punch the center hole out so it reads as a gear rather than a solid disc.
		// Fill it with pure black (RGB 0,0,0): the alpha-derivation pass below
		// treats fully black pixels as fully transparent, so this actually
		// "erases" the gear body instead of just drawing an outline over it.
		HBRUSH holeBrush = CreateSolidBrush(RGB(0, 0, 0));
		HGDIOBJ oldHoleBrush = SelectObject(memDC, holeBrush);
		HPEN holePen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
		HGDIOBJ oldHolePen = SelectObject(memDC, holePen);
		int hr = (int)(size * 0.16);
		Ellipse(memDC, center.x - hr, center.y - hr, center.x + hr, center.y + hr);
		SelectObject(memDC, oldHolePen);
		SelectObject(memDC, oldHoleBrush);
		DeleteObject(holePen);
		DeleteObject(holeBrush);

		SelectObject(memDC, oldPen);
		SelectObject(memDC, oldBrush);
		DeleteObject(pen);
		DeleteObject(brush);
		SelectObject(memDC, oldBmp);
		DeleteDC(memDC);

		// GDI shape drawing does not set the alpha channel; derive alpha from
		// how far each pixel's color is from transparent black so the gear
		// composites correctly via AlphaBlend/premultiplied-alpha menu bitmaps.
		size_t pixelCount = (size_t)size * size;
		for (size_t i = 0; i < pixelCount; ++i) {
			BYTE* px = bits + i * 4; // B, G, R, A
			BYTE maxc = max(px[0], max(px[1], px[2]));
			BYTE alpha = maxc;
			if (alpha == 0) continue;
			px[0] = (BYTE)((int)px[0] * alpha / 255);
			px[1] = (BYTE)((int)px[1] * alpha / 255);
			px[2] = (BYTE)((int)px[2] * alpha / 255);
			px[3] = alpha;
		}

		return dest;
	}
};

struct Buffer {
	size_t  capacity, size;
	Byte* data;

	Buffer() : capacity(0), size(0), data(0) {}
	~Buffer() {}

	void free() {
		if (data) {
			delete[] data;
			data = 0;
		}
		capacity = size = 0;
	}
	bool load(const String& str, bool append_null) {
		load(str.c_str(), str.size() * sizeof(Char));
		if (append_null) {
			Char str_end[1] = { 0 };
			load(str_end, sizeof(Char));
		}
		return true;
	}
	bool load(const void* src, size_t src_size) {
		grow(src_size + size);
		memcpy(data + size, src, src_size);
		size += src_size;
		return true;
	}
	bool load(const String& file_path) {
		FileWrap f(file_path, L"rb");
		if (!f.is_open()) {
			return false;
		}
		size_t file_size = f.size();
		if (!file_size) {
			return false;
		}
		grow(file_size + size);
		f.read(data + size, file_size);
		size += file_size;
		return true;
	}
	bool save(const String& file_path) {
		FileWrap f(file_path, L"wb");
		if (!f.is_open()) {
			return false;
		}
		f.write(data, size);
		return true;
	}

private:
	struct FileWrap {
		FILE* f;
		FileWrap(const String& path, const String& mode) { f = _wfopen(path.c_str(), mode.c_str()); }
		~FileWrap() { f&& fclose(f); f = 0; }
		bool    is_open() { return f != 0; }
		size_t  write(Byte* data, size_t size) { return fwrite(data, 1, size, f); }
		size_t  read(Byte* data, size_t size) { return fread(data, 1, size, f); }
		size_t  size() {
			fseek(f, 0L, SEEK_END);
			size_t file_size = ftell(f);
			fseek(f, 0L, SEEK_SET);
			return file_size;
		}
	};
	void grow(size_t new_capacity) {
		if (capacity >= new_capacity) {
			return;
		}
		size_t new_cap = capacity ? capacity : 256;
		while (new_cap < new_capacity) {
			new_cap *= 2;
		}
		Byte* new_data = new Byte[new_cap];
		if (data) {
			memcpy(new_data, data, size);
			delete[] data;
		}
		data = new_data;
		capacity = new_cap;
	}
};

/**************************************************************************************************
 * Cache
 **************************************************************************************************/

struct Bmp {

	BITMAPFILEHEADER    file_header;
	BITMAPINFOHEADER    info_header;
	Buffer              bits;
	HBITMAP             hBmp;

	Bmp() : hBmp(0) {
		memset(&file_header, 0, sizeof(BITMAPFILEHEADER));
		memset(&info_header, 0, sizeof(BITMAPINFOHEADER));
	}

	void close() {
		::DeleteObject(hBmp);
		bits.free();
		hBmp = 0;
		memset(&file_header, 0, sizeof(BITMAPFILEHEADER));
		memset(&info_header, 0, sizeof(BITMAPINFOHEADER));
	}
	int total_size() {
		return file_header.bfSize;
	}
	int bits_size() {
		return file_header.bfSize - sizeof(BITMAPINFOHEADER) - sizeof(BITMAPFILEHEADER);
	}
	bool load_bits_and_headers(Byte* bytes) {
		close();
		int pos = 0;
		memcpy(&file_header, bytes + pos, sizeof(BITMAPFILEHEADER));
		pos += sizeof(BITMAPFILEHEADER);
		memcpy(&info_header, bytes + pos, sizeof(BITMAPINFOHEADER));
		pos += sizeof(BITMAPINFOHEADER);
		return fill_bitmap(bytes + pos, bits_size());
	}
	bool load_bits_only(Byte* bytes, int bits_size, int width, int height) {
		close();
		memset(&file_header, 0, sizeof(BITMAPFILEHEADER));
		file_header.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + bits_size;
		file_header.bfType = 0x4d42;
		file_header.bfOffBits = 0x36;
		info_header = create_info_header(width, height);
		return fill_bitmap(bytes, bits_size);
	}
	bool serialize(Buffer& buffer) {
		buffer.load(&file_header, sizeof(BITMAPFILEHEADER));
		buffer.load(&info_header, sizeof(BITMAPINFOHEADER));
		buffer.load(bits.data, bits.size);
		return true;
	}
	static BITMAPINFOHEADER create_info_header(int width, int height) {
		BITMAPINFOHEADER bmih = { 0 };
		bmih.biSize = sizeof(BITMAPINFOHEADER);
		bmih.biWidth = width;
		bmih.biHeight = height;
		bmih.biPlanes = 1;
		bmih.biBitCount = 32;
		bmih.biCompression = BI_RGB;
		return bmih;
	}
	static bool convert_file_icon(const HICON icon, Bmp& bmp) {
		static IWICImagingFactory* img_factory = 0;
		if (!img_factory) {
			// In VS 2011 beta, clsid has to be changed to CLSID_WICImagingFactory1 (from CLSID_WICImagingFactory)
			if (!SUCCEEDED(::CoInitialize(0)) || !SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory1, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&img_factory)))) {
				return false;
			}
		}
		IWICBitmap* pBitmap = 0;
		IWICFormatConverter* pConverter = 0;
		UINT cx = 0, cy = 0;
		if (SUCCEEDED(img_factory->CreateBitmapFromHICON(icon, &pBitmap))) {
			if (SUCCEEDED(img_factory->CreateFormatConverter(&pConverter))) {
				if (SUCCEEDED(pConverter->Initialize(pBitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, 0, 0.0f, WICBitmapPaletteTypeCustom))) {
					if (SUCCEEDED(pConverter->GetSize(&cx, &cy))) {
						const UINT stride = cx * sizeof(DWORD);
						const UINT buf_size = cy * stride;
						Byte* buf = new Byte[buf_size];
						pConverter->CopyPixels(0, stride, buf_size, buf);
						bmp.load_bits_only(buf, buf_size, cx, -(int)cy);
						delete[] buf;
					}
				}
				pConverter->Release();
			}
			pBitmap->Release();
		}
		::DestroyIcon(icon);

		return true;
	}

	static bool load_ico_file_directly(const String& file_path, Bmp& bmp) {
		static IWICImagingFactory* img_factory = 0;
		if (!img_factory) {
			if (!SUCCEEDED(::CoInitialize(0)) || !SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory1, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&img_factory)))) {
				return false;
			}
		}

		IWICBitmapDecoder* pDecoder = 0;
		IWICBitmapFrameDecode* pFrame = 0;
		IWICBitmapScaler* pScaler = 0;
		IWICFormatConverter* pConverter = 0;

		UINT cx = 0, cy = 0;

		// Load the image file
		if (FAILED(img_factory->CreateDecoderFromFilename(
			file_path.c_str(),
			nullptr,
			GENERIC_READ,
			WICDecodeMetadataCacheOnDemand,
			&pDecoder))) {
			return false;
		}

		// Get first frame
		if (FAILED(pDecoder->GetFrame(0, &pFrame))) {
			pDecoder->Release();
			return false;
		}

		// Get original dimensions
		UINT orig_width = 0, orig_height = 0;
		pFrame->GetSize(&orig_width, &orig_height);

		// Create scaler for high-quality downsampling to 32x32
		if (SUCCEEDED(img_factory->CreateBitmapScaler(&pScaler))) {
			if (SUCCEEDED(pScaler->Initialize(pFrame, 32, 32, WICBitmapInterpolationModeFant))) {
				if (SUCCEEDED(img_factory->CreateFormatConverter(&pConverter))) {
					if (SUCCEEDED(pConverter->Initialize(pScaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom))) {
						if (SUCCEEDED(pConverter->GetSize(&cx, &cy))) {
							const UINT stride = cx * sizeof(DWORD);
							const UINT buf_size = cy * stride;
							Byte* buf = new Byte[buf_size];
							if (SUCCEEDED(pConverter->CopyPixels(nullptr, stride, buf_size, buf))) {
								bmp.load_bits_only(buf, buf_size, cx, -(int)cy);
								delete[] buf;
								pConverter->Release();
								pScaler->Release();
								pFrame->Release();
								pDecoder->Release();
								return true;
							}
							delete[] buf;
						}
					}
					pConverter->Release();
				}
			}
			pScaler->Release();
		}

		pFrame->Release();
		pDecoder->Release();
		return false;
	}
	static HICON extract_file_icon(const String& file_path) {
		SHFILEINFOW file_info = { 0 };
		HIMAGELIST hfi = (HIMAGELIST)::SHGetFileInfo(file_path.c_str(), 0, &file_info, sizeof(SHFILEINFOW), SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
		return ::ImageList_GetIcon(hfi, file_info.iIcon, ILD_NORMAL);
	}

	static HICON extract_icon_from_path_with_index(const String& icon_path) {
		HICON hIcon = 0;
		String path = icon_path;
		int icon_index = 0;

		// Check if path contains icon index (e.g., "path.dll,2")
		size_t comma_pos = icon_path.find(L',');
		if (comma_pos != String::npos) {
			path = icon_path.substr(0, comma_pos);
			icon_index = _wtoi(icon_path.substr(comma_pos + 1).c_str());
		}

		// Expand environment variables if present
		TCHAR expanded_path[MAX_PATH] = { 0 };
		::ExpandEnvironmentStrings(path.c_str(), expanded_path, MAX_PATH);
		path = expanded_path;

		// For DLLs and other resources, use ExtractIconEx with large icon (32x32)
		::ExtractIconEx(path.c_str(), icon_index, &hIcon, 0, 1);

		if (!hIcon) {
			// Fallback to default file icon
			hIcon = extract_file_icon(path);
		}

		return hIcon;
	}

private:
	bool fill_bitmap(void* bytes, int byte_count) {
		Byte* buf = 0;
		if (!create_bitmap(info_header.biWidth, info_header.biHeight, (void**)(&buf), &hBmp)) {
			return false;
		}
		memcpy(buf, bytes, byte_count);
		return bits.load(bytes, byte_count);
	}
	static bool create_bitmap(int width, int height, void** bits, HBITMAP* phBmp) {
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader = create_info_header(width, height);
		*phBmp = ::CreateDIBSection(GetDC(0), &bmi, DIB_RGB_COLORS, bits, 0, 0);
		return *phBmp != 0;
	}
};

struct Cache {

	struct Item {
		String  name;
		Bmp     bmp;
		bool    is_submenu;
		bool    is_mini_submenu; // folder ends with .submenu-mini: force 16px icons for this submenu and its descendants
		String  submenu_path;
		String  relative_path; // For items in submenus

		Item() : is_submenu(false), is_mini_submenu(false) {}

		bool create(const String& file_name, const String& file_path, const String& base_dir = L"") {
			name = file_name;
			is_submenu = false;
			is_mini_submenu = false;
			submenu_path.clear();
			relative_path.clear();

			DWORD attrs = ::GetFileAttributes(file_path.c_str());
			if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {

				// Mark as submenu if needed (both .submenu and .submenu-mini folders are submenus,
				// including .submenu variants followed by a --singlesubmenu layout suffix)
				if (Util::IsMiniSubmenuFolderName(file_name)) {
					is_submenu = true;
					is_mini_submenu = true;
					submenu_path = file_path;
				}
				else if (Util::IsSubmenuFolderName(file_name)) {
					is_submenu = true;
					submenu_path = file_path;
				}
				// Hidden automatic detection: folders without an explicit
				// submenu/layout suffix are still treated as submenus if they
				// contain at least one shortcut/item or a nested folder. The
				// folder name on disk is left untouched.
				else if (Util::FolderHasShortcutOrSubfolder(file_path)) {
					is_submenu = true;
					submenu_path = file_path;
				}

				// Hidden per-folder ".stacky-config" (written by the
				// Configuration window) takes priority over the folder-name
				// suffix for the mini-icon flag, so ".submenu-mini"/".mini"
				// suffixes are no longer required once configured via the UI.
				StackyFolderConfig scfg = StackyFolderConfig::Load(file_path);
				if (is_submenu && scfg.loaded) is_mini_submenu = scfg.mini_icons;

				// A custom icon chosen in the Configuration window (root
				// "menu_icon_path" or per-submenu "submenu_icon_path")
				// takes priority over desktop.ini/the folder's own icon.
				if (scfg.loaded) {
					String customIconPath = is_submenu ? scfg.submenu_icon_path : scfg.menu_icon_path;
					if (!customIconPath.empty()) {
						if (Util::ends_with(customIconPath, L".ico") && customIconPath.find(L',') == String::npos) {
							if (Bmp::load_ico_file_directly(customIconPath, bmp)) {
								return true;
							}
						} else {
							HICON hIcon = Bmp::extract_icon_from_path_with_index(customIconPath);
							if (hIcon) {
								if (Bmp::convert_file_icon(hIcon, bmp)) {
									return true;
								}
							}
						}
					}
				}

				// For ANY folder: try custom icon from desktop.ini first
				String icon_path = Util::ReadIconFromDesktopIni(file_path);
				if (!icon_path.empty()) {
					// Try direct .ico file loading for better quality
					if (Util::ends_with(icon_path, L".ico")) {
						if (Bmp::load_ico_file_directly(icon_path, bmp)) {
							return true;
						}
					}
					else {
						HICON hIcon = Bmp::extract_icon_from_path_with_index(icon_path);
						if (hIcon) {
							if (Bmp::convert_file_icon(hIcon, bmp)) {
								return true;
							}
						}
					}
				}

				// Fallback to normal folder icon
				if (Bmp::convert_file_icon(Bmp::extract_file_icon(file_path), bmp)) {
					return true;
				}
			}

			// Regular item - check if it's an .ico file first for better quality
			if (Util::ends_with(file_path, L".ico")) {
				if (Bmp::load_ico_file_directly(file_path, bmp)) {
					return true;
				}
			}

			// Web shortcut: try favicon cache first (if no custom icon)
			if (!base_dir.empty() && !Util::HasCustomIcon(file_path)) {
				String web_url = Util::GetWebUrl(file_path);
				if (!web_url.empty()) {
					String favicon_path = Util::GetFaviconCachePath(base_dir, file_name);
					if (::GetFileAttributes(favicon_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
						// WIC can decode both ICO and PNG
						if (Bmp::load_ico_file_directly(favicon_path, bmp)) {
							return true;
						}
						// If WIC failed (e.g. malformed file) remove it so it gets re-fetched next time
						::DeleteFile(favicon_path.c_str());
					}
				}
			}

			// Otherwise use original extraction method
			if (!Bmp::convert_file_icon(Bmp::extract_file_icon(file_path), bmp)) {
				return false;
			}

			return true;
		}
		void serialize(Buffer& buffer) {
			buffer.load(name, true);
			buffer.load(&is_submenu, sizeof(is_submenu));
			if (is_submenu) {
				buffer.load(submenu_path, true);
			}
			buffer.load(&is_mini_submenu, sizeof(is_mini_submenu));
			bmp.serialize(buffer);
		}
		void unserialize(Buffer& buffer, size_t& pos) {
			name = (Char*)(buffer.data + pos);
			pos += (name.size() + 1) * sizeof(Char);

			memcpy(&is_submenu, buffer.data + pos, sizeof(is_submenu));
			pos += sizeof(is_submenu);

			if (is_submenu) {
				submenu_path = (Char*)(buffer.data + pos);
				pos += (submenu_path.size() + 1) * sizeof(Char);
			}

			memcpy(&is_mini_submenu, buffer.data + pos, sizeof(is_mini_submenu));
			pos += sizeof(is_mini_submenu);

			bmp.load_bits_and_headers(buffer.data + pos);
			pos += bmp.total_size();
		}
	};


	std::vector<Item>   items;
	int                 fixed_items;
	bool                was_rebuilt;
	String              base_dir;
	String              cache_path;  // public so favicon thread can delete it

	Cache(const String& stack_path) : last_modified(0), was_rebuilt(false), scanned_last_modified(0), fixed_items(0) {
		base_dir = Util::trim(Util::rtrim(stack_path, DIR_SEP), L"\"") + DIR_SEP;
		cache_path = path(CACHE_FILE_NAME);
	}

	String path(const String& file = L"") const {
		return base_dir + file;
	}

	bool scan() {
		return scan_directory(base_dir, L"");
	}

	bool scan_directory(const String& dir_path, const String& relative_path) {
		WIN32_FIND_DATA ffd = { 0 };
		HANDLE hfind = FindFirstFile((dir_path + L"*").c_str(), &ffd);
		if (hfind == INVALID_HANDLE_VALUE) {
			return false;
		}
		do {
			String filename = ffd.cFileName;
			if (filename == L"." || filename == L".." || ffd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN || Util::ends_with(filename, L".ignore") || filename == DESKTOP_INI)
				continue;

			String full_filename = relative_path + filename;
			scanned_items.push_back(full_filename);
			update_max_modified(full_filename);

			// If this is a .submenu or .submenu-mini folder, or an automatically
			// detected hidden submenu folder (contains shortcuts/subfolders),
			// recursively scan it.
			if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
				(Util::IsSubmenuFolderName(filename) || Util::ends_with(filename, SUBMENU_MINI_SUFFIX) ||
				 Util::FolderHasShortcutOrSubfolder(dir_path + filename + DIR_SEP))) {
				scan_directory(dir_path + filename + DIR_SEP, full_filename + DIR_SEP);
			}
		} while (FindNextFile(hfind, &ffd) != 0);
		FindClose(hfind);
		return true;
	}

	bool load() {
		Buffer buffer;
		items.clear();

		if (!buffer.load(cache_path)) {
			// Cache file doesn't exist, will rebuild
			rebuild();
			was_rebuilt = true;
			return true;
		}

		// Check cache version
		size_t pos = 0;
		if (buffer.size < sizeof(DWORD)) {
			// Invalid or old cache format
			rebuild();
			was_rebuilt = true;
			return true;
		}

		DWORD version = 0;
		memcpy(&version, buffer.data + pos, sizeof(DWORD));
		pos += sizeof(DWORD);

		if (version != CACHE_VERSION) {
			// Cache format changed, rebuild
			rebuild();
			was_rebuilt = true;
			return true;
		}

		// Load items
		for (; pos < buffer.size; ) {
			Item item;
			item.unserialize(buffer, pos);
			items.push_back(item);
		}
		last_modified = Util::get_modified(cache_path);

		if (is_outdated()) {
			rebuild();
			was_rebuilt = true;
		}

		return true;
	}

	// Unconditionally rebuilds the cache from the already-scanned directory
	// contents, regardless of what is_outdated() would say. Used by
	// RebuildStackyCache() (called from the Configuration window right after
	// saving a ".stacky-config") because editing a hidden config file doesn't
	// reliably bump the containing folder's modification time on NTFS, so
	// is_outdated()'s timestamp heuristic can miss the change and leave the
	// stale cache (e.g. an outdated mini_icons flag) in place.
	void force_rebuild() {
		rebuild();
		was_rebuilt = true;
	}

private:
	Time        last_modified;
	StringList  scanned_items;
	Time        scanned_last_modified;

	bool rebuild() {
		Buffer buffer;
		items.clear();

		// Write cache version first
		buffer.load(&CACHE_VERSION, sizeof(CACHE_VERSION));

		Item item;
		item.create(Util::rtrim(path(), DIR_SEP), path(), base_dir);
		item.serialize(buffer);
		items.push_back(item);
		for (size_t i = 0; i < scanned_items.size(); i++) {
			String file_name = scanned_items[i];
			item.create(file_name, path(file_name), base_dir);
			item.serialize(buffer);
			items.push_back(item);
		}

		save(buffer);
		return true;
	}
	void save(Buffer buffer) {
		::DeleteFile(cache_path.c_str());
		buffer.save(cache_path);
		::SetFileAttributes(cache_path.c_str(), FILE_ATTRIBUTE_HIDDEN);
	}
	void update_max_modified(const String& filename) {
		Time ft = Util::get_modified(path(filename));
		scanned_last_modified = scanned_last_modified < ft ? ft : scanned_last_modified;
	}
	bool is_outdated() {
		if (scanned_last_modified > last_modified || items.size() < 1 || scanned_items.size() + 1 != items.size()) {
			return true;
		}
		for (size_t i = 0; i < scanned_items.size(); i++) if (scanned_items[i] != items[i + 1].name) {
			return true;
		}
		return false;
	}
};

// Forces an immediate rescan/rebuild of the hidden "!stacky.cache" file for
// folderPath. Deleting the cache file alone (as the Configuration window
// used to do) isn't enough to make the change visible the first time the
// menu is opened afterwards: Cache::load() only rebuilds *lazily*, the next
// time stacky-plus.exe itself scans the folder, so the very first open right
// after a config change could still show a half-populated/no-icon menu
// (built from a stale in-memory state) before a *second* open finally
// reflects it. Scanning+rebuilding here, synchronously, from the
// Configuration window itself right after saving ".stacky-config" avoids
// that altogether: the cache on disk is already fresh and correct before the
// menu is ever opened again.
//
// Note: this always force-rebuilds (via force_rebuild(), not load()),
// because is_outdated()'s timestamp-based staleness check can miss a
// ".stacky-config" edit (editing a hidden file doesn't reliably bump the
// containing folder's own modification time on NTFS), which previously
// caused toggled settings like "mini icons" to sometimes not take effect
// after clicking "Guardar".
void RebuildStackyCache(const std::wstring& folderPath) {
	Cache cache(folderPath);
	if (cache.scan()) {
		cache.force_rebuild();
	}
}

struct MenuEntry {
	Cache::Item* item;     // points to cache item (folder or file)
	String text;           // display text (trimmed)
	bool is_submenu;
	bool populated;        // for lazy submenus
	String submenu_prefix; // relative prefix like L\"Foo.submenu\\\\\"
	bool is_path = false;
};

/**************************************************************************************************
 * DPI-aware icon cache for owner-draw
 **************************************************************************************************/
struct IconCache {
	// bmp/srcSz refer to the ORIGINAL (never modified/owned) source bitmap, while
	// sz is the logical draw size requested for this (source, iconPx) pair. The
	// actual scaling to sz is done at draw time via AlphaBlend, which (unlike
	// StretchBlt in HALFTONE mode) correctly interpolates the premultiplied alpha
	// channel. This avoids pre-scaling into a separate bitmap altogether, so we
	// never own/delete the original Cache::Item bitmaps here.
	struct Entry { HBITMAP bmp; SIZE srcSz; SIZE sz; };
	struct Key {
		const void* src;
		int px;
		bool operator==(const Key& o) const { return src == o.src && px == o.px; }
	};
	struct KeyHash {
		size_t operator()(const Key& k) const {
			return std::hash<const void*>()(k.src) ^ (std::hash<int>()(k.px) << 1);
		}
	};
	std::unordered_map<Key, Entry, KeyHash> map;

	Entry& get(HWND hwnd, HBITMAP src, int iconPx = NORMAL_ICON_PX) {
		Key key{ src, iconPx };
		auto it = map.find(key);
		if (it != map.end()) return it->second;

		UINT dpi = GetDpiForWindow(hwnd);
		int s = MulDiv(iconPx, dpi, 96);

		// Get source bitmap dimensions
		BITMAP bm;
		GetObject(src, sizeof(BITMAP), &bm);

		return map[key] = { src, { bm.bmWidth, bm.bmHeight }, { s, s } };
	}

	// Nothing to delete: all cached bitmaps alias the original Cache::Item bitmaps,
	// which are owned (and deleted) by Cache::Item/Bmp itself.
	~IconCache() {}
};

/**************************************************************************************************
 * Lazy submenu payload
 **************************************************************************************************/
struct LazySubmenuData {
	Cache* cache;
	String folder_path;
};

// Forward declaration: defined later as a free helper (used by App::MaxNameWidthPx
// for --singlesubmenu column-width measurement, and by GridWndProc/GridShowTip).
static String GridDisplayName(const String& name, const String& prefix);

/**************************************************************************************************
 * The app
 **************************************************************************************************/
struct App {

	App(Cache* c, const String& options) : cache(c), window(0) {
		// Hide header (shortcuts folder) by default
		hide_header = true;
		// Allow showing header if explicitly requested
		if (options.find(L"--show-header") != String::npos) {
			hide_header = false;
		}
		compact_header = options.find(L"--compact-header") != String::npos;
		bool wantDark  = options.find(L"--dark-mode") != String::npos;
		bool wantLight = options.find(L"--light-mode") != String::npos;
		if (wantDark)       theme_mode = THEME_DARK;
		else if (wantLight) theme_mode = THEME_LIGHT;
		else                theme_mode = THEME_SYSTEM;
		dark_mode = (theme_mode == THEME_DARK) ||
			(theme_mode == THEME_SYSTEM && GetSystemTheme().dark);
		mouse_position = options.find(L"--mouseposition") != String::npos;
		mini_mode = options.find(L"--mini") != String::npos;
		single_submenu_mode = options.find(L"--singlesubmenu") != String::npos;
		folders_first = options.find(L"--foldersfirst") != String::npos;

		// Parse iconmenu-NN / iconmenu-NN-name / iconmenu-C2
		grid_mode = GRID_NONE;
		icon_cols = 4;
		size_t im = options.find(L"iconmenu-");
		if (im != String::npos) {
			const wchar_t* p = options.c_str() + im + 9; // skip "iconmenu-"
			if (p[0] == L'C' || p[0] == L'c') {
				// iconmenu-C2 (cascade mode, always 2 cols) / iconmenu-C2-name (cascade + names)
				const wchar_t* afterC = p + 2; // skip "C2"
				if (wcsncmp(afterC, L"-name", 5) == 0)
					grid_mode = GRID_CASCADE_NAME;
				else
					grid_mode = GRID_CASCADE;
				icon_cols = 2;
			} else if (p[0] == L'F' || p[0] == L'f') {
				// iconmenu-F2: row-based layout, submenu items on row 1, others on row 2
				// iconmenu-F2-name: same, with truncated name below each icon
				const wchar_t* afterF = p + 2; // skip "F2"
				if (wcsncmp(afterF, L"-name", 5) == 0)
					grid_mode = GRID_F2_NAME;
				else
					grid_mode = GRID_F2;
				icon_cols = 2;
			} else {
				wchar_t* end = nullptr;
				long n = wcstol(p, &end, 10);
				if (end != p && n >= 1 && n <= 99) icon_cols = (int)n;
				// check for -name-right / -name suffix (check the longer one first)
				if (end && wcsncmp(end, L"-name-right", 11) == 0)
					grid_mode = GRID_NAME_RIGHT;
				else if (end && wcsncmp(end, L"-name", 5) == 0)
					grid_mode = GRID_NAME;
				else
					grid_mode = GRID_ICON;
			}
		}

		// A hidden ".stacky-config" inside the root stack folder (written by
		// the Configuration window) overrides the command-line options above,
		// so menus configured through the UI no longer need any --argument.
		StackyFolderConfig rootCfg = StackyFolderConfig::Load(cache->base_dir);
		if (rootCfg.loaded) {
			if (rootCfg.theme == SCFG_THEME_DARK)       theme_mode = THEME_DARK;
			else if (rootCfg.theme == SCFG_THEME_LIGHT) theme_mode = THEME_LIGHT;
			else                                        theme_mode = THEME_SYSTEM;
			dark_mode = (theme_mode == THEME_DARK) ||
				(theme_mode == THEME_SYSTEM && GetSystemTheme().dark);
			mouse_position = rootCfg.mouse_position;
			mini_mode = rootCfg.mini_icons;
			single_submenu_mode = rootCfg.mode == SCFG_MODE_SINGLESUB;
			folders_first = rootCfg.sort_mode == SCFG_SORT_FOLDERSFIRST;

			switch (rootCfg.mode) {
			case SCFG_MODE_ICONGRID:
				icon_cols = rootCfg.grid_cols;
				if (rootCfg.grid_names_right)    grid_mode = GRID_NAME_RIGHT;
				else if (rootCfg.grid_names_below) grid_mode = GRID_NAME;
				else                              grid_mode = GRID_ICON;
				break;
			case SCFG_MODE_DOUBLE_COL:
				icon_cols = 2;
				grid_mode = rootCfg.doublecol_names ? GRID_CASCADE_NAME : GRID_CASCADE;
				break;
			case SCFG_MODE_DOUBLE_ROW:
				icon_cols = 2;
				grid_mode = rootCfg.doublerow_names ? GRID_F2_NAME : GRID_F2;
				break;
			case SCFG_MODE_SINGLESUB:
				grid_mode = GRID_NONE; // root stays as default list in --singlesubmenu
				break;
			default:
				grid_mode = GRID_NONE;
				break;
			}
		}
	}

	bool init() {
		// Create window
		Util::kill_other_stackies();
		WNDCLASS wc{0};
		wc.lpfnWndProc = window_proc;
		wc.hInstance = GetModuleHandle(nullptr);
		wc.lpszClassName = STACKY_WINDOW_NAME;
		RegisterClass(&wc);

		// Register popup class
		WNDCLASS wcPopup{0};
		wcPopup.lpfnWndProc = PopupWndProc;
		wcPopup.hInstance = GetModuleHandle(nullptr);
		wcPopup.lpszClassName = STACKY_POPUP_CLASS;
		wcPopup.hCursor = LoadCursor(nullptr, IDC_ARROW);
		RegisterClass(&wcPopup);

		// Register icon-grid class
		WNDCLASS wcGrid{0};
		wcGrid.lpfnWndProc = GridWndProc;
		wcGrid.hInstance = GetModuleHandle(nullptr);
		wcGrid.lpszClassName = STACKY_GRID_CLASS;
		wcGrid.hCursor = LoadCursor(nullptr, IDC_ARROW);
		RegisterClass(&wcGrid);

		// Register tooltip class
		WNDCLASS wcTip{0};
		wcTip.lpfnWndProc = TipWndProc;
		wcTip.hInstance = GetModuleHandle(nullptr);
		wcTip.lpszClassName = STACKY_TIP_CLASS;
		RegisterClass(&wcTip);

		window = CreateWindow(STACKY_WINDOW_NAME, STACKY_WINDOW_NAME,
			WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);

		HMENU menu = CreatePopupMenu();

		// Install hook BEFORE showing menu to catch window creation events
		// This ensures rounded corners are applied to both root menu and submenus
		g_hMenuHook = SetWindowsHookEx(WH_CBT, MenuWindowHook, nullptr, GetCurrentThreadId());

		build_root_menu(menu);

		// Calculate menu position above the Stacky taskbar icon
		int menuX = 0, menuY = 0;
		if (grid_mode != GRID_NONE) {
			auto gm = MeasureGridSize();
			CalculateMenuPosition(menuX, menuY, gm.totalW, gm.totalH);
			ShowIconGridAt(menuX, menuY);
		} else {
			auto menuSz = MeasureMenuSize();
			CalculateMenuPosition(menuX, menuY, menuSz.w, menuSz.h);
			// Show owner-drawn popup at calculated position
			g_isFirstMenu = true;
			ShowPopupAt(menuX, menuY);
		}

		// Uninstall hook after popup is closed
		if (g_hMenuHook) {
			UnhookWindowsHookEx(g_hMenuHook);
			g_hMenuHook = nullptr;
		}
		return true;
	}

	void run() {
		MSG msg;
		while (GetMessage(&msg, nullptr, 0, 0)) DispatchMessage(&msg);
	}

	// Launches a fresh stacky-plus.exe process with no arguments, which opens
	// the advanced Configuration window (see wWinMain's ERR_PATH_MISSING/empty
	// command-line branch). Used by the "Settings"/"Configuraci\u00F3n" context-menu
	// entry so it doesn't need to run the config UI's own message loop nested
	// inside this already-running popup/grid instance.
	void LaunchConfigWindow() {
		String exePath = Util::GetExeFolder() + L"\\" + STACKY_EXEC_NAME;
		ShellExecute(nullptr, nullptr, exePath.c_str(), L"", nullptr, SW_SHOWNORMAL);
	}

	// Build and show the right-click context menu for a submenu-folder icon.
	// owner: window that will own the popup (its DPI is used for icon scaling).
	// submenu_path: absolute path of the target .submenu / .submenu-mini folder.
	// Returns after the user picks an item (or dismisses the menu) and performs the action.
	void ShowSubfolderContextMenu(HWND owner, const String& submenu_path) {
		UINT dpi = GetDpiForWindow(owner);
		int px = MulDiv(16, dpi, 96);
		const auto& S = Util::GetSubmenuCtxMenuStrings();

		HBITMAP bmpTree     = Util::CreateFolderTreeBitmap(px);
		HBITMAP bmpFolder   = Util::CreateStockFolderBitmap(px);
		HBITMAP bmpStacky   = Util::CreateStackyExeBitmap(px);
		HBITMAP bmpCross    = Util::CreateRedCrossBitmap(px);
		HBITMAP bmpSettings = Util::CreateSettingsGearBitmap(px);

		HMENU ctxMenu = CreatePopupMenu();

		MENUITEMINFO mii{ sizeof(mii) };
		mii.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
		mii.dwTypeData = (LPWSTR)S.open_subfolder;
		mii.wID = WM_CTX_OPEN_SUBFOLDER;
		mii.hbmpItem = bmpTree;
		InsertMenuItem(ctxMenu, -1, TRUE, &mii);

		MENUITEMINFO mii2{ sizeof(mii2) };
		mii2.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
		mii2.dwTypeData = (LPWSTR)S.open_menu_folder;
		mii2.wID = WM_CTX_OPEN_MENU_FOLDER;
		mii2.hbmpItem = bmpFolder;
		InsertMenuItem(ctxMenu, -1, TRUE, &mii2);

		MENUITEMINFO mii3{ sizeof(mii3) };
		mii3.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
		mii3.dwTypeData = (LPWSTR)S.open_stacky_folder;
		mii3.wID = WM_CTX_OPEN_STACKY_FOLDER;
		mii3.hbmpItem = bmpStacky;
		InsertMenuItem(ctxMenu, -1, TRUE, &mii3);

		MENUITEMINFO sep{ sizeof(sep) };
		sep.fMask = MIIM_FTYPE;
		sep.fType = MFT_SEPARATOR;
		InsertMenuItem(ctxMenu, -1, TRUE, &sep);

		MENUITEMINFO miiSettings{ sizeof(miiSettings) };
		miiSettings.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
		miiSettings.dwTypeData = (LPWSTR)S.settings;
		miiSettings.wID = WM_CTX_OPEN_SETTINGS;
		miiSettings.hbmpItem = bmpSettings;
		InsertMenuItem(ctxMenu, -1, TRUE, &miiSettings);

		MENUITEMINFO sep2{ sizeof(sep2) };
		sep2.fMask = MIIM_FTYPE;
		sep2.fType = MFT_SEPARATOR;
		InsertMenuItem(ctxMenu, -1, TRUE, &sep2);

		MENUITEMINFO mii4{ sizeof(mii4) };
		mii4.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
		mii4.dwTypeData = (LPWSTR)S.close_menu;
		mii4.wID = WM_CTX_CLOSE_MENU;
		mii4.hbmpItem = bmpCross;
		InsertMenuItem(ctxMenu, -1, TRUE, &mii4);

		Util::EnableDarkContextMenu(dark_mode);

		POINT pt{}; GetCursorPos(&pt);
		SetForegroundWindow(owner);
		UINT cmd = (UINT)TrackPopupMenuEx(ctxMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
		PostMessage(owner, WM_NULL, 0, 0); // needed so the menu dismisses correctly on some Windows versions

		Util::EnableDarkContextMenu(false);

		// Restore foreground/focus to the root popup/grid window (walking up through
		// hover-child/submenu parents if needed) so it correctly receives WM_KILLFOCUS
		// (and thus closes, cascading to any open children) on the next click outside of it.
		RestoreFocusToRootMenuWindow(owner);

		DestroyMenu(ctxMenu);
		if (bmpTree)     DeleteObject(bmpTree);
		if (bmpFolder)   DeleteObject(bmpFolder);
		if (bmpStacky)   DeleteObject(bmpStacky);
		if (bmpCross)    DeleteObject(bmpCross);
		if (bmpSettings) DeleteObject(bmpSettings);

		switch (cmd) {
		case WM_CTX_OPEN_SUBFOLDER:
			ShellExecute(nullptr, nullptr, submenu_path.c_str(), nullptr, nullptr, SW_NORMAL);
			break;
		case WM_CTX_OPEN_MENU_FOLDER:
			ShellExecute(nullptr, nullptr, cache->path().c_str(), nullptr, nullptr, SW_NORMAL);
			break;
		case WM_CTX_OPEN_STACKY_FOLDER:
			ShellExecute(nullptr, nullptr, Util::GetExeFolder().c_str(), nullptr, nullptr, SW_NORMAL);
			break;
		case WM_CTX_OPEN_SETTINGS:
			LaunchConfigWindow();
			break;
		case WM_CTX_CLOSE_MENU:
			// Intentionally no-op: the context menu is already closed at this point
			// (TrackPopupMenuEx returned) and the owning popup/grid window is left untouched.
			break;
		default:
			break;
		}
	}

	// Build and show the right-click context menu for a shortcut (non-submenu) icon.
	// owner: window that will own the popup (its DPI is used for icon scaling).
	// shortcut_path: absolute path of the target shortcut/file.
	// isRoot: true if this shortcut lives directly in the root menu folder, in
	// which case the "Open shortcut location" entry is omitted (it would just
	// open the same folder as "Open main menu folder").
	// Returns after the user picks an item (or dismisses the menu) and performs the action.
	void ShowShortcutContextMenu(HWND owner, const String& shortcut_path, bool isRoot) {
		UINT dpi = GetDpiForWindow(owner);
		int px = MulDiv(16, dpi, 96);
		const auto& S = Util::GetSubmenuCtxMenuStrings();

		// Determine the real executable target so we can offer "Run as administrator"
		// for shortcuts (.lnk) that point to an .exe, as well as direct .exe files.
		String targetPath = shortcut_path;
		size_t extPos = shortcut_path.find_last_of(L'.');
		bool isLnk = extPos != String::npos && _wcsicmp(shortcut_path.c_str() + extPos, L".lnk") == 0;
		if (isLnk) {
			Char resolved[MAX_PATH] = { 0 };
			if (SUCCEEDED(Util::ResolveShortcut(owner, shortcut_path.c_str(), resolved, MAX_PATH)) && resolved[0]) {
				targetPath = resolved;
			}
		}
		size_t targetExtPos = targetPath.find_last_of(L'.');
		bool isExe = targetExtPos != String::npos && _wcsicmp(targetPath.c_str() + targetExtPos, L".exe") == 0;

		HBITMAP bmpOpenFolder = isRoot ? nullptr : Util::CreateOpenFolderBitmap(px);
		HBITMAP bmpFolder     = Util::CreateStockFolderBitmap(px);
		HBITMAP bmpStacky     = Util::CreateStackyExeBitmap(px);
		HBITMAP bmpCross      = Util::CreateRedCrossBitmap(px);
		HBITMAP bmpShield     = isExe ? Util::CreateShieldBitmap(px) : nullptr;
		HBITMAP bmpSettings   = Util::CreateSettingsGearBitmap(px);

		HMENU ctxMenu = CreatePopupMenu();

		if (isExe && bmpShield) {
			MENUITEMINFO miiAdmin{ sizeof(miiAdmin) };
			miiAdmin.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
			miiAdmin.dwTypeData = (LPWSTR)S.run_as_admin;
			miiAdmin.wID = WM_CTX_RUN_AS_ADMIN;
			miiAdmin.hbmpItem = bmpShield;
			InsertMenuItem(ctxMenu, -1, TRUE, &miiAdmin);

			MENUITEMINFO sepAdmin{ sizeof(sepAdmin) };
			sepAdmin.fMask = MIIM_FTYPE;
			sepAdmin.fType = MFT_SEPARATOR;
			InsertMenuItem(ctxMenu, -1, TRUE, &sepAdmin);
		}

		if (!isRoot) {
			MENUITEMINFO mii{ sizeof(mii) };
			mii.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
			mii.dwTypeData = (LPWSTR)S.open_shortcut_location;
			mii.wID = WM_CTX_OPEN_SHORTCUT_LOCATION;
			mii.hbmpItem = bmpOpenFolder;
			InsertMenuItem(ctxMenu, -1, TRUE, &mii);
		}

		MENUITEMINFO mii2{ sizeof(mii2) };
		mii2.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
		mii2.dwTypeData = (LPWSTR)S.open_menu_folder;
		mii2.wID = WM_CTX_OPEN_MENU_FOLDER;
		mii2.hbmpItem = bmpFolder;
		InsertMenuItem(ctxMenu, -1, TRUE, &mii2);

		MENUITEMINFO mii3{ sizeof(mii3) };
		mii3.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
		mii3.dwTypeData = (LPWSTR)S.open_stacky_folder;
		mii3.wID = WM_CTX_OPEN_STACKY_FOLDER;
		mii3.hbmpItem = bmpStacky;
		InsertMenuItem(ctxMenu, -1, TRUE, &mii3);

		MENUITEMINFO sep{ sizeof(sep) };
		sep.fMask = MIIM_FTYPE;
		sep.fType = MFT_SEPARATOR;
		InsertMenuItem(ctxMenu, -1, TRUE, &sep);

		MENUITEMINFO miiSettings{ sizeof(miiSettings) };
		miiSettings.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
		miiSettings.dwTypeData = (LPWSTR)S.settings;
		miiSettings.wID = WM_CTX_OPEN_SETTINGS;
		miiSettings.hbmpItem = bmpSettings;
		InsertMenuItem(ctxMenu, -1, TRUE, &miiSettings);

		MENUITEMINFO sep2{ sizeof(sep2) };
		sep2.fMask = MIIM_FTYPE;
		sep2.fType = MFT_SEPARATOR;
		InsertMenuItem(ctxMenu, -1, TRUE, &sep2);

		MENUITEMINFO mii4{ sizeof(mii4) };
		mii4.fMask = MIIM_STRING | MIIM_ID | MIIM_BITMAP;
		mii4.dwTypeData = (LPWSTR)S.close_menu;
		mii4.wID = WM_CTX_CLOSE_MENU;
		mii4.hbmpItem = bmpCross;
		InsertMenuItem(ctxMenu, -1, TRUE, &mii4);

		Util::EnableDarkContextMenu(dark_mode);

		POINT pt{}; GetCursorPos(&pt);
		SetForegroundWindow(owner);
		UINT cmd = (UINT)TrackPopupMenuEx(ctxMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
		PostMessage(owner, WM_NULL, 0, 0); // needed so the menu dismisses correctly on some Windows versions

		Util::EnableDarkContextMenu(false);

		// Restore foreground/focus to the root popup/grid window (walking up through
		// hover-child/submenu parents if needed) so it correctly receives WM_KILLFOCUS
		// (and thus closes, cascading to any open children) on the next click outside of it.
		RestoreFocusToRootMenuWindow(owner);

		DestroyMenu(ctxMenu);
		if (bmpOpenFolder) DeleteObject(bmpOpenFolder);
		if (bmpFolder)     DeleteObject(bmpFolder);
		if (bmpStacky)     DeleteObject(bmpStacky);
		if (bmpCross)      DeleteObject(bmpCross);
		if (bmpShield)     DeleteObject(bmpShield);
		if (bmpSettings)   DeleteObject(bmpSettings);

		switch (cmd) {
		case WM_CTX_RUN_AS_ADMIN: {
			SHELLEXECUTEINFO sei{ sizeof(sei) };
			sei.fMask = SEE_MASK_DEFAULT;
			sei.hwnd = owner;
			sei.lpVerb = L"runas";
			sei.lpFile = targetPath.c_str();
			size_t tpos = targetPath.find_last_of(L"\\/");
			String tfolder = (tpos == String::npos) ? String() : targetPath.substr(0, tpos);
			sei.lpDirectory = tfolder.empty() ? nullptr : tfolder.c_str();
			sei.nShow = SW_NORMAL;
			ShellExecuteEx(&sei);
			break;
		}
		case WM_CTX_OPEN_SHORTCUT_LOCATION: {
			String p = shortcut_path;
			size_t pos = p.find_last_of(L"\\/");
			String folder = (pos == String::npos) ? cache->path() : p.substr(0, pos);
			ShellExecute(nullptr, nullptr, folder.c_str(), nullptr, nullptr, SW_NORMAL);
			break;
		}
		case WM_CTX_OPEN_MENU_FOLDER:
			ShellExecute(nullptr, nullptr, cache->path().c_str(), nullptr, nullptr, SW_NORMAL);
			break;
		case WM_CTX_OPEN_STACKY_FOLDER:
			ShellExecute(nullptr, nullptr, Util::GetExeFolder().c_str(), nullptr, nullptr, SW_NORMAL);
			break;
		case WM_CTX_OPEN_SETTINGS:
			LaunchConfigWindow();
			break;
		case WM_CTX_CLOSE_MENU:
			// Intentionally no-op: the context menu is already closed at this point
			// (TrackPopupMenuEx returned) and the owning popup/grid window is left untouched.
			break;
		default:
			break;
		}
	}

	friend LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	friend LRESULT CALLBACK GridWndProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	friend LRESULT CALLBACK TipWndProc  (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	// Data members accessed by free helper functions (GridShowTip, GridOpenSubChild)
	Cache*    cache;
	IconCache icon_cache;
	bool      dark_mode;
	ThemeMode theme_mode = THEME_SYSTEM;
	GridMode  grid_mode;
	int       icon_cols;
	bool      mini_mode; // --mini: force 16px icons for every menu/submenu
	bool      single_submenu_mode; // --singlesubmenu: root menu identical to default, submenus become
									// flat icon-grids (no nested submenus) with per-folder layout variants
	bool      folders_first; // --foldersfirst: .submenu folders listed before plain shortcuts, alphabetically within each group	// Effective selection/highlight color: system accent when following the
	// system theme, otherwise the fixed dark/light selection colors already
	// used elsewhere. Accent is only read once (cached in g_sysTheme) and
	// refreshed on system change notifications.
	// --light-mode ignores the Windows accent color entirely and always uses
	// the fixed hover color #E5E5E5.
	COLORREF SelectionColor() const {
		if (theme_mode == THEME_LIGHT) return RGB(0xE5, 0xE5, 0xE5);
		if (theme_mode == THEME_SYSTEM) return GetSystemTheme().accent;
		return dark_mode ? RGB(64, 64, 64) : GetSysColor(COLOR_HIGHLIGHT);
	}

	// Effective menu/submenu background color. --light-mode always uses the
	// fixed color #F9F9F9 instead of the system menu color.
	COLORREF BackgroundColor() const {
		if (theme_mode == THEME_LIGHT) return RGB(0xF9, 0xF9, 0xF9);
		return dark_mode ? RGB(32, 32, 32) : GetSysColor(COLOR_MENU);
	}

	// Text color to use over the selection/hover background. --light-mode uses
	// the fixed hover background (#E5E5E5) which is near-white, so the
	// highlighted text must stay dark instead of using COLOR_HIGHLIGHTTEXT
	// (typically white), otherwise it becomes invisible.
	COLORREF SelectionTextColor() const {
		if (theme_mode == THEME_LIGHT) return RGB(0, 0, 0);
		return dark_mode ? RGB(255, 255, 255) : GetSysColor(COLOR_HIGHLIGHTTEXT);
	}

	// Returns true if the menu identified by `prefix` ("" =root) should use mini (16px)
	// icons: either --mini was passed on the command line, or the prefix chain
	// contains a folder ending in .submenu-mini (which propagates to all descendants).
	bool IsMiniPrefix(const String& prefix) const {
		if (mini_mode) return true;
		if (prefix.empty()) return false;
		// prefix looks like "Foo.submenu\\Bar.submenu-mini\\"; check each ancestor
		// folder name (the cache item whose name+DIR_SEP is a leading segment of prefix).
		for (auto& ci : cache->items) {
			if (!ci.is_submenu) continue;
			String withSep = ci.name + DIR_SEP;
			if (prefix.rfind(withSep, 0) == 0 && ci.is_mini_submenu) return true;
		}
		return false;
	}

	// Icon size (in logical px, before DPI scaling) to use for a given menu prefix.
	int IconPxFor(const String& prefix) const {
		return IsMiniPrefix(prefix) ? MINI_ICON_PX : NORMAL_ICON_PX;
	}

	// --singlesubmenu variant selection.
	struct SubmenuVariant {
		GridMode mode;
		int      cols;
	};

	// Parses --singlesubmenu layout from the folder leaf name:
	//   "Music.icononly-4"     -> { GRID_SS_ICON, 4 }
	//   "Music.2-name-right"   -> { GRID_SS_NAME_RIGHT, 2 }
	//   "Music.3-name-below"   -> { GRID_SS_NAME_BELOW, 3 }
	//   "Music.submenu"        -> default { GRID_SS_ICON, 3 }
	// ".submenu" is not required; the layout token itself marks the folder.
	static SubmenuVariant ParseSubmenuVariantFromRawName(const String& rawName) {
		SubmenuVariant result{ GRID_SS_ICON, 3 };
		String leaf = Util::LastPathSegment(rawName);
		auto parsed = Util::ParseSSLayout(leaf);
		if (parsed.kind == Util::SS_LAYOUT_NAME_RIGHT) {
			result.mode = GRID_SS_NAME_RIGHT;
			result.cols = parsed.cols;
		} else if (parsed.kind == Util::SS_LAYOUT_NAME_BELOW) {
			result.mode = GRID_SS_NAME_BELOW;
			result.cols = parsed.cols;
		} else if (parsed.kind == Util::SS_LAYOUT_ICON) {
			result.mode = GRID_SS_ICON;
			result.cols = parsed.cols;
		}
		return result;
	}

	// Finds the .submenu Cache::Item whose relative prefix matches and returns
	// its parsed layout variant. prefix looks like "Folder.submenu\\".
	// A hidden ".stacky-config" inside the folder (written by the
	// Configuration window) takes priority over the folder-name layout
	// suffix (.icononly-NN / .NN-name-right / .NN-name-below), so the
	// suffix is no longer required once configured via the UI.
	SubmenuVariant SubmenuVariantFor(const String& prefix) const {
		if (!prefix.empty()) {
			String withoutSep = prefix.substr(0, prefix.size() - String(DIR_SEP).size());
			for (auto& ci : cache->items) {
				if (ci.is_submenu && ci.name == withoutSep) {
					StackyFolderConfig scfg = StackyFolderConfig::Load(cache->path(ci.name));
					if (scfg.loaded) {
						SubmenuVariant result{ GRID_SS_ICON, scfg.submenu_cols };
						if (scfg.submenu_layout == SCFG_LAYOUT_NAME_RIGHT) result.mode = GRID_SS_NAME_RIGHT;
						else if (scfg.submenu_layout == SCFG_LAYOUT_NAME_BELOW) result.mode = GRID_SS_NAME_BELOW;
						else result.mode = GRID_SS_ICON;
						return result;
					}
					return ParseSubmenuVariantFromRawName(ci.name);
				}
			}
		}
		return SubmenuVariant{ GRID_SS_ICON, 3 };
	}

	// The grid mode/columns actually in effect for a given prefix. In --singlesubmenu
	// mode, every non-root submenu (prefix non-empty) uses its own per-folder layout
	// variant instead of the app-wide grid_mode/icon_cols (root stays untouched).
	GridMode EffectiveGridMode(const String& prefix) const {
		if (single_submenu_mode && !prefix.empty()) return SubmenuVariantFor(prefix).mode;
		return grid_mode;
	}
	int EffectiveIconCols(const String& prefix) const {
		if (single_submenu_mode && !prefix.empty()) return SubmenuVariantFor(prefix).cols;
		return icon_cols;
	}

	// --foldersfirst: display key for a cache item used to compare items alphabetically
	// (sort-prefix, .submenu suffix, and known extensions stripped, so ordering matches
	// what the user actually sees in the menu).
	String SortKeyFor(const Cache::Item& it) const {
		String s = Util::LastPathSegment(it.name);
		s = Util::StripSortPrefix(s);
		if (it.is_submenu) {
			s = Util::StripSubmenuSuffix(s);
		} else {
			for (auto& ext : { L".lnk", L".bat", L".cmd", L".exe", L".vbs", L".url" })
				if (Util::ends_with(s, ext)) { s = Util::rtrim(s, ext); break; }
		}
		return s;
	}

	// --foldersfirst comparator: .submenu folders sort before plain shortcuts;
	// within each group, items are compared alphabetically (case-insensitive).
	bool FoldersFirstLess(size_t ai, size_t bi) const {
		auto& a = cache->items[ai];
		auto& b = cache->items[bi];
		if (a.is_submenu != b.is_submenu) return a.is_submenu;
		return _wcsicmp(SortKeyFor(a).c_str(), SortKeyFor(b).c_str()) < 0;
	}

	// Returns true if the menu/submenu identified by `prefix` ("" = root) has
	// "AGREGAR SEPARADOR DE SUBMENÚS Y ACCESOS DIRECTOS SIMPLES" enabled in
	// its .stacky-config, so a separator should be drawn automatically below
	// the last submenu folder item, before the plain shortcut items.
	bool AddSeparatorEnabled(const String& prefix) const {
		String folder = prefix.empty() ? cache->base_dir : cache->path(Util::rtrim(prefix, DIR_SEP));
		StackyFolderConfig cfg = StackyFolderConfig::Load(folder);
		return cfg.loaded && cfg.add_separator;
	}

	// Ordered list of direct-child item indices for the owner-drawn popup menu
	// (default and --singlesubmenu modes, non-grid rendering) at the given
	// prefix ("" = root). This is what MeasureMenuSize/WM_PAINT/WM_MOUSEMOVE/
	// WM_LBUTTONDOWN/WM_RBUTTONDOWN must all iterate over so the visible order
	// matches --foldersfirst; separators are kept inline in scan order when
	// --foldersfirst is off (matching build_root_menu/build_submenu), and
	// dropped when it's on (also matching them).
	// Sentinel index meaning "auto-inserted separator" (not backed by a real
	// cache item), used by AddSeparatorEnabled() grouping below. Consumers
	// (MeasureMenuSize/WM_PAINT/WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_RBUTTONDOWN)
	// must check for this value before indexing cache->items.
	static constexpr size_t kAutoSepIndex = (size_t)-1;

	std::vector<size_t> PopupItems(const String& prefix) const {
		std::vector<size_t> v;
		bool isRoot = prefix.empty();
		for (size_t i = (isRoot ? 1 : 0); i < cache->items.size(); ++i) {
			auto& it = cache->items[i];
			if (isRoot) {
				if (it.name.find(DIR_SEP) != String::npos) continue;
			} else {
				if (it.name.rfind(prefix, 0) != 0) continue;
				String rel = it.name.substr(prefix.size());
				if (rel.empty()) continue;
				if (rel.find(DIR_SEP) != String::npos) continue;
			}
			if (IsSeparatorFile(it.name)) {
				if (!folders_first) v.push_back(i);
				continue;
			}
			// --singlesubmenu: at the root, hide a submenu folder (.icononly-NN /
			// .NN-name-right / .NN-name-below) that only contains nested submenu
			// folders (no direct plain shortcuts), matching build_root_menu/GridItems.
			if (single_submenu_mode && isRoot && it.is_submenu) {
				String childPrefix = it.name + DIR_SEP;
				if (!SubmenuHasDirectPlainItem(childPrefix)) continue;
			}
			v.push_back(i);
		}
		if (folders_first) {
			std::sort(v.begin(), v.end(),
				[this](size_t a, size_t b) { return FoldersFirstLess(a, b); });
		}

		// "Agregar separador de submenús y accesos directos simples": groups
		// submenu folders before plain shortcuts (if not already grouped by
		// --foldersfirst) and inserts an auto separator between the two
		// groups, matching build_root_menu()/build_submenu(). Note: even
		// when --foldersfirst already sorts submenus before plain shortcuts,
		// it does not draw a separator line by itself, so the auto separator
		// must still be inserted in that case too.
		if (AddSeparatorEnabled(prefix)) {
			if (!folders_first) {
				std::stable_partition(v.begin(), v.end(),
					[this](size_t i) { return cache->items[i].is_submenu; });
			}
			for (size_t p = 1; p < v.size(); ++p) {
				if (!cache->items[v[p]].is_submenu && cache->items[v[p - 1]].is_submenu) {
					v.insert(v.begin() + p, kAutoSepIndex);
					break;
				}
			}
		}
		return v;
	}

private:
	HWND    window;
	bool    hide_header;
	bool    compact_header;
	bool    mouse_position;

	// helper: make a display label for the base folder
	const String header_label() {
		if (compact_header) {
			// show the last folder name instead of full path
			String p = cache->base_dir;
			if (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
			size_t pos = p.find_last_of(L"\\/");
			return (pos == String::npos) ? p : p.substr(pos + 1);
		}
		else {
			String p = cache->base_dir;
			if (!p.empty() && p.back() == L'\\')
				p.pop_back();
			return p;
		}
	}

	// Calculate optimal menu position above the Stacky taskbar icon.
	// Centers the menu horizontally over the cursor (= icon center at launch time).
	void CalculateMenuPosition(int& out_x, int& out_y, int menuWidth, int menuHeight) {
		if (mouse_position) {
			// --mouseposition: open the menu at the current mouse cursor position
			// (top-left corner of the menu at the cursor), clamped to the work area.
			POINT cursor{};
			GetCursorPos(&cursor);
			HMONITOR hMonitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
			RECT workArea = Util::GetWorkAreaForMonitor(hMonitor);

			int menuX = cursor.x;
			int menuY = cursor.y;

			if (menuX < workArea.left) menuX = workArea.left;
			if (menuX + menuWidth > workArea.right) menuX = workArea.right - menuWidth;
			if (menuY < workArea.top) menuY = workArea.top;
			if (menuY + menuHeight > workArea.bottom) menuY = workArea.bottom - menuHeight;

			out_x = menuX;
			out_y = menuY;
			return;
		}

		// Determine which monitor the cursor (= clicked taskbar icon) is on, so
		// the menu opens above the taskbar segment on THAT monitor rather than
		// always the primary monitor's taskbar (multi-monitor setups can have
		// the pinned shortcut visible - and clicked - on any monitor).
		POINT cursor{};
		GetCursorPos(&cursor);
		HMONITOR hMonitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
		RECT taskbarRect = Util::GetTaskbarRectForMonitor(hMonitor);
		RECT workArea = Util::GetWorkAreaForMonitor(hMonitor);

		// Y: bottom of menu aligns with top of taskbar
		int menuY = taskbarRect.top - menuHeight;
		if (menuY < workArea.top) menuY = workArea.top;

		// X: center menu over cursor (cursor is over the taskbar icon at launch)
		int menuX = cursor.x - menuWidth / 2;

		// Clamp X to work area
		if (menuX < workArea.left) menuX = workArea.left;
		if (menuX + menuWidth > workArea.right) menuX = workArea.right - menuWidth;

		out_x = menuX;
		out_y = menuY;
	}

	// Calculate approximate menu height based on cache items
	int CalculateApproximateMenuHeight() {
		UINT dpi = GetDpiForWindow(window);
		int itemHeight = MulDiv(32, dpi, 96); // Standard item height
		int separatorHeight = MulDiv(6, dpi, 96); // Separator height
		int padding = MulDiv(8, dpi, 96); // Top/bottom padding

		// Count items: skip base folder (item 0) if hidden
		int itemCount = 0;
		int separatorCount = 0;

		for (size_t i = hide_header ? 1 : 0; i < cache->items.size(); ++i) {
			auto& it = cache->items[i];

			// Only count direct children in root
			if (it.name.find(DIR_SEP) != String::npos) continue;

			// Count separators differently
			if (IsSeparatorFile(it.name)) {
				separatorCount++;
			} else {
				itemCount++;
			}
		}

		// If not hidden, add header item + separator
		if (!hide_header && cache->items.size() >= 1) {
			itemCount++; // header
			separatorCount++; // separator after header
		}

		int approximateHeight = padding * 2 + 
			(itemCount * itemHeight) + 
			(separatorCount * separatorHeight);

		// Add some buffer for safety
		approximateHeight += MulDiv(20, dpi, 96);

		return approximateHeight;
	}

	// Measure the exact pixel size needed to display all items with the given prefix.
	// Pass empty prefix for the root menu. Uses the same metrics as MakeLayout.
	struct MenuSize { int w; int h; };
	MenuSize MeasureMenuSize(const String& prefix = L"") {
		UINT dpi = GetDpiForWindow(window);
		int iconSz    = MulDiv(IconPxFor(prefix), dpi, 96);  // matches IconCache and MakeLayout
		int vPad      = MulDiv(6,  dpi, 96) * 9 / 10;
		int hPad      = MulDiv(8,  dpi, 96);
		int iconGap   = MulDiv(8,  dpi, 96);
		int arrowGap  = MulDiv(6,  dpi, 96);
		int arrowSz   = MulDiv(7,  dpi, 96);
		int sepH      = MulDiv(8,  dpi, 96);
		int rawItemH  = iconSz * 9 / 10 + vPad * 2;
		int itemH     = (rawItemH < iconSz) ? iconSz : rawItemH;
		// arrow width matches paint: h3 = arrowSz * 866 / 1000
		int arrowW    = arrowSz * 866 / 1000;

		HDC hdc = GetDC(window); // use app window DC for correct DPI-aware measurement
		NONCLIENTMETRICS ncm{}; ncm.cbSize = sizeof(ncm);
		SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
		ncm.lfMenuFont.lfWeight = FW_NORMAL;
		HFONT hf = CreateFontIndirect(&ncm.lfMenuFont);
		HGDIOBJ oldF = SelectObject(hdc, hf);

		int maxTextW = 0;
		int totalH   = vPad; // top window padding

		bool isRoot = prefix.empty();

		// Header item (only for root popup)
		if (isRoot && !hide_header && cache->items.size() >= 1) {
			auto& ci = cache->items[0];
			SIZE ts{};
			GetTextExtentPoint32(hdc, ci.name.c_str(), (int)ci.name.size(), &ts);
			if (ts.cx > maxTextW) maxTextW = ts.cx;
			totalH += itemH + sepH;
		}

		for (size_t i : PopupItems(prefix)) {
			if (i == kAutoSepIndex) { totalH += sepH; continue; }
			auto& ci = cache->items[i];

			if (IsSeparatorFile(ci.name)) {
					totalH += sepH;
					continue;
				}

			String disp = ci.name;
			if (!isRoot) disp = ci.name.substr(prefix.size());
			disp = Util::StripSortPrefix(disp);
			if (ci.is_submenu)
				disp = Util::StripSubmenuSuffix(disp);
			else
				for (auto& ext : { L".lnk", L".bat", L".cmd", L".exe", L".vbs", L".url" })
					if (Util::ends_with(disp, ext)) { disp = Util::rtrim(disp, ext); break; }

			SIZE ts{};
			GetTextExtentPoint32(hdc, disp.c_str(), (int)disp.size(), &ts);
			int needed = ts.cx + (ci.is_submenu ? (arrowGap + arrowW + hPad) : 0);
			if (needed > maxTextW) maxTextW = needed;
			totalH += itemH;
		}

		totalH += vPad; // bottom window padding

		SelectObject(hdc, oldF);
		DeleteObject(hf);
		ReleaseDC(window, hdc);

		// Extra safety margin avoids any font-metrics rounding truncation
		int margin = MulDiv(8, dpi, 96);
		int totalW = hPad + iconSz + iconGap + maxTextW + hPad + margin;
		if (totalW < MulDiv(120, dpi, 96)) totalW = MulDiv(120, dpi, 96);

		return { totalW, totalH };
	}

	// -----------------------------------------------------------------------
	// Icon-grid helpers (public so free helper functions can access them)
	// -----------------------------------------------------------------------
public:

	// Returns grid items for the given prefix (root="" or "Folder.submenu\\").
	// In GRID_CASCADE mode submenus are included; otherwise excluded.
	// The prefix="" always scans root-level items only.
	std::vector<size_t> GridItems(const String& prefix = L"") const {
		std::vector<size_t> v;
		bool isRoot = prefix.empty();
		GridMode effMode = EffectiveGridMode(prefix);
		for (size_t i = 1; i < cache->items.size(); ++i) {
			auto& ci = cache->items[i];
			if (isRoot) {
				if (ci.name.find(DIR_SEP) != String::npos) continue;
			} else {
				if (ci.name.rfind(prefix, 0) != 0) continue;
				String rel = ci.name.substr(prefix.size());
				if (rel.empty()) continue;
				if (rel.find(DIR_SEP) != String::npos) continue;
			}
				if (IsSeparatorFile(ci.name)) continue;
					// --singlesubmenu grids (GRID_SS_*) never show nested submenu folders:
					// no submenu can have child submenus in this mode.
					if (ci.is_submenu && (effMode == GRID_SS_ICON || effMode == GRID_SS_NAME_RIGHT || effMode == GRID_SS_NAME_BELOW)) continue;
					// --singlesubmenu: at the root, hide a .submenu folder that only
					// contains nested .submenu folders (no direct plain shortcuts).
					if (single_submenu_mode && isRoot && ci.is_submenu) {
						String childPrefix = ci.name + DIR_SEP;
						if (!SubmenuHasDirectPlainItem(childPrefix)) continue;
					}
					if (ci.is_submenu && effMode != GRID_CASCADE && effMode != GRID_CASCADE_NAME && effMode != GRID_F2 && effMode != GRID_F2_NAME
						&& effMode != GRID_SS_ICON && effMode != GRID_SS_NAME_RIGHT && effMode != GRID_SS_NAME_BELOW) continue;
					v.push_back(i);
				}
				if (folders_first) {
					std::sort(v.begin(), v.end(),
						[this](size_t a, size_t b) { return FoldersFirstLess(a, b); });
				}
				if (effMode == GRID_F2 || effMode == GRID_F2_NAME) {
					std::vector<size_t> withSub, withoutSub;
					for (auto idx : v) {
						if (cache->items[idx].is_submenu) withSub.push_back(idx);
						else withoutSub.push_back(idx);
					}
					v.clear();
					v.insert(v.end(), withSub.begin(), withSub.end());
					v.insert(v.end(), withoutSub.begin(), withoutSub.end());
				} else if ((effMode == GRID_CASCADE || effMode == GRID_CASCADE_NAME) && !isRoot) {
					// Submenu (non-root): when the multi-column layout applies (n > 2 items,
					// mixed submenu/plain items, not all items being submenus), group all
					// submenu items first so GridCellPos can place them (plus enough leading
					// plain items to pad the column to full height) in a single column,
					// matching MeasureGridSize's grouping condition.
					int n = (int)v.size();
					int subCount = 0;
					for (auto idx : v) if (cache->items[idx].is_submenu) subCount++;
					if (n > 2 && subCount > 0 && subCount < n) {
						std::vector<size_t> withSub, withoutSub;
						for (auto idx : v) {
							if (cache->items[idx].is_submenu) withSub.push_back(idx);
							else withoutSub.push_back(idx);
						}
						v.clear();
						v.insert(v.end(), withSub.begin(), withSub.end());
						v.insert(v.end(), withoutSub.begin(), withoutSub.end());
					}
				}
				return v;
			}

	struct GridMetrics {
		int cellSz;   // icon-area side = iconSz + cellPad*2
		int cellW;    // actual column width used for x-stepping (== cellSz except for
					  // GRID_SS_NAME_RIGHT / GRID_SS_NAME_BELOW, which are wider)
		int cellPad;  // padding around icon inside cell
		int labelH;   // extra height for name label (0 unless GRID_NAME / GRID_SS_NAME_BELOW)
		int cellH;    // total cell height = cellSz + labelH
		int cols;
		int rows;
		int totalW;
		int totalH;
		int f2Row1Count; // GRID_F2 only: number of items placed in row 1 (submenu items).
						 // Also reused for GRID_CASCADE/GRID_CASCADE_NAME submenus in
						 // subTwoCol mode: number of items placed in the submenu column.
		int topPad;      // GRID_F2 only: extra space reserved above row 0 for the upward submenu triangle
		bool subTwoCol;  // GRID_CASCADE/GRID_CASCADE_NAME submenus only: laid out in 2 columns
						 // (submenu items grouped in one column, plain items in the other)
	};

	// Returns the width (in device pixels, at the given dpi) of the widest display
	// name among the grid items for `prefix`, measured with the main-menu font, and
	// the max/actual number of text lines used (1 for SS_NAME_RIGHT, 1-2 for SS_NAME_BELOW).
	int MaxNameWidthPx(const std::vector<size_t>& its, const String& prefix, HDC hdc) const {
		int maxW = 0;
		for (auto idx : its) {
			String disp = GridDisplayName(cache->items[idx].name, prefix);
			SIZE ts{};
			GetTextExtentPoint32(hdc, disp.c_str(), (int)disp.size(), &ts);
			if (ts.cx > maxW) maxW = ts.cx;
		}
		return maxW;
	}

	// Measure grid for a given prefix (root = empty).
	GridMetrics MeasureGridSize(const String& prefix = L"") const {
		UINT dpi    = GetDpiForWindow(window);
		GridMode effMode = EffectiveGridMode(prefix);
		int iconSz  = MulDiv(IconPxFor(prefix), dpi, 96);
		int cellPad = MulDiv(8,  dpi, 96);
		int cellSz  = iconSz + cellPad * 2;
		int labelH  = (effMode == GRID_NAME || effMode == GRID_CASCADE_NAME || effMode == GRID_F2_NAME) ? MulDiv(16, dpi, 96) : 0;
		int cellW   = cellSz;
		int cellH   = cellSz + labelH;
		auto its    = GridItems(prefix);
		int n       = (int)its.size();
		int cols, rows, f2Row1Count;
		bool subTwoCol = false;

		if (effMode == GRID_SS_NAME_RIGHT || effMode == GRID_NAME_RIGHT) {
			cols = EffectiveIconCols(prefix);
			if (cols < 1) cols = 1;
			rows = n > 0 ? (n + cols - 1) / cols : 1;
			f2Row1Count = cols;
			HDC hdc = GetDC(window);
			NONCLIENTMETRICS ncm{}; ncm.cbSize = sizeof(ncm);
			SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
			ncm.lfMenuFont.lfWeight = FW_NORMAL;
			HFONT hf = CreateFontIndirect(&ncm.lfMenuFont);
			HGDIOBJ oldF = SelectObject(hdc, hf);
			int maxTextW = MaxNameWidthPx(its, prefix, hdc);
			SelectObject(hdc, oldF); DeleteObject(hf); ReleaseDC(window, hdc);
			int maxAllowed = MulDiv(200, dpi, 96);
			int textW = (maxTextW > maxAllowed) ? maxAllowed : maxTextW;
			int textGap = 0;
			int rightPad = MulDiv(8, dpi, 96); // keep the name from touching the submenu's right border
			cellW = cellSz + textGap + textW;
			return { cellSz, cellW, cellPad, labelH, cellSz, cols, rows, cellW * cols + rightPad, cellSz * rows, f2Row1Count, 0, false };
		} else if (effMode == GRID_SS_NAME_BELOW) {
			cols = EffectiveIconCols(prefix);
			if (cols < 1) cols = 1;
			rows = n > 0 ? (n + cols - 1) / cols : 1;
			f2Row1Count = cols;
			int sidePad = MulDiv(24, dpi, 96);
			int topGap  = MulDiv(8,  dpi, 96);
			HDC hdc = GetDC(window);
			NONCLIENTMETRICS ncm{}; ncm.cbSize = sizeof(ncm);
			SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
			ncm.lfMenuFont.lfWeight = FW_NORMAL;
			HFONT hf = CreateFontIndirect(&ncm.lfMenuFont);
			HGDIOBJ oldF = SelectObject(hdc, hf);
			TEXTMETRIC tm{}; GetTextMetrics(hdc, &tm);
			SelectObject(hdc, oldF); DeleteObject(hf); ReleaseDC(window, hdc);
			int lineH = tm.tmHeight + tm.tmExternalLeading;
			int nameH = lineH; // single line only
			int bottomPad = MulDiv(8, dpi, 96); // keep names from touching the submenu's bottom border
			cellW = iconSz + sidePad * 2;
			int cellSzHere = iconSz + topGap; // icon area incl. top gap (icon is drawn below the gap)
			cellH = cellSzHere + nameH;
			return { cellSzHere, cellW, sidePad, nameH, cellH, cols, rows, cellW * cols, cellH * rows + bottomPad, f2Row1Count, 0, false };
		}

		if ((effMode == GRID_CASCADE || effMode == GRID_CASCADE_NAME) && !prefix.empty()) {
			// Submenu (non-root) layout:
			// - 2 items, or all items have a submenu -> single column.
			// - No items have a submenu -> plain 2-column grid (column-major wrapping).
			// - 3+ items with a mix of submenu/plain items -> 2 or more columns, with all
			//   submenu items (plus enough plain items to pad the column to full height)
			//   grouped into a single column placed on the side the submenu opens towards
			//   (decided later in GridCellPos via openRight). Aim for a roughly square
			//   arrangement to minimize empty cells.
			// Submenu (non-root) layout:
			// - 1 item -> single row, single column.
			// - 2 items, or all items have a submenu -> single column.
			// - No items have a submenu -> plain 2-column grid (column-major wrapping).
			// - 3+ items with a mix of submenu/plain items -> exactly 2 columns, with all
			//   submenu items (plus enough plain items to pad the column, minimizing empty
			//   cells) grouped into a single column placed on the side the submenu opens
			//   towards (decided later in GridCellPos via openRight).
			int subCount = 0;
			for (auto idx : its) if (cache->items[idx].is_submenu) subCount++;
			int nonSubCount = n - subCount;
			if (n <= 1) {
				// A single item never needs more than one column.
				cols        = 1;
				rows        = n > 0 ? n : 1;
				f2Row1Count = 0;
			} else if (n == 2 || subCount == n) {
				// 2 items (regardless of submenu status), or all items have a submenu.
				cols        = 1;
				rows        = n;
				f2Row1Count = 0;
			} else if (subCount == 0) {
				cols        = 2;
				rows        = (n + cols - 1) / cols;
				f2Row1Count = 0;
			} else {
				subTwoCol = true;
				cols = 2;
				// Submenu column must hold all subCount items; use the minimal row
				// count that fits all n items in exactly 2 columns, but never less
				// than subCount (the submenu column cannot overflow into a 3rd column).
				int minRowsFor2Cols = (n + 1) / 2; // ceil(n/2)
				rows = max(subCount, minRowsFor2Cols);
				f2Row1Count = rows; // submenu column height: subs + filler plain items
			}
		} else if (grid_mode == GRID_F2 || grid_mode == GRID_F2_NAME) {
			int subCount = 0;
			for (auto idx : its) if (cache->items[idx].is_submenu) subCount++;
			int nonSubCount = n - subCount;
			// Single row if <=3 items, or if all items have a submenu (never put a
			// submenu item on row 2). Having zero submenu items does NOT force a
			// single row - with >=4 plain items we still want 2 rows.
			bool singleRow = (n <= 3) || (subCount == n && n > 0);
			if (singleRow) {
				cols = n > 0 ? n : 1;
				rows = 1;
				f2Row1Count = n;
			} else if (subCount >= nonSubCount) {
				// Submenu items (row 1) are at least as many as plain items (row 2).
				cols = subCount;
				rows = 2;
				f2Row1Count = subCount;
			} else {
				// Plain items outnumber submenu items: move some plain items into row 1
				// (alongside all submenu items, which always come first) to balance the
				// two rows and reduce the number of columns. All submenu items still end
				// up in row 1 because GridItems() places them first and f2Row1Count >= subCount.
				f2Row1Count = (n + 1) / 2; // ceil(n/2), always >= subCount since subCount < n/2
				cols = max(f2Row1Count, n - f2Row1Count);
				rows = 2;
			}
		} else {
			cols = EffectiveIconCols(prefix);
			if (cols < 1) cols = 1;
			rows = n > 0 ? (n + cols - 1) / cols : 1;
			f2Row1Count = cols; // unused outside GRID_F2
		}
		int topPad = 0; // GRID_F2 no longer reserves extra space; triangle is drawn inside the cell.
		return { cellSz, cellW, cellPad, labelH, cellH, cols, rows, cellSz * cols, cellH * rows + topPad, f2Row1Count, topPad, subTwoCol };
	}

	// Create params passed to GridWndProc for both root and submenu grids.
	struct GridCreateParams {
		App*   app;
		String prefix;     // "" for root, "Folder.submenu\\" for child
		HWND   parentHwnd; // nullptr for root
		int    openRight;  // +1=open submenus to the right, -1=left, 0=auto (root)
	};

	void ShowIconGridAt(int x, int y) {
		auto* cp = new GridCreateParams{ this, L"", nullptr, 0 };
		auto gm  = MeasureGridSize();
		HWND grid = CreateWindowEx(
			WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
			STACKY_GRID_CLASS, L"",
			WS_POPUP | WS_BORDER,
			x, y, gm.totalW, gm.totalH,
			nullptr, nullptr, GetModuleHandle(nullptr), cp);
		if (!grid) { delete cp; return; }
		Util::SetWindowRoundedCorners(grid);
		ShowWindow(grid, SW_SHOW);
		SetForegroundWindow(grid);
		SetFocus(grid);
		UpdateWindow(grid);
		MSG msg;
		while (IsWindow(grid)) {
			while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			Sleep(5);
		}
	}
	// prefix="" for root menu, "FolderName.submenu\\" for submenus.
	// anchorX/anchorY: preferred top-left corner in screen coords.
	void ShowPopupWithPrefix(const String& prefix, int x, int y) {
		auto sz = MeasureMenuSize(prefix);

		// Clamp to monitor work area
		HMONITOR hMon = MonitorFromPoint({ x, y }, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi{ sizeof(mi) };
		GetMonitorInfo(hMon, &mi);
		RECT& wa = mi.rcWork;
		if (x + sz.w > wa.right)  x = wa.right  - sz.w;
		if (y + sz.h > wa.bottom) y = wa.bottom  - sz.h;
		if (x < wa.left) x = wa.left;
		if (y < wa.top)  y = wa.top;

		auto* state = new PopupState{ this, prefix };

		HWND popup = CreateWindowEx(
			WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
			STACKY_POPUP_CLASS,
			L"",
			WS_POPUP | WS_BORDER,
			x, y, sz.w, sz.h,
			nullptr, nullptr, GetModuleHandle(nullptr), state);

		if (!popup) { delete state; return; }

		Util::SetWindowRoundedCorners(popup);
		ShowWindow(popup, SW_SHOW);
		SetForegroundWindow(popup);
		SetFocus(popup);
		UpdateWindow(popup);

		MSG msg;
		while (IsWindow(popup)) {
			while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			Sleep(5);
		}
	}

	// Create and show an owner-drawn popup at screen coordinates (x,y).
	// Blocks until the popup window is closed.
	void ShowPopupAt(int x, int y) {
		ShowPopupWithPrefix(L"", x, y);
	}

	static void InsertSeparator(HMENU menu) {
		MENUITEMINFO mii{ sizeof(mii) };
		mii.fMask = MIIM_FTYPE | MIIM_DATA;
		mii.fType = MFT_OWNERDRAW;
		mii.dwItemData = (ULONG_PTR)nullptr;   // null itemData = separator marker
		InsertMenuItem(menu, -1, TRUE, &mii);
	}

	static bool IsSeparatorFile(String name) {
		// handle ".separator" and ".separator.lnk"
		if (Util::ends_with(name, L".lnk")) name = Util::rtrim(name, L".lnk");
		return Util::ends_with(name, L".separator");
	}

	// --singlesubmenu: returns true if the submenu folder identified by
	// `prefix` (e.g. "Apps.submenu\\") has at least one direct-child item
	// that is a plain shortcut (not itself a .submenu folder, not a separator).
	// Used to decide whether a root-level .submenu folder should be hidden
	// (when it contains only nested .submenu folders) or shown (when it has
	// at least one simple shortcut alongside possible nested submenus).
	bool SubmenuHasDirectPlainItem(const String& prefix) const {
		for (size_t i = 1; i < cache->items.size(); ++i) {
			auto& ci = cache->items[i];
			if (ci.name.rfind(prefix, 0) != 0) continue;
			String rel = ci.name.substr(prefix.size());
			if (rel.empty()) continue;
			if (rel.find(DIR_SEP) != String::npos) continue; // only direct children
			if (IsSeparatorFile(ci.name)) continue;
			if (ci.is_submenu) continue; // a nested .submenu folder, not a plain shortcut
			return true;
		}
		return false;
	}

	void build_root_menu(HMENU menu) {
		if (!hide_header && cache->items.size() >= 1) {
			auto* e = new MenuEntry{};
			e->item = &cache->items[0];      // base folder cache item
			e->is_submenu = false;
			e->populated = false;
			e->is_path = true;
			e->text = header_label();

			MENUITEMINFO mii{ sizeof(mii) };
			mii.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STRING | MIIM_ID;
			mii.fType = MFT_OWNERDRAW;
			mii.dwItemData = (ULONG_PTR)e;
			mii.dwTypeData = (LPWSTR)e->text.c_str();
			mii.wID = WM_OPEN_TARGET_FOLDER;  // special command

			InsertMenuItem(menu, -1, TRUE, &mii);

			// separator below the folder item (matches old behavior)
			InsertSeparator(menu);
		}

		// Collect eligible root-level indices first (so --foldersfirst can reorder
		// them before menu items are actually inserted). When --foldersfirst is
		// off, items are inserted (and separators placed) in the original scan
		// order, exactly as before.
		std::vector<size_t> rootIdx;
		for (size_t i = 1; i < cache->items.size(); ++i) {
			auto& it = cache->items[i];

			// root: only direct children
			if (it.name.find(DIR_SEP) != String::npos) continue;

			if (IsSeparatorFile(it.name)) {
				if (!folders_first) InsertSeparator(menu);
				continue;
			}

			// --singlesubmenu: hide a .submenu folder from the main menu if it
			// contains only nested .submenu folders (no direct plain shortcuts).
			if (single_submenu_mode && it.is_submenu) {
				String childPrefix = it.name + DIR_SEP;
				if (!SubmenuHasDirectPlainItem(childPrefix)) continue;
			}

			rootIdx.push_back(i);
		}
		if (folders_first) {
			std::sort(rootIdx.begin(), rootIdx.end(),
				[this](size_t a, size_t b) { return FoldersFirstLess(a, b); });
		}

		// "Agregar separador de submenús y accesos directos simples": groups
		// submenu folders before plain shortcuts (if not already grouped by
		// --foldersfirst) and inserts a separator between the two groups.
		// Even when --foldersfirst already sorts submenus before plain
		// shortcuts, it doesn't draw a separator line by itself, so the
		// auto separator insertion below must still run in that case.
		bool addSep = AddSeparatorEnabled(L"");
		if (addSep && !folders_first) {
			std::stable_partition(rootIdx.begin(), rootIdx.end(),
				[this](size_t i) { return cache->items[i].is_submenu; });
		}

		bool insertedAutoSep = false;
		for (size_t idxPos = 0; idxPos < rootIdx.size(); ++idxPos) {
			size_t i = rootIdx[idxPos];
			auto& it = cache->items[i];

			if (addSep && !insertedAutoSep && !it.is_submenu && idxPos > 0 &&
				cache->items[rootIdx[idxPos - 1]].is_submenu) {
				InsertSeparator(menu);
				insertedAutoSep = true;
			}

			// create MenuEntry once; never store mixed pointer types
			auto* e = new MenuEntry{};
			e->item = &it;
			e->is_submenu = it.is_submenu;
			e->populated = false;

			// display text
			if (it.is_submenu) {
				String t = it.name;
				t = Util::StripSubmenuSuffix(t);
				e->text = t;
				e->submenu_prefix = it.name + DIR_SEP; // RELATIVE prefix!
			}
			else {
				String t = it.name;
				t = Util::rtrim(t, L".bat");
				t = Util::rtrim(t, L".cmd");
				t = Util::rtrim(t, L".exe");
				t = Util::rtrim(t, L".lnk");
				t = Util::rtrim(t, L".url");
				t = Util::rtrim(t, L".vbs");
				e->text = t;
			}

			MENUITEMINFO mii{ sizeof(mii) };
			mii.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STRING | (it.is_submenu ? MIIM_SUBMENU : MIIM_ID);
			mii.fType = MFT_OWNERDRAW;
			mii.dwItemData = (ULONG_PTR)e;
			mii.dwTypeData = (LPWSTR)e->text.c_str();

			if (it.is_submenu) {
				mii.hSubMenu = CreatePopupMenu();
			}
			else {
				mii.wID = WM_MENU_ITEM + (UINT)i; // unique ID per item
			}

			InsertMenuItem(menu, -1, TRUE, &mii);
		}
	}

	void build_submenu(HMENU menu, const String& prefix) {
		std::vector<size_t> idx;
		for (size_t i = 0; i < cache->items.size(); ++i) {
			auto& it = cache->items[i];

			// must match relative prefix
			if (it.name.rfind(prefix, 0) != 0) continue;

			String rel = it.name.substr(prefix.size());

			if (IsSeparatorFile(rel)) {
				if (!folders_first) InsertSeparator(menu);
				continue;
			}

			// direct children only (unless it.is_submenu)
			if (!it.is_submenu && rel.find(DIR_SEP) != String::npos) continue;

			// --singlesubmenu: nested .submenu folders are never shown inside
			// another submenu (no submenu can have child submenus in this mode).
			if (single_submenu_mode && it.is_submenu) continue;

			idx.push_back(i);
		}
		if (folders_first) {
			std::sort(idx.begin(), idx.end(),
				[this](size_t a, size_t b) { return FoldersFirstLess(a, b); });
		}

		bool addSep = AddSeparatorEnabled(prefix);
		if (addSep && !folders_first) {
			std::stable_partition(idx.begin(), idx.end(),
				[this](size_t i) { return cache->items[i].is_submenu; });
		}

		bool insertedAutoSep = false;
		for (size_t idxPos = 0; idxPos < idx.size(); ++idxPos) {
			size_t i = idx[idxPos];
			auto& it = cache->items[i];
			String rel = it.name.substr(prefix.size());

			if (addSep && !insertedAutoSep && !it.is_submenu && idxPos > 0 &&
				cache->items[idx[idxPos - 1]].is_submenu) {
				InsertSeparator(menu);
				insertedAutoSep = true;
			}

			auto* e = new MenuEntry{};
			e->item = &it;
			e->is_submenu = it.is_submenu;
			e->populated = false;

			if (it.is_submenu) {
				// must be direct child submenu folder
				if (rel.find(DIR_SEP) != String::npos) { delete e; continue; }
				e->text = Util::StripSubmenuSuffix(rel);
				e->submenu_prefix = it.name + DIR_SEP;
			}
			else {
				String t = rel;
				t = Util::rtrim(t, L".lnk");
				t = Util::rtrim(t, L".vbs");
				t = Util::rtrim(t, L".cmd");
				t = Util::rtrim(t, L".bat");
				e->text = t;
			}

			MENUITEMINFO mii{ sizeof(mii) };
			mii.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STRING | (it.is_submenu ? MIIM_SUBMENU : MIIM_ID);
			mii.fType = MFT_OWNERDRAW;
			mii.dwItemData = (ULONG_PTR)e;
			mii.dwTypeData = (LPWSTR)e->text.c_str();

			if (it.is_submenu) mii.hSubMenu = CreatePopupMenu();
			else mii.wID = WM_MENU_ITEM + (UINT)i;

			InsertMenuItem(menu, -1, TRUE, &mii);
		}
	}

	void on_init_menu_popup(HMENU hMenu) {
		int c = GetMenuItemCount(hMenu);
		for (int i = 0; i < c; ++i) {
			MENUITEMINFO mii{ sizeof(mii) };
			mii.fMask = MIIM_DATA | MIIM_SUBMENU;
			GetMenuItemInfo(hMenu, i, TRUE, &mii);

			if (!mii.hSubMenu) continue;

			auto* e = (MenuEntry*)mii.dwItemData;
			if (!e || !e->is_submenu || e->populated) continue;

			build_submenu(mii.hSubMenu, e->submenu_prefix);
			e->populated = true;
		}
	}

	void on_measure_item(MEASUREITEMSTRUCT* mis) {
		if (mis->CtlType != ODT_MENU) return;

		if (mis->itemData == 0) {
			UINT dpi = GetDpiForWindow(window);
			mis->itemHeight = MulDiv(6, dpi, 96);   // slim separator
			mis->itemWidth = 10;
			return;
		}

		auto* e = (MenuEntry*)mis->itemData;
		if (!e) return;

		UINT dpi = GetDpiForWindow(window);
		int icon = MulDiv(32, dpi, 96);
		int pad = MulDiv(8, dpi, 96);

		HDC hdc = GetDC(window);
		HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
		HFONT old = (HFONT)SelectObject(hdc, font);

		SIZE ts{};
		GetTextExtentPoint32(hdc, e->text.c_str(), (int)e->text.size(), &ts);

		SelectObject(hdc, old);
		ReleaseDC(window, hdc);

		// cap text width for path entries (smart ellipsis will be used when drawing)
		int maxText = ts.cx;
		if (e->is_path) {
			// cap to ~70% of work area width on the monitor where the cursor is
			HMONITOR mon = Util::GetMonitorFromCursor();
			RECT wa = Util::GetWorkAreaForMonitor(mon);
			int maxMenu = (int)((wa.right - wa.left) * 0.70);
			maxText = min(ts.cx, maxMenu);
		}

		mis->itemHeight = max((UINT)GetSystemMetrics(SM_CYMENU), (UINT)(icon + pad));
		mis->itemWidth = icon + pad + maxText + pad;
	}

	void on_draw_item(DRAWITEMSTRUCT* dis) {
		if (dis->CtlType != ODT_MENU) return;

		auto* e = (MenuEntry*)dis->itemData;
		// ----- SEPARATOR DRAW -----
		if (!e) {
			COLORREF bg = BackgroundColor();
			COLORREF line = dark_mode ? RGB(70, 70, 70) : GetSysColor(COLOR_3DSHADOW);

			// Fill background
			HBRUSH b = CreateSolidBrush(bg);
			FillRect(dis->hDC, &dis->rcItem, b);
			DeleteObject(b);

			// Full width, small padding
			UINT dpi = GetDpiForWindow(window);
			int pad = MulDiv(2, dpi, 96);

			int left = dis->rcItem.left + pad;
			int right = dis->rcItem.right - pad;
			int y = (dis->rcItem.top + dis->rcItem.bottom) / 2;

			// Draw line
			HPEN pen = CreatePen(PS_SOLID, 1, line);
			HPEN old = (HPEN)SelectObject(dis->hDC, pen);

			MoveToEx(dis->hDC, left, y, nullptr);
			LineTo(dis->hDC, right, y);

			SelectObject(dis->hDC, old);
			DeleteObject(pen);
			return;
		}

		if (!e || !e->item) return;

		const bool sel = (dis->itemState & ODS_SELECTED) != 0;
		const bool disab = (dis->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;

		// Colors
		const COLORREF bg = BackgroundColor();
		const COLORREF fg = dark_mode ? RGB(240, 240, 240) : GetSysColor(COLOR_MENUTEXT);
		const COLORREF disfg = dark_mode ? RGB(140, 140, 140) : GetSysColor(COLOR_GRAYTEXT);

		// Selection colors (avoid the bright default blue in dark mode)
		const COLORREF selBg = SelectionColor();
		const COLORREF selFg = SelectionTextColor();

		// Paint background
		HBRUSH hbr = CreateSolidBrush(sel ? selBg : bg);
		FillRect(dis->hDC, &dis->rcItem, hbr);
		DeleteObject(hbr);

		// Icon (DPI-scaled) + alpha blend
		auto& ic = icon_cache.get(window, e->item->bmp.hBmp);

		int x = dis->rcItem.left + 4;
		int y = dis->rcItem.top + (dis->rcItem.bottom - dis->rcItem.top - ic.sz.cy) / 2;

		HDC mem = CreateCompatibleDC(dis->hDC);
		HGDIOBJ old = SelectObject(mem, ic.bmp);

		BLENDFUNCTION bf{};
		bf.BlendOp = AC_SRC_OVER;
		bf.SourceConstantAlpha = disab ? 140 : 255; // slightly dim icons when disabled
		bf.AlphaFormat = AC_SRC_ALPHA;

		AlphaBlend(dis->hDC, x, y, ic.sz.cx, ic.sz.cy, mem, 0, 0, ic.srcSz.cx, ic.srcSz.cy, bf);

		SelectObject(mem, old);
		DeleteDC(mem);

		// Text
		RECT tr = dis->rcItem;
		tr.left += ic.sz.cx + 8;

		SetBkMode(dis->hDC, TRANSPARENT);
		SetTextColor(dis->hDC, disab ? disfg : (sel ? selFg : fg));

		UINT flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT;
		if (e->is_path) flags |= DT_PATH_ELLIPSIS;
		else           flags |= DT_END_ELLIPSIS;

		DrawText(dis->hDC, e->text.c_str(), -1, &tr, flags);
	}

	static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
		App* app = (App*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

		switch (msg) {
		case WM_NCCREATE:
			SetWindowLongPtr(hwnd, GWLP_USERDATA,
				(LONG_PTR)((CREATESTRUCT*)lp)->lpCreateParams);
			break;

		case WM_INITMENUPOPUP:
			app->on_init_menu_popup((HMENU)wp);
			break;

		case WM_MEASUREITEM:
			app->on_measure_item((MEASUREITEMSTRUCT*)lp);
			return TRUE;

		case WM_DRAWITEM:
			app->on_draw_item((DRAWITEMSTRUCT*)lp);
			return TRUE;

		case WM_COMMAND: {
			UINT id = LOWORD(wp);
			if (id == WM_OPEN_TARGET_FOLDER) {
				ShellExecute(nullptr, nullptr, app->cache->path().c_str(), nullptr, nullptr, SW_NORMAL);
				return TRUE;
			}

			if (id >= WM_MENU_ITEM) {
				size_t idx = id - WM_MENU_ITEM;
				auto& it = app->cache->items[idx];
				String cmd = app->cache->path(it.name);

				if (GetKeyState(VK_SHIFT) & 0x8000)
				{
					TCHAR  filepath[MAX_PATH] = { 0 };
					Util::ResolveShortcut(NULL, cmd.c_str(), filepath, _countof(filepath));

					ITEMIDLIST* pidl = ILCreateFromPath(filepath);
					if (pidl)
					{
						SHOpenFolderAndSelectItems(pidl, 0, 0, 0);
						ILFree(pidl);
					}
				}
				else
				{
					ShellExecute(nullptr, nullptr, cmd.c_str(), nullptr, nullptr, SW_NORMAL);
					Util::RegisterRecentLaunch(cmd);
					{
						String web_url = Util::GetWebUrl(cmd);
						if (!web_url.empty() && !Util::HasCustomIcon(cmd))
							Util::TriggerFaviconDownloadAsync(app->cache->base_dir, it.name, web_url, app->cache->cache_path, cmd);
					}
				}
			}
			break;
		}
		case WM_EXITMENULOOP:
			// WM_EXITMENULOOP is sent before WM_COMMAND, so the app termination has to be delayed.
			// This also allows to wait for the possible UAC prompt.
			::SetTimer(hwnd, 0, APP_EXIT_DELAY, 0);
			break;

		case WM_TIMER:
			::PostQuitMessage(0);
			::DestroyWindow(hwnd);
			break;
		}
		return DefWindowProc(hwnd, msg, wp, lp);
	}
};

/**************************************************************************************************
 * Owner-drawn popup window procedure
 **************************************************************************************************/

// Shared layout constants used in WM_PAINT, WM_MOUSEMOVE and WM_LBUTTONDOWN.
// All values are in pixels, DPI-scaled by the caller.
struct PopupLayout {
	int dpi;
	int iconSz;   // icon width/height
	int vPad;     // per-item vertical padding (top AND bottom inside each row)
	int hPad;     // horizontal outer padding
	int iconGap;  // gap between icon right edge and text left edge
	int sepH;     // separator height
	int itemH;    // total item row height = iconSz + 2*vPad
	int arrowSz;  // equilateral triangle side length
	int arrowGap; // gap between text right and arrow left
};

static PopupLayout MakeLayout(HWND hwnd, int iconPx = NORMAL_ICON_PX) {
	PopupLayout l{};
	l.dpi     = (int)GetDpiForWindow(hwnd);
	l.iconSz  = MulDiv(iconPx, l.dpi, 96);   // matches IconCache scaling
	l.vPad    = MulDiv(6,  l.dpi, 96) * 9 / 10; // 10% less row height
	l.hPad    = MulDiv(8,  l.dpi, 96);
	l.iconGap = MulDiv(8,  l.dpi, 96);
	l.sepH    = MulDiv(8,  l.dpi, 96);
	l.arrowSz = MulDiv(7,  l.dpi, 96);   // equilateral triangle side
	l.arrowGap= MulDiv(6,  l.dpi, 96);
	l.itemH   = l.iconSz * 9 / 10 + l.vPad * 2; // total row height ~10% smaller
	if (l.itemH < l.iconSz) l.itemH = l.iconSz; // never clip the icon
	return l;
}

LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_NCCREATE:
		SetWindowLongPtr(hwnd, GWLP_USERDATA,
			(LONG_PTR)((CREATESTRUCT*)lParam)->lpCreateParams);
		return TRUE;
	case WM_CREATE: {
		auto* state = (PopupState*)((CREATESTRUCT*)lParam)->lpCreateParams;
		if (state) state->hotItem = -1;
		return 0;
	}
	case WM_SETTINGCHANGE:
	case WM_DWMCOLORIZATIONCOLORCHANGED: {
		// System color mode/accent changed: refresh the cache and repaint if
		// this menu is following the system theme (event-driven, no polling).
		auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (state && state->app && state->app->theme_mode == THEME_SYSTEM) {
			RefreshSystemThemeCache();
			state->app->dark_mode = GetSystemTheme().dark;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		break;
	}
	case WM_ERASEBKGND:
		return 1; // suppress � WM_PAINT fills every pixel with double-buffer
	case WM_NCDESTROY: {
		auto* ps = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (ps) {
			// Destroy any open hover-child
			if (ps->childHwnd && IsWindow(ps->childHwnd))
				DestroyWindow(ps->childHwnd);
			// Notify parent that this hover-child is gone
			if (ps->parentHwnd && IsWindow(ps->parentHwnd)) {
				auto* parentState = (PopupState*)GetWindowLongPtr(ps->parentHwnd, GWLP_USERDATA);
				if (parentState && parentState->childHwnd == hwnd) {
					parentState->childHwnd = nullptr;
					parentState->hotSubIdx  = -1;
				}
			}
			delete ps;
			SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
		}
		return 0;
	}

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		App* app = state ? state->app : nullptr;
		RECT rc; GetClientRect(hwnd, &rc);

		// Double-buffer: render into a memory DC, blit once at the end
		HDC memDC  = CreateCompatibleDC(hdc);
		HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
		HGDIOBJ oldMemBmp = SelectObject(memDC, memBmp);

		if (!app) {
			HBRUSH hbr = CreateSolidBrush(app ? app->BackgroundColor() : GetSysColor(COLOR_MENU));
			FillRect(memDC, &rc, hbr);
			DeleteObject(hbr);
			BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
			SelectObject(memDC, oldMemBmp); DeleteObject(memBmp); DeleteDC(memDC);
			EndPaint(hwnd, &ps);
			return 0;
		}
		const String& prefix = state->prefix;

		// From here, paint into memDC instead of hdc
		HDC& hdc_ = memDC; // alias so the rest of the code below works unchanged

		PopupLayout L = MakeLayout(hwnd, app ? app->IconPxFor(prefix) : NORMAL_ICON_PX);

		COLORREF bgColor    = app->BackgroundColor();
		COLORREF selColor   = app->SelectionColor();
		COLORREF fgColor    = app->dark_mode ? RGB(240,240,240): GetSysColor(COLOR_MENUTEXT);
		COLORREF fgSelColor = app->SelectionTextColor();
		COLORREF sepColor   = app->dark_mode ? RGB(70,70,70)   : GetSysColor(COLOR_3DSHADOW);
		COLORREF arrowColor = app->dark_mode ? RGB(200,200,200): RGB(0,0,0);

		// Fill background
		HBRUSH bgBrush = CreateSolidBrush(bgColor);
		FillRect(hdc_, &rc, bgBrush);
		DeleteObject(bgBrush);

		// System menu font, non-bold
		NONCLIENTMETRICS ncm{}; ncm.cbSize = sizeof(ncm);
		SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
		ncm.lfMenuFont.lfWeight = FW_NORMAL;
		HFONT hMenuFont = CreateFontIndirect(&ncm.lfMenuFont);
		HGDIOBJ oldFont = SelectObject(hdc_, hMenuFont);

		int y = rc.top + L.vPad;

		// Header item (root only)
		bool isRoot = prefix.empty();
		if (isRoot && !app->hide_header && app->cache->items.size() >= 1) {
			auto& ci = app->cache->items[0];
			bool hot = (state->hotItem == 0);
			RECT ir = { rc.left, y, rc.right, y + L.itemH };
			HBRUSH hbr = CreateSolidBrush(hot ? selColor : bgColor);
			FillRect(hdc_, &ir, hbr); DeleteObject(hbr);

			auto& ic = app->icon_cache.get(hwnd, ci.bmp.hBmp, app->IconPxFor(prefix));
			int ix = ir.left + L.hPad;
			int iy = ir.top  + (L.itemH - ic.sz.cy) / 2;
			HDC mem = CreateCompatibleDC(hdc_); HGDIOBJ old = SelectObject(mem, ic.bmp);
			BLENDFUNCTION bf{}; bf.BlendOp=AC_SRC_OVER; bf.SourceConstantAlpha=255; bf.AlphaFormat=AC_SRC_ALPHA;
			AlphaBlend(hdc_, ix, iy, ic.sz.cx, ic.sz.cy, mem, 0,0, ic.srcSz.cx, ic.srcSz.cy, bf);
			SelectObject(mem, old); DeleteDC(mem);

			RECT tr = ir;
			tr.left  = ix + ic.sz.cx + L.iconGap;
			tr.right = ir.right - L.hPad;
			SetBkMode(hdc_, TRANSPARENT);
			SetTextColor(hdc_, hot ? fgSelColor : fgColor);
			DrawText(hdc_, ci.name.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
			y += L.itemH;

			// separator after header
			int yy = y + L.sepH / 2;
			HPEN pen = CreatePen(PS_SOLID, 1, sepColor);
			HPEN oldPen = (HPEN)SelectObject(hdc_, pen);
			MoveToEx(hdc_, rc.left + L.hPad, yy, nullptr);
			LineTo(hdc_, rc.right - L.hPad, yy);
			SelectObject(hdc_, oldPen); DeleteObject(pen);
			y += L.sepH;
		}

		for (size_t i : app->PopupItems(prefix)) {
			if (i == App::kAutoSepIndex) {
				int yy = y + L.sepH / 2;
				HPEN pen = CreatePen(PS_SOLID, 1, sepColor);
				HPEN oldPen = (HPEN)SelectObject(hdc_, pen);
				MoveToEx(hdc_, rc.left + L.hPad, yy, nullptr);
				LineTo(hdc_, rc.right - L.hPad, yy);
				SelectObject(hdc_, oldPen); DeleteObject(pen);
				y += L.sepH;
				continue;
			}
			auto& ci = app->cache->items[i];

			if (App::IsSeparatorFile(ci.name)) {
				int yy = y + L.sepH / 2;
				HPEN pen = CreatePen(PS_SOLID, 1, sepColor);
				HPEN oldPen = (HPEN)SelectObject(hdc_, pen);
				MoveToEx(hdc_, rc.left + L.hPad, yy, nullptr);
				LineTo(hdc_, rc.right - L.hPad, yy);
				SelectObject(hdc_, oldPen); DeleteObject(pen);
				y += L.sepH;
				continue;
			}

			bool hot = (state->hotItem == (int)i);
			RECT ir = { rc.left, y, rc.right, y + L.itemH };
			HBRUSH hbr = CreateSolidBrush(hot ? selColor : bgColor);
			FillRect(hdc_, &ir, hbr); DeleteObject(hbr);

			auto& ic = app->icon_cache.get(hwnd, ci.bmp.hBmp, app->IconPxFor(prefix));
			int ix = ir.left + L.hPad;
			int iy = ir.top  + (L.itemH - ic.sz.cy) / 2;
			HDC mem = CreateCompatibleDC(hdc_); HGDIOBJ old = SelectObject(mem, ic.bmp);
			BLENDFUNCTION bf{}; bf.BlendOp=AC_SRC_OVER; bf.SourceConstantAlpha=255; bf.AlphaFormat=AC_SRC_ALPHA;
			AlphaBlend(hdc_, ix, iy, ic.sz.cx, ic.sz.cy, mem, 0,0, ic.srcSz.cx, ic.srcSz.cy, bf);
			SelectObject(mem, old); DeleteDC(mem);

			// Strip sort-prefix (%NN%), suffix and extensions for display
			String disp = isRoot ? ci.name : ci.name.substr(prefix.size());
			disp = Util::StripSortPrefix(disp);
			if (ci.is_submenu)
				disp = Util::StripSubmenuSuffix(disp);
			else
				for (auto& ext : { L".lnk", L".bat", L".cmd", L".exe", L".vbs", L".url" })
					if (Util::ends_with(disp, ext)) { disp = Util::rtrim(disp, ext); break; }

			// Text rect: from after icon to before arrow (if submenu) or right margin
			int arrowH3     = L.arrowSz * 866 / 1000;
			int arrowTotalW = ci.is_submenu ? (L.arrowGap + arrowH3 + L.hPad) : L.hPad;
			RECT tr = ir;
			tr.left  = ix + ic.sz.cx + L.iconGap;
			tr.right = ir.right - arrowTotalW;

			SetBkMode(hdc_, TRANSPARENT);
			SetTextColor(hdc_, hot ? fgSelColor : fgColor);
			DrawText(hdc_, disp.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

			// Draw equilateral triangle arrow, right-aligned inside the item
			if (ci.is_submenu) {
				int side = L.arrowSz;
				int h3   = arrowH3;
				int cx   = ir.right - L.hPad - h3;
				int cy   = ir.top + L.itemH / 2;

				POINT tri[3] = {
					{ cx,      cy - side/2 },
					{ cx,      cy + side/2 },
					{ cx + h3, cy          }
				};
				HBRUSH triBrush = CreateSolidBrush(arrowColor);
				HPEN   triPen   = CreatePen(PS_NULL, 0, arrowColor);
				HGDIOBJ ob = SelectObject(hdc_, triBrush);
				HGDIOBJ op = SelectObject(hdc_, triPen);
				Polygon(hdc_, tri, 3);
				SelectObject(hdc_, ob); SelectObject(hdc_, op);
				DeleteObject(triBrush); DeleteObject(triPen);
			}

			y += L.itemH;
		}

		SelectObject(hdc_, oldFont);
		DeleteObject(hMenuFont);

		// Blit the completed frame to the real DC in one shot (eliminates flicker)
		BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
		SelectObject(memDC, oldMemBmp); DeleteObject(memBmp); DeleteDC(memDC);
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_MOUSEMOVE: {
		auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		App* app = state ? state->app : nullptr;
		if (!app) return 0;
		const String& prefix = state->prefix;
		bool isRoot = prefix.empty();

		PopupLayout L = MakeLayout(hwnd, app->IconPxFor(prefix));
		RECT rc; GetClientRect(hwnd, &rc);
		POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

		int newHot = -1;
		int newHotSubIdx  = -1;  // item index if it is a submenu row
		String newSubPrefix;

		int y = rc.top + L.vPad;

		if (isRoot && !app->hide_header && app->cache->items.size() >= 1) {
			RECT r = { rc.left, y, rc.right, y + L.itemH };
			if (PtInRect(&r, pt)) newHot = 0;
			y += L.itemH + L.sepH;
		}

		if (newHot == -1) {
			for (size_t i : app->PopupItems(prefix)) {
				if (i == App::kAutoSepIndex) { y += L.sepH; continue; }
				auto& it = app->cache->items[i];
				bool isSep = App::IsSeparatorFile(it.name);
				int h = isSep ? L.sepH : L.itemH;
				RECT r = { rc.left, y, rc.right, y + h };
				if (!isSep && PtInRect(&r, pt)) {
					newHot = (int)i;
					if (it.is_submenu) {
						newHotSubIdx = (int)i;
						newSubPrefix = it.name + DIR_SEP;
					}
					break;
				}
				y += h;
			}
		}

		if (newHot != state->hotItem) {
			state->hotItem = newHot;
			InvalidateRect(hwnd, nullptr, FALSE);
		}

		// Cancel any pending close when the cursor comes back
		if (state->closePending) {
			state->closePending = false;
			KillTimer(hwnd, POPUP_TIMER_CLOSE_CHILD);
		}

		// --- Hover submenu logic ---
		// Estimate how "deliberately" the cursor is heading toward the submenu
		// arrow versus just sweeping vertically across unrelated rows, using
		// the distance/time since the previous WM_MOUSEMOVE. A slow-vertical /
		// horizontal-leaning move is treated as intentional and opens almost
		// immediately; a fast vertical sweep keeps the full debounce so the
		// highlight doesn't visually "split" while windows are torn down.
		DWORD nowTick = GetTickCount();
		UINT openDelay = POPUP_OPEN_CHILD_DELAY_MS;
		if (state->lastMovePt.x != -1) {
			DWORD dt = nowTick - state->lastMoveTick;
			if (dt == 0) dt = 1;
			double dx = (double)abs(pt.x - state->lastMovePt.x);
			double dy = (double)abs(pt.y - state->lastMovePt.y);
			double vy = dy / (double)dt;
			if (vy <= POPUP_SUB_OPEN_VY_THRESHOLD || dx >= dy) {
				openDelay = POPUP_OPEN_CHILD_FAST_DELAY_MS;
			}
		}
		state->lastMovePt = pt;
		state->lastMoveTick = nowTick;

		if (newHotSubIdx != state->hotSubIdx) {
			// Close previous child if different row
			if (state->childHwnd && IsWindow(state->childHwnd)) {
				DestroyWindow(state->childHwnd);
				state->childHwnd = nullptr;
			}
			state->hotSubIdx = newHotSubIdx;

			// Cancel any debounce timer left over from a previously-hovered
			// submenu row; a new one (if applicable) is armed below.
			KillTimer(hwnd, POPUP_TIMER_OPEN_CHILD);
			state->pendingSubIdx = -1;

			if (newHotSubIdx != -1) {
				// Don't open the child popup immediately in the general case:
				// creating/destroying a top-level window on every row crossed
				// while sweeping the mouse quickly is what makes the highlight
				// look "split" (see PopupState::pendingSubIdx comment). Only
				// arm a short timer here; WM_TIMER actually creates the child
				// if the cursor is still on this same row once it fires. When
				// the direction heuristic above indicates a deliberate move
				// toward this row, openDelay is shortened so it feels instant.
				state->pendingSubIdx = newHotSubIdx;
				state->pendingSubPrefix = newSubPrefix;
				state->pendingSubY = y; // row top (client y), matches the value used below
				SetTimer(hwnd, POPUP_TIMER_OPEN_CHILD, openDelay, nullptr);
			}
		}

		TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
		TrackMouseEvent(&tme);
		return 0;
	}

	case WM_MOUSELEAVE: {
		auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (state) {
			if (state->hotItem != -1) {
				state->hotItem = -1;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			// Reset move tracking so re-entering the popup doesn't compute a
			// bogus velocity from a stale point captured before the cursor left.
			state->lastMovePt = { -1, -1 };
			// Don't destroy the child immediately: the cursor may be crossing the
			// 1-px gap between parent and child. Use a short grace-period timer.
			if (state->childHwnd && IsWindow(state->childHwnd)) {
				state->closePending = true;
				SetTimer(hwnd, POPUP_TIMER_CLOSE_CHILD, 150, nullptr);
			} else {
				state->hotSubIdx = -1;
			}
		}
		return 0;
	}

	case WM_TIMER: {
		if (wParam == POPUP_TIMER_OPEN_CHILD) {
			KillTimer(hwnd, POPUP_TIMER_OPEN_CHILD);
			auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
			App* app = state ? state->app : nullptr;
			// Only open if the cursor is still on the same submenu row that
			// armed this timer (hotSubIdx didn't change while waiting).
			if (app && state && state->pendingSubIdx != -1 && state->pendingSubIdx == state->hotSubIdx
				&& !(state->childHwnd && IsWindow(state->childHwnd))) {
				PopupLayout L = MakeLayout(hwnd, app->IconPxFor(state->prefix));
				int y = state->pendingSubY;
				const String& newSubPrefix = state->pendingSubPrefix;

				RECT wr; GetWindowRect(hwnd, &wr);
				POINT mid = { 0, y + L.itemH / 2 };
				ClientToScreen(hwnd, &mid);
				int sx = wr.right;
				int sy = mid.y;

				if (app->single_submenu_mode) {
					// Open a flat, non-recursive icon grid instead of a nested popup.
					auto gm = app->MeasureGridSize(newSubPrefix);
					if (gm.totalW > 0 && gm.totalH > 0) {
						HMONITOR hMon = MonitorFromPoint({ sx, sy }, MONITOR_DEFAULTTONEAREST);
						MONITORINFO mi2{ sizeof(mi2) };
						GetMonitorInfo(hMon, &mi2);
						RECT& wa = mi2.rcWork;
						int cx = sx, cy = sy - gm.totalH / 2;
						if (cx + gm.totalW > wa.right)  cx = wr.left - gm.totalW;
						if (cy + gm.totalH > wa.bottom) cy = wa.bottom - gm.totalH;
						if (cy < wa.top)                cy = wa.top;

						auto* cp = new App::GridCreateParams{ app, newSubPrefix, hwnd, 0 };
						HWND child = CreateWindowEx(
							WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
							STACKY_GRID_CLASS, L"",
							WS_POPUP | WS_BORDER,
							cx, cy, gm.totalW, gm.totalH,
							nullptr, nullptr, GetModuleHandle(nullptr), cp);

						if (child) {
							Util::SetWindowRoundedCorners(child);
							ShowWindow(child, SW_SHOWNOACTIVATE);
							UpdateWindow(child);
							state->childHwnd = child;
						} else {
							delete cp;
							state->hotSubIdx = -1;
						}
					} else {
						state->hotSubIdx = -1;
					}
				} else {
					// Open new child hover-popup without stealing focus
					auto sz = app->MeasureMenuSize(newSubPrefix);
					HMONITOR hMon = MonitorFromPoint({ sx, sy }, MONITOR_DEFAULTTONEAREST);
					MONITORINFO mi2{ sizeof(mi2) };
					GetMonitorInfo(hMon, &mi2);
					RECT& wa = mi2.rcWork;
					int cx = sx, cy = sy - sz.h / 2;
					if (cx + sz.w > wa.right)  cx = wr.left - sz.w;
					if (cy + sz.h > wa.bottom) cy = wa.bottom - sz.h;
					if (cy < wa.top)           cy = wa.top;

					auto* childState = new PopupState{ app, newSubPrefix };
					childState->parentHwnd = hwnd;

					HWND child = CreateWindowEx(
						WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
						STACKY_POPUP_CLASS, L"",
						WS_POPUP | WS_BORDER,
						cx, cy, sz.w, sz.h,
						nullptr, nullptr, GetModuleHandle(nullptr), childState);

					if (child) {
						Util::SetWindowRoundedCorners(child);
						ShowWindow(child, SW_SHOWNOACTIVATE);
						UpdateWindow(child);
						state->childHwnd = child;
					} else {
						delete childState;
						state->hotSubIdx = -1;
					}
				}
			}
			if (state) state->pendingSubIdx = -1;
		}
		if (wParam == POPUP_TIMER_CLOSE_CHILD) {
			KillTimer(hwnd, POPUP_TIMER_CLOSE_CHILD);
			auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
			if (state && state->closePending) {
				state->closePending = false;
				if (state->childHwnd && IsWindow(state->childHwnd)) {
					// Check if cursor is now inside the child window
					POINT cur; GetCursorPos(&cur);
					RECT childRc; GetWindowRect(state->childHwnd, &childRc);
					if (!PtInRect(&childRc, cur)) {
						DestroyWindow(state->childHwnd);
						state->childHwnd = nullptr;
						state->hotSubIdx  = -1;
					}
					// else: cursor entered child — keep it alive
				}
			}
		}
		return 0;
	}

	case WM_LBUTTONDOWN: {
		auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		App* app = state ? state->app : nullptr;
		if (app) {
			const String& prefix = state->prefix;
			bool isRoot = prefix.empty();
			PopupLayout L = MakeLayout(hwnd, app->IconPxFor(prefix));
			RECT rc; GetClientRect(hwnd, &rc);
			POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			int y = rc.top + L.vPad;

			// Header click (root only)
			if (isRoot && !app->hide_header && app->cache->items.size() >= 1) {
				RECT r = { rc.left, y, rc.right, y + L.itemH };
				if (PtInRect(&r, pt)) {
					ShellExecute(nullptr, nullptr, app->cache->path().c_str(), nullptr, nullptr, SW_NORMAL);
					PostMessage(hwnd, WM_CLOSE, 0, 0);
					return 0;
				}
				y += L.itemH + L.sepH;
			}

			for (size_t i : app->PopupItems(prefix)) {
				if (i == App::kAutoSepIndex) { y += L.sepH; continue; }
				auto& it = app->cache->items[i];
				bool isSep = App::IsSeparatorFile(it.name);
				int h = isSep ? L.sepH : L.itemH;
				RECT r = { rc.left, y, rc.right, y + h };
				if (PtInRect(&r, pt)) {
					if (!isSep) {
						if (it.is_submenu) {
							// Submenu already open via hover; clicking just closes parent
							if (IsWindow(hwnd)) PostMessage(hwnd, WM_CLOSE, 0, 0);
						} else {
							String cmd = app->cache->path(it.name);
							ShellExecute(nullptr, nullptr, cmd.c_str(), nullptr, nullptr, SW_NORMAL);
							Util::RegisterRecentLaunch(cmd);
							{
								String web_url = Util::GetWebUrl(cmd);
								if (!web_url.empty() && !Util::HasCustomIcon(cmd))
									Util::TriggerFaviconDownloadAsync(app->cache->base_dir, it.name, web_url, app->cache->cache_path, cmd);
							}
							PostMessage(hwnd, WM_CLOSE, 0, 0);
						}
					}
					return 0;
				}
				y += h;
			}
		}
		PostMessage(hwnd, WM_CLOSE, 0, 0);
		return 0;
	}

	case WM_RBUTTONDOWN: {
		auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		App* app = state ? state->app : nullptr;
		if (app) {
			const String& prefix = state->prefix;
			bool isRoot = prefix.empty();
			PopupLayout L = MakeLayout(hwnd, app->IconPxFor(prefix));
			RECT rc; GetClientRect(hwnd, &rc);
			POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			int y = rc.top + L.vPad;

			if (isRoot && !app->hide_header && app->cache->items.size() >= 1) {
				y += L.itemH + L.sepH;
			}

			for (size_t i : app->PopupItems(prefix)) {
				if (i == App::kAutoSepIndex) { y += L.sepH; continue; }
				auto& it = app->cache->items[i];
				bool isSep = App::IsSeparatorFile(it.name);
				int h = isSep ? L.sepH : L.itemH;
				RECT r = { rc.left, y, rc.right, y + h };
				if (PtInRect(&r, pt)) {
					if (!isSep && it.is_submenu) {
						String submenu_path = it.submenu_path.empty() ? app->cache->path(it.name) : it.submenu_path;
						app->ShowSubfolderContextMenu(hwnd, submenu_path);
					} else if (!isSep) {
						String shortcut_path = app->cache->path(it.name);
						app->ShowShortcutContextMenu(hwnd, shortcut_path, isRoot);
					}
					return 0;
				}
				y += h;
			}
		}
		return 0;
	}

	case WM_KEYDOWN:
		if (wParam == VK_ESCAPE) {
			auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
			if (state && state->childHwnd && IsWindow(state->childHwnd))
				DestroyWindow(state->childHwnd);
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		}
		return 0;

	case WM_KILLFOCUS: {
		auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		HWND newFocus = (HWND)wParam;
		// Don't close if focus moved to our hover-child, or to any deeper
		// descendant (2nd level or beyond) of it, e.g. after a right-click
		// context menu was shown from a nested submenu.
		bool focusToChild = state && state->childHwnd && IsDescendantMenuWindow(state->childHwnd, newFocus);
		// Don't close if we ARE a hover-child (parent controls our lifetime)
		bool isHoverChild = state && (state->parentHwnd != nullptr);
		if (!focusToChild && !isHoverChild)
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		return 0;
	}

	case WM_CLOSE: {
		// Destroy any open hover-child before we close
		auto* state = (PopupState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		if (state && state->childHwnd && IsWindow(state->childHwnd)) {
			DestroyWindow(state->childHwnd);
			state->childHwnd = nullptr;
		}
		DestroyWindow(hwnd);
		return 0;
	}

	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**************************************************************************************************
 * Grid state (per icon-grid window)
 **************************************************************************************************/
struct GridState {
    App*      app;
    String    prefix;       // "" for root, "Folder.submenu\\" for child
    HWND      parentHwnd;   // nullptr for root grid
    GridMode  grid_mode;
    // geometry
    int       cellSz;       // icon-area square side
    int       cellW;        // actual column step (== cellSz except SS_NAME_RIGHT/SS_NAME_BELOW)
    int       cellPad;      // side padding used by SS_NAME_BELOW (24px) for name-extend math
    int       cellH;        // cellSz + labelH
    int       labelH;
    int       cols;
    int       rows;
    int       f2Row1Count;  // GRID_F2 only: number of items in row 1 (submenu items).
                            // Also reused for GRID_CASCADE/GRID_CASCADE_NAME submenus in
                            // subTwoCol mode: number of items in the submenu column.
    int       topPad;       // GRID_F2 only: space reserved above row 0 for upward triangle
    bool      subTwoCol;    // GRID_CASCADE/GRID_CASCADE_NAME submenus only: 2-column layout
    int       iconPx;       // logical icon size (20 for mini, 32 normal), before DPI scaling
    // hover
    int       hotCell;
    bool      trackingMouse;
    bool      dark_mode;
    // tooltip
    HWND      tipHwnd;
    // submenu child (GRID_CASCADE)
    HWND      subChild;
    int       hotSubCell;   // cell index that opened subChild (-1 = none)
    bool      subClosePending;
    int       openRight;    // +1=open submenus to right, -1=left (inherited from root column)
};

struct TipData {
    wchar_t label[512];
    bool    dark;
};
static TipData g_tipData;

// Returns true if `candidate` is `ancestor` itself, or is reachable from
// `ancestor` by following the chain of hover-opened children (childHwnd for
// popups, subChild for grids), at ANY nesting depth. Used by WM_KILLFOCUS so
// that a popup/grid does not incorrectly close itself when focus moves to a
// deeply nested descendant (2nd level or deeper) instead of its direct child
// (e.g. right-click context menus opened from nested submenus, which call
// SetForegroundWindow on that nested window rather than the direct child).
bool IsDescendantMenuWindow(HWND ancestor, HWND candidate) {
    if (!ancestor || !candidate) return false;
    if (ancestor == candidate) return true;

    HWND cur = ancestor;
    while (cur && IsWindow(cur)) {
        wchar_t cls[64] = { 0 };
        GetClassName(cur, cls, _countof(cls));
        HWND next = nullptr;
        if (wcscmp(cls, STACKY_POPUP_CLASS) == 0) {
            auto* s = (PopupState*)GetWindowLongPtr(cur, GWLP_USERDATA);
            if (s) next = s->childHwnd;
        } else if (wcscmp(cls, STACKY_GRID_CLASS) == 0) {
            auto* s = (GridState*)GetWindowLongPtr(cur, GWLP_USERDATA);
            if (s) next = s->subChild;
        }
        if (!next || !IsWindow(next)) return false;
        if (next == candidate) return true;
        cur = next;
    }
    return false;
}

// Walk up the popup/grid parent chain from `owner` (which may itself be a
// hover-opened child popup or nested submenu grid) to find the root menu
// window, then restore keyboard focus and foreground status to it. This is
// needed after closing an app-owned right-click context menu (TrackPopupMenuEx),
// which otherwise leaves focus nowhere useful, so WM_KILLFOCUS never fires
// again and the whole menu/submenu chain stays stuck open.
void RestoreFocusToRootMenuWindow(HWND owner) {
    if (!owner || !IsWindow(owner)) return;

    HWND root = owner;
    while (true) {
        wchar_t cls[64] = { 0 };
        GetClassName(root, cls, _countof(cls));
        HWND parent = nullptr;
        if (wcscmp(cls, STACKY_POPUP_CLASS) == 0) {
            auto* s = (PopupState*)GetWindowLongPtr(root, GWLP_USERDATA);
            if (s) parent = s->parentHwnd;
        } else if (wcscmp(cls, STACKY_GRID_CLASS) == 0) {
            auto* s = (GridState*)GetWindowLongPtr(root, GWLP_USERDATA);
            if (s) parent = s->parentHwnd;
        }
        if (!parent || !IsWindow(parent)) break;
        root = parent;
    }

    if (IsWindow(root)) {
        SetForegroundWindow(root);
        SetFocus(root);
    }
}

// Timer IDs for the grid window
static const UINT_PTR GRID_TIMER_SHOW      = 1;  // delay before showing tooltip
static const UINT_PTR GRID_TIMER_HIDE      = 2;  // auto-hide tooltip
static const UINT_PTR GRID_TIMER_SUBCL     = 3;  // grace period before closing sub-grid

	// Helper: build display name from a cache item name + prefix.
	// Strips .submenu / .submenu-mini and layout tokens so the shown label
	// is the clean folder/file name.
static String GridDisplayName(const String& name, const String& prefix) {
	String s = name.substr(prefix.size());
	s = Util::StripSortPrefix(s);
	for (auto& ext : { L".lnk", L".bat", L".cmd", L".exe", L".vbs", L".url" })
		if (Util::ends_with(s, ext)) { s = Util::rtrim(s, ext); break; }
	s = Util::StripSubmenuSuffix(s);
	return s;
}

// Helper: compute (col,row) for a given item index according to grid layout.
// For GRID_F2, row 1 (row==0) holds the items with a submenu (the first f2Row1Count
// items, since GridItems() already reorders them to the front); row 2 (row==1) holds
// the rest. For GRID_CASCADE/GRID_CASCADE_NAME submenus in subTwoCol mode, the first
// f2Row1Count items (all submenu items, plus enough plain items to pad the column,
// since GridItems() groups them first) go into a single column placed on the side the
// submenu opens towards (openRight); the remaining plain items fill the other
// column(s) in column-major order. Other modes use plain column-major wrapping.
static void GridCellPos(GridState* gs, int idx, int& col, int& row) {
    if (gs->grid_mode == GRID_F2 || gs->grid_mode == GRID_F2_NAME) {
        if (idx < gs->f2Row1Count) { row = 0; col = idx; }
        else                       { row = 1; col = idx - gs->f2Row1Count; }
    } else if (gs->subTwoCol) {
        // openRight > 0 -> submenu opens to the right -> submenu column is the
        // rightmost column; openRight <= 0 -> submenu column is the leftmost column.
        int subCol = (gs->openRight > 0) ? (gs->cols - 1) : 0;
        if (idx < gs->f2Row1Count) {
            col = subCol;
            row = idx;
        } else {
            int rem = idx - gs->f2Row1Count;
            int colInRem = rem / gs->f2Row1Count;
            row = rem % gs->f2Row1Count;
            // Fill the remaining columns, skipping over the submenu column.
            col = (subCol == 0) ? (colInRem + 1) : colInRem;
        }
    } else {
        col = idx % gs->cols;
        row = idx / gs->cols;
    }
}

// Helper: show or hide a tooltip for the grid
static void GridShowTip(HWND hwnd, GridState* gs, int cellIdx) {
    if (gs->tipHwnd && IsWindow(gs->tipHwnd)) {
        DestroyWindow(gs->tipHwnd);
        gs->tipHwnd = nullptr;
    }
    if (cellIdx < 0) return;

    auto items = gs->app->GridItems(gs->prefix);
    if (cellIdx >= (int)items.size()) return;

    auto& ci = gs->app->cache->items[items[cellIdx]];
    String label = GridDisplayName(ci.name, gs->prefix);

    HDC measDC = GetDC(hwnd);
    LOGFONT lf{}; lf.lfHeight = -12; lf.lfWeight = FW_NORMAL;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT fnt = CreateFontIndirect(&lf);
    HFONT oldF = (HFONT)SelectObject(measDC, fnt);
    SIZE ts{}; GetTextExtentPoint32(measDC, label.c_str(), (int)label.size(), &ts);
    SelectObject(measDC, oldF); DeleteObject(fnt); ReleaseDC(hwnd, measDC);

    int tipW = ts.cx + 10, tipH = ts.cy + 6;
    int col, row;
    GridCellPos(gs, cellIdx, col, row);
    POINT origin = {0,0}; ClientToScreen(hwnd, &origin);
    int tipX = origin.x + col * gs->cellW + gs->cellW/2 - tipW/2;
    int tipY = origin.y + gs->topPad + row * gs->cellH - tipH - 2;
    // if above screen, place below icon
    if (tipY < 0) tipY = origin.y + gs->topPad + row * gs->cellH + gs->cellSz + 2;

    wcsncpy_s(g_tipData.label, label.c_str(), 511);
    g_tipData.dark = gs->dark_mode;

    gs->tipHwnd = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        STACKY_TIP_CLASS, L"", WS_POPUP,
        tipX, tipY, tipW, tipH,
        hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
    if (gs->tipHwnd) {
        SetWindowLongPtr(gs->tipHwnd, GWLP_USERDATA, (LONG_PTR)&g_tipData);
        ShowWindow(gs->tipHwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(gs->tipHwnd);
        SetTimer(hwnd, GRID_TIMER_HIDE, 4000, nullptr);
    }
}

// Helper: open a sub-grid for a submenu item.
// GRID_CASCADE / GRID_CASCADE_NAME: single-column sub-grid opening left/right.
// GRID_F2: row-based sub-grid (same layout rules as the main grid) opening upward.
static void GridOpenSubChild(HWND hwnd, GridState* gs, int cellIdx) {
    auto items = gs->app->GridItems(gs->prefix);
    if (cellIdx < 0 || cellIdx >= (int)items.size()) return;
    auto& ci = gs->app->cache->items[items[cellIdx]];
    if (!ci.is_submenu) return;

    String childPrefix = ci.name + DIR_SEP;

    if (gs->grid_mode == GRID_F2 || gs->grid_mode == GRID_F2_NAME) {
        auto gm = gs->app->MeasureGridSize(childPrefix);
        if (gm.totalW <= 0 || gm.totalH <= 0) return;
        int subN = (int)gs->app->GridItems(childPrefix).size();
        if (subN == 0) return;

        int col, row;
        GridCellPos(gs, cellIdx, col, row);
        RECT wr; GetWindowRect(hwnd, &wr);
        POINT origin = {0, 0}; ClientToScreen(hwnd, &origin);
        int cx = origin.x + col * gs->cellSz;
        int cy = origin.y + gs->topPad + row * gs->cellH; // top of the cell that owns the submenu

        HMONITOR hMon = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfo(hMon, &mi);

        // Always open upward: bottom of sub-grid touches top of the cell.
        int subY = cy - gm.totalH;
        if (subY < mi.rcWork.top) subY = mi.rcWork.top;

        // Clamp horizontally within work area.
        int subX = cx;
        if (subX + gm.totalW > mi.rcWork.right) subX = mi.rcWork.right - gm.totalW;
        if (subX < mi.rcWork.left) subX = mi.rcWork.left;

        auto* cp = new App::GridCreateParams{ gs->app, childPrefix, hwnd, gs->openRight };
        HWND child = CreateWindowEx(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            STACKY_GRID_CLASS, L"", WS_POPUP | WS_BORDER,
            subX, subY, gm.totalW, gm.totalH,
            nullptr, nullptr, GetModuleHandle(nullptr), cp);
        if (!child) { delete cp; return; }
        Util::SetWindowRoundedCorners(child);
        ShowWindow(child, SW_SHOWNOACTIVATE);
        UpdateWindow(child);
        gs->subChild   = child;
        gs->hotSubCell = cellIdx;
        return;
    }

    UINT dpi = GetDpiForWindow(hwnd);

    // Determine preferred open direction:
    // - Root grid (no parent): column of cellIdx in the 2-col root decides direction.
    // - Sub-grid: inherit the direction already established by the root column.
    int openRight;
    if (gs->parentHwnd == nullptr) {
        // Root grid: col 0 (left column) -> open submenu to the left; col 1 (right) -> right.
        int col = cellIdx % gs->cols;
        openRight = (col == 0) ? -1 : +1;
    } else {
        // Inherited direction from the chain started at the root.
        openRight = gs->openRight;
    }

    // Measure the submenu (handles the plain 1-column layout as well as the
    // 2-column layout used when plain items outnumber submenu items).
    auto gm = gs->app->MeasureGridSize(childPrefix);
    int subN = (int)gs->app->GridItems(childPrefix).size();
    if (subN == 0 || gm.totalW <= 0 || gm.totalH <= 0) return;
    int subW = gm.totalW;
    int subH = gm.totalH;

    int col, row;
    GridCellPos(gs, cellIdx, col, row);
    RECT wr; GetWindowRect(hwnd, &wr);
    POINT origin = {0, 0}; ClientToScreen(hwnd, &origin);
    int rowTop    = origin.y + gs->topPad + row * gs->cellH;       // top of the row that owns the submenu item
    int rowBottom = rowTop + gs->cellH;                            // bottom of that same row
    int cy = rowTop; // default: child's top row touches the top of the parent's row

    // Get work area to check available space
    HMONITOR hMon = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)}; GetMonitorInfo(hMon, &mi);

    int cx;
    if (openRight > 0) {
        // Prefer right; fall back to left if no room
        if (wr.right + subW <= mi.rcWork.right)
            cx = wr.right;
        else
            cx = wr.left - subW;
    } else {
        // Prefer left; fall back to right if no room
        if (wr.left - subW >= mi.rcWork.left)
            cx = wr.left - subW;
        else
            cx = wr.right;
    }

    // If top-aligning the child to the parent row overflows the bottom of the
    // work area (typically because the row is the last one in the parent),
    // anchor to the bottom of the parent's row instead: the child's bottom row
    // then touches the bottom edge of the parent's row, keeping contact
    // between the two instead of opening a row higher with a gap.
    if (cy + subH > mi.rcWork.bottom) {
        cy = rowBottom - subH;
    }
    // Final safety clamp in case neither anchor fits the work area at all
    // (e.g. submenu taller than the available work area).
    if (cy + subH > mi.rcWork.bottom) cy = mi.rcWork.bottom - subH;
    if (cy < mi.rcWork.top)           cy = mi.rcWork.top;

    auto* cp = new App::GridCreateParams{ gs->app, childPrefix, hwnd, openRight };
    HWND child = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        STACKY_GRID_CLASS, L"", WS_POPUP | WS_BORDER,
        cx, cy, subW, subH,
        nullptr, nullptr, GetModuleHandle(nullptr), cp);
    if (!child) { delete cp; return; }
    Util::SetWindowRoundedCorners(child);
    ShowWindow(child, SW_SHOWNOACTIVATE);
    UpdateWindow(child);
    gs->subChild   = child;
    gs->hotSubCell = cellIdx;
}

// ---------------------------------------------------------------------------
// Tooltip window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK TipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        auto* td = (TipData*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        bool  dm = td && td->dark;
        COLORREF tipBg     = dm ? RGB(50,50,50)    : RGB(255,255,225);
        COLORREF tipBorder = dm ? RGB(110,110,110) : RGB(0,0,0);
        COLORREF tipFg     = dm ? RGB(220,220,220) : RGB(0,0,0);

        // double-buffer
        HDC memDC = CreateCompatibleDC(dc);
        HBITMAP memBmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        HBRUSH br = CreateSolidBrush(tipBg);
        FillRect(memDC, &rc, br); DeleteObject(br);
        HPEN borderPen = CreatePen(PS_SOLID, 1, tipBorder);
        HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
        HBRUSH nullBr = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH oldBr = (HBRUSH)SelectObject(memDC, nullBr);
        Rectangle(memDC, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(memDC, oldBr); SelectObject(memDC, oldPen); DeleteObject(borderPen);

        if (td && td->label[0]) {
            LOGFONT lf{}; lf.lfHeight = -12; lf.lfWeight = FW_NORMAL;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            HFONT fnt = CreateFontIndirect(&lf);
            HFONT old = (HFONT)SelectObject(memDC, fnt);
            SetTextColor(memDC, tipFg); SetBkColor(memDC, tipBg);
            SetBkMode(memDC, TRANSPARENT);
            RECT tr = { rc.left+4, rc.top+2, rc.right-4, rc.bottom-2 };
            DrawText(memDC, td->label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(memDC, old); DeleteObject(fnt);
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp); DeleteObject(memBmp); DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Grid window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK GridWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCT*)lParam;
        auto* cp = (App::GridCreateParams*)cs->lpCreateParams;
        auto  gm  = cp->app->MeasureGridSize(cp->prefix);
        GridMode effMode = cp->app->EffectiveGridMode(cp->prefix);
        // For GRID_CASCADE/GRID_CASCADE_NAME sub-grids use gm.cols whenever it computed
        // a multi-column layout (subTwoCol, or an all-plain-items submenu now laid out
        // as 2 columns); otherwise force 1 column. GRID_F2 and GRID_SS_* sub-grids always use gm.cols.
        int cols = (cp->prefix.empty() || effMode == GRID_F2 || effMode == GRID_F2_NAME ||
                    effMode == GRID_SS_ICON || effMode == GRID_SS_NAME_RIGHT || effMode == GRID_SS_NAME_BELOW ||
                    effMode == GRID_NAME_RIGHT ||
                    gm.cols > 1) ? gm.cols : 1;
        auto* gs  = new GridState{};
        gs->app           = cp->app;
        gs->prefix        = cp->prefix;
        gs->parentHwnd    = cp->parentHwnd;
        gs->grid_mode     = effMode;
        gs->cellSz        = gm.cellSz;
        gs->cellW         = gm.cellW;
        gs->cellPad       = gm.cellPad;
        gs->cellH         = gm.cellH;
        gs->labelH        = gm.labelH;
        gs->cols          = cols;
        gs->rows          = gm.rows;
        gs->f2Row1Count   = gm.f2Row1Count;
        gs->topPad        = gm.topPad;
        gs->subTwoCol     = gm.subTwoCol;
        gs->iconPx        = cp->app->IconPxFor(cp->prefix);
        gs->hotCell       = -1;
        gs->tipHwnd       = nullptr;
        gs->trackingMouse = false;
        gs->dark_mode     = cp->app->dark_mode;
        gs->subChild      = nullptr;
        gs->hotSubCell    = -1;
        gs->subClosePending = false;
        gs->openRight     = cp->openRight;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)gs);
        delete cp;
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED: {
        // System color mode/accent changed: refresh the cache and repaint if
        // this grid is following the system theme (event-driven, no polling).
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (gs && gs->app && gs->app->theme_mode == THEME_SYSTEM) {
            RefreshSystemThemeCache();
            gs->app->dark_mode = GetSystemTheme().dark;
            gs->dark_mode      = gs->app->dark_mode;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    }
    case WM_DESTROY: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (gs) {
            KillTimer(hwnd, GRID_TIMER_SHOW);
            KillTimer(hwnd, GRID_TIMER_HIDE);
            KillTimer(hwnd, GRID_TIMER_SUBCL);
            if (gs->tipHwnd && IsWindow(gs->tipHwnd)) DestroyWindow(gs->tipHwnd);
            if (gs->subChild && IsWindow(gs->subChild)) DestroyWindow(gs->subChild);
            // notify parent (may be a grid window or, in --singlesubmenu mode, a popup window)
            if (gs->parentHwnd && IsWindow(gs->parentHwnd)) {
                wchar_t parentClass[64] = { 0 };
                GetClassName(gs->parentHwnd, parentClass, _countof(parentClass));
                if (wcscmp(parentClass, STACKY_GRID_CLASS) == 0) {
                    auto* ps = (GridState*)GetWindowLongPtr(gs->parentHwnd, GWLP_USERDATA);
                    if (ps && ps->subChild == hwnd) {
                        ps->subChild   = nullptr;
                        ps->hotSubCell = -1;
                    }
                } else if (wcscmp(parentClass, STACKY_POPUP_CLASS) == 0) {
                    auto* ps = (PopupState*)GetWindowLongPtr(gs->parentHwnd, GWLP_USERDATA);
                    if (ps && ps->childHwnd == hwnd) {
                        ps->childHwnd = nullptr;
                        ps->hotSubIdx = -1;
                    }
                }
            }
            HWND parent = gs->parentHwnd;
            delete gs;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            // Root icon-grid only. A --singlesubmenu hover grid is a child of
            // the list popup; quitting here would tear down the whole menu.
            if (!parent) PostQuitMessage(0);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!gs) return DefWindowProc(hwnd, msg, wParam, lParam);
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        // double-buffer
        HDC memDC = CreateCompatibleDC(dc);
        HBITMAP memBmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        bool dm = gs->dark_mode;
        COLORREF gridBg  = gs->app ? gs->app->BackgroundColor() : (dm ? RGB(32,32,32) : RGB(240,240,240));
        COLORREF gridHov = gs->app ? gs->app->SelectionColor() : (dm ? RGB(64,64,64) : RGB(204,228,247));
        COLORREF labelFg = dm ? RGB(220,220,220): RGB(30,30,30);
        COLORREF labelFgSel = gs->app ? gs->app->SelectionTextColor() : (dm ? RGB(255,255,255) : GetSysColor(COLOR_HIGHLIGHTTEXT));
        HBRUSH bgBr = CreateSolidBrush(gridBg);
        FillRect(memDC, &rc, bgBr); DeleteObject(bgBr);

        auto items = gs->app->GridItems(gs->prefix);
        int  sz    = gs->cellSz;
        int  cW    = gs->cellW;
        int  cH    = gs->cellH;
        UINT dpi   = GetDpiForWindow(hwnd);
        int  iSz   = MulDiv(gs->iconPx, dpi, 96);
        int  iPad  = MulDiv(8,  dpi, 96);

        // Prepare label font (used for GRID_NAME and also submenu arrow)
        HFONT labelFnt = nullptr;
        if (gs->grid_mode == GRID_NAME || gs->grid_mode == GRID_CASCADE || gs->grid_mode == GRID_CASCADE_NAME || gs->grid_mode == GRID_F2_NAME) {
            LOGFONT lf{}; lf.lfHeight = -MulDiv(9, dpi, 96); lf.lfWeight = FW_NORMAL;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            labelFnt = CreateFontIndirect(&lf);
        }
        // Main-menu font, used for GRID_SS_NAME_RIGHT / GRID_SS_NAME_BELOW / GRID_NAME_RIGHT item names
        // (per spec: same font/size as the main menu).
        HFONT mainMenuFnt = nullptr;
        if (gs->grid_mode == GRID_SS_NAME_RIGHT || gs->grid_mode == GRID_SS_NAME_BELOW || gs->grid_mode == GRID_NAME_RIGHT) {
            NONCLIENTMETRICS ncm{}; ncm.cbSize = sizeof(ncm);
            SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            ncm.lfMenuFont.lfWeight = FW_NORMAL;
            mainMenuFnt = CreateFontIndirect(&ncm.lfMenuFont);
        }

        for (int i = 0; i < (int)items.size(); ++i) {
            int col, row;
            GridCellPos(gs, i, col, row);
            int x = col * cW;
            int y = gs->topPad + row * cH;
            RECT cell = { x, y, x + cW, y + cH };

            // hover highlight (whole cell)
            if (i == gs->hotCell) {
                HBRUSH hlBr = CreateSolidBrush(gridHov);
                FillRect(memDC, &cell, hlBr); DeleteObject(hlBr);
            }

            // draw icon
            auto& ci = gs->app->cache->items[items[i]];
            auto& ic = gs->app->icon_cache.get(hwnd, ci.bmp.hBmp, gs->iconPx);
            HDC tmpDC = CreateCompatibleDC(memDC);
            HGDIOBJ oldTmp = SelectObject(tmpDC, ic.bmp);
            BLENDFUNCTION bf{}; bf.BlendOp = AC_SRC_OVER; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
            int drawSz = iSz;
            int iconX, iconY;
            if (gs->grid_mode == GRID_SS_NAME_RIGHT || gs->grid_mode == GRID_NAME_RIGHT) {
                iconX = x + iPad + (iSz - drawSz) / 2;
                iconY = y + (cH - iSz) / 2;
            } else if (gs->grid_mode == GRID_SS_NAME_BELOW) {
                iconX = x + (cW - iSz) / 2;
                iconY = y + (sz - iSz) + (iSz - drawSz) / 2;
            } else {
                iconX = x + iPad + (iSz - drawSz) / 2;
                iconY = y + iPad + (iSz - drawSz) / 2;
            }
            AlphaBlend(memDC, iconX, iconY, drawSz, drawSz, tmpDC, 0, 0, ic.srcSz.cx, ic.srcSz.cy, bf);
            SelectObject(tmpDC, oldTmp); DeleteDC(tmpDC);

            // draw label below icon (GRID_NAME / GRID_CASCADE_NAME / GRID_F2_NAME modes)
            if ((gs->grid_mode == GRID_NAME || gs->grid_mode == GRID_CASCADE_NAME || gs->grid_mode == GRID_F2_NAME) && gs->labelH > 0 && labelFnt) {
                String disp = GridDisplayName(ci.name, gs->prefix);
                HFONT oldF = (HFONT)SelectObject(memDC, labelFnt);
                SetTextColor(memDC, (i == gs->hotCell) ? labelFgSel : labelFg);
                SetBkMode(memDC, TRANSPARENT);
                // Measure width of 'a' as lateral margin
                SIZE aSz{}; GetTextExtentPoint32(memDC, L"a", 1, &aSz);
                int margin = aSz.cx;
                // Start the label rect right at the icon's bottom edge (removing the
                // cell's own bottom padding, so there is no empty gap above the name).
                RECT lr = { x + margin, y + sz - iPad, x + sz - margin, y + cH };
                // Truncate text to fit without ellipsis: clip word by word then char by char
                int availW = lr.right - lr.left;
                String truncated = disp;
                SIZE ts2{};
                GetTextExtentPoint32(memDC, truncated.c_str(), (int)truncated.size(), &ts2);
                while (!truncated.empty() && ts2.cx > availW) {
                    truncated.pop_back();
                    GetTextExtentPoint32(memDC, truncated.c_str(), (int)truncated.size(), &ts2);
                }
                DrawText(memDC, truncated.c_str(), -1, &lr,
                    DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                SelectObject(memDC, oldF);
            }

            // draw name to the right of the icon (GRID_SS_NAME_RIGHT / GRID_NAME_RIGHT variants)
            if ((gs->grid_mode == GRID_SS_NAME_RIGHT || gs->grid_mode == GRID_NAME_RIGHT) && mainMenuFnt) {
                String disp = GridDisplayName(ci.name, gs->prefix);
                HFONT oldF = (HFONT)SelectObject(memDC, mainMenuFnt);
                SetTextColor(memDC, (i == gs->hotCell) ? labelFgSel : labelFg);
                SetBkMode(memDC, TRANSPARENT);
                int textGap = 0;
                RECT lr = { x + sz + textGap, y, x + cW, y + cH };
                int availW = lr.right - lr.left;
                String truncated = disp;
                SIZE ts2{};
                GetTextExtentPoint32(memDC, truncated.c_str(), (int)truncated.size(), &ts2);
                while (!truncated.empty() && ts2.cx > availW) {
                    truncated.pop_back();
                    GetTextExtentPoint32(memDC, truncated.c_str(), (int)truncated.size(), &ts2);
                }
                DrawText(memDC, truncated.c_str(), -1, &lr,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                SelectObject(memDC, oldF);
            }

            // draw name below the icon (--singlesubmenu GRID_SS_NAME_BELOW variant):
            // single centered line, allowed to extend up to 16px into the empty
            // space between icons on either side, truncated (no ellipsis) if still too long.
            if (gs->grid_mode == GRID_SS_NAME_BELOW && gs->labelH > 0 && mainMenuFnt) {
                String disp = GridDisplayName(ci.name, gs->prefix);
                HFONT oldF = (HFONT)SelectObject(memDC, mainMenuFnt);
                SetTextColor(memDC, (i == gs->hotCell) ? labelFgSel : labelFg);
                SetBkMode(memDC, TRANSPARENT);
                int extend = MulDiv(16, dpi, 96);
                RECT lr = { x + gs->cellPad - extend, y + sz, x + cW - gs->cellPad + extend, y + cH };
                if (lr.left < x) lr.left = x;
                if (lr.right > x + cW) lr.right = x + cW;
                int availW = lr.right - lr.left;
                String truncated = disp;
                SIZE ts2{};
                GetTextExtentPoint32(memDC, truncated.c_str(), (int)truncated.size(), &ts2);
                while (!truncated.empty() && ts2.cx > availW) {
                    truncated.pop_back();
                    GetTextExtentPoint32(memDC, truncated.c_str(), (int)truncated.size(), &ts2);
                }
                RECT dr = lr;
                DrawText(memDC, truncated.c_str(), -1, &dr, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
                SelectObject(memDC, oldF);
            }


            // draw small submenu indicator for GRID_CASCADE / GRID_CASCADE_NAME submenu items
            if ((gs->grid_mode == GRID_CASCADE || gs->grid_mode == GRID_CASCADE_NAME) && ci.is_submenu) {
                int ts      = MulDiv(6, dpi, 96);  // triangle size
                int margin  = iPad / 2;
                int gap2mm  = MulDiv(2 * 96, dpi, 96 * 25);  // 2mm in pixels at current DPI
                int ty_mid  = y + sz / 2;      // vertically centred in icon area
                COLORREF arrowClr = dm ? RGB(180,180,180) : RGB(80,80,80);
                POINT tri[3];
                // For root grid (openRight==0): use column. For sub-grids: use inherited direction.
                bool arrowLeft = (gs->openRight != 0) ? (gs->openRight < 0) : (col == 0);
                if (arrowLeft) {
                    // left-pointing triangle, gap2mm to the left of icon left edge
                    int tx = x + margin - gap2mm - ts;
                    if (tx < x) tx = x;  // safety clamp
                    tri[0] = { tx + ts, ty_mid - ts/2 };
                    tri[1] = { tx + ts, ty_mid + ts/2 };
                    tri[2] = { tx,      ty_mid         };
                } else {
                    // right-pointing triangle, gap2mm to the right of icon right edge
                    int tx = x + sz - margin + gap2mm;
                    if (tx + ts > x + sz) tx = x + sz - ts;  // safety clamp
                    tri[0] = { tx,      ty_mid - ts/2 };
                    tri[1] = { tx,      ty_mid + ts/2 };
                    tri[2] = { tx + ts, ty_mid         };
                }
                HBRUSH arBr = CreateSolidBrush(arrowClr);
                HPEN   arPn = CreatePen(PS_NULL, 0, arrowClr);
                HGDIOBJ ob = SelectObject(memDC, arBr);
                HGDIOBJ op = SelectObject(memDC, arPn);
                Polygon(memDC, tri, 3);
                SelectObject(memDC, ob); SelectObject(memDC, op);
                DeleteObject(arBr); DeleteObject(arPn);
            }

            // draw upward-pointing submenu indicator for GRID_F2 submenu items, placed
            // in the top padding strip of the cell (inside the active/click area) so it
            // never overlaps the icon itself. Submenus in GRID_F2 always open upward.
            if ((gs->grid_mode == GRID_F2 || gs->grid_mode == GRID_F2_NAME) && ci.is_submenu) {
                int ts      = min(MulDiv(6, dpi, 96), iPad - MulDiv(1, dpi, 96));
                if (ts < MulDiv(3, dpi, 96)) ts = MulDiv(3, dpi, 96); // keep a minimum visible size
                int tx_mid  = x + sz / 2;  // horizontally centred over the icon
                COLORREF arrowClr = dm ? RGB(180,180,180) : RGB(80,80,80);
                // Centre the triangle within the top padding strip (y .. y+iPad), which sits
                // above the icon (icon starts at y+iPad) - no overlap with the icon.
                int ty = y + (iPad - ts) / 2;
                if (ty < y) ty = y;
                POINT tri[3] = {
                    { tx_mid - ts/2, ty + ts },
                    { tx_mid + ts/2, ty + ts },
                    { tx_mid,        ty      },
                };
                HBRUSH arBr = CreateSolidBrush(arrowClr);
                HPEN   arPn = CreatePen(PS_NULL, 0, arrowClr);
                HGDIOBJ ob = SelectObject(memDC, arBr);
                HGDIOBJ op = SelectObject(memDC, arPn);
                Polygon(memDC, tri, 3);
                SelectObject(memDC, ob); SelectObject(memDC, op);
                DeleteObject(arBr); DeleteObject(arPn);
            }
        }
        if (labelFnt) DeleteObject(labelFnt);
        if (mainMenuFnt) DeleteObject(mainMenuFnt);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp); DeleteObject(memBmp); DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!gs) break;

        if (!gs->trackingMouse) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, HOVER_DEFAULT };
            TrackMouseEvent(&tme);
            gs->trackingMouse = true;
        }

        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        auto items = gs->app->GridItems(gs->prefix);
        int newHot = -1;
        for (int i = 0; i < (int)items.size(); ++i) {
            int col, row;
            GridCellPos(gs, i, col, row);
            RECT cell = { col*gs->cellW, gs->topPad + row*gs->cellH,
                          col*gs->cellW + gs->cellW, gs->topPad + row*gs->cellH + gs->cellH };
            if (PtInRect(&cell, pt)) { newHot = i; break; }
        }

        if (newHot != gs->hotCell) {
            gs->hotCell = newHot;
            InvalidateRect(hwnd, nullptr, FALSE);

            KillTimer(hwnd, GRID_TIMER_SHOW);
            KillTimer(hwnd, GRID_TIMER_HIDE);
            if (gs->tipHwnd && IsWindow(gs->tipHwnd)) {
                DestroyWindow(gs->tipHwnd); gs->tipHwnd = nullptr;
            }

            if (newHot >= 0) {
                bool isSubmenu = (gs->grid_mode == GRID_CASCADE || gs->grid_mode == GRID_CASCADE_NAME || gs->grid_mode == GRID_F2 || gs->grid_mode == GRID_F2_NAME) &&
                    newHot < (int)items.size() &&
                    gs->app->cache->items[items[newHot]].is_submenu;

                // 500 ms delay before showing tooltip for all modes
                SetTimer(hwnd, GRID_TIMER_SHOW, 1000, nullptr);

                // GRID_CASCADE / GRID_CASCADE_NAME / GRID_F2 / GRID_F2_NAME: manage sub-grid on hover
                if (gs->grid_mode == GRID_CASCADE || gs->grid_mode == GRID_CASCADE_NAME || gs->grid_mode == GRID_F2 || gs->grid_mode == GRID_F2_NAME) {
                    // Cancel any pending close
                    if (gs->subClosePending) {
                        gs->subClosePending = false;
                        KillTimer(hwnd, GRID_TIMER_SUBCL);
                    }
                    if (isSubmenu && newHot != gs->hotSubCell) {
                        // Close current sub if different
                        if (gs->subChild && IsWindow(gs->subChild)) {
                            DestroyWindow(gs->subChild);
                            gs->subChild = nullptr;
                        }
                        gs->hotSubCell = -1;
                        GridOpenSubChild(hwnd, gs, newHot);
                    } else if (!isSubmenu) {
                        // Close sub-grid if moving to non-submenu
                        if (gs->subChild && IsWindow(gs->subChild)) {
                            gs->subClosePending = true;
                            SetTimer(hwnd, GRID_TIMER_SUBCL, 150, nullptr);
                        }
                    }
                }
            } else {
                // No cell
                if ((gs->grid_mode == GRID_CASCADE || gs->grid_mode == GRID_CASCADE_NAME) && gs->subChild && IsWindow(gs->subChild)) {
                    gs->subClosePending = true;
                    SetTimer(hwnd, GRID_TIMER_SUBCL, 150, nullptr);
                }
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (gs) {
            gs->trackingMouse = false;
            gs->hotCell = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            KillTimer(hwnd, GRID_TIMER_SHOW);
            KillTimer(hwnd, GRID_TIMER_HIDE);
            if (gs->tipHwnd && IsWindow(gs->tipHwnd)) {
                DestroyWindow(gs->tipHwnd); gs->tipHwnd = nullptr;
            }
            if ((gs->grid_mode == GRID_CASCADE || gs->grid_mode == GRID_CASCADE_NAME || gs->grid_mode == GRID_F2 || gs->grid_mode == GRID_F2_NAME) && gs->subChild && IsWindow(gs->subChild)) {
                // Grace period - cursor may be entering sub-grid
                gs->subClosePending = true;
                SetTimer(hwnd, GRID_TIMER_SUBCL, 150, nullptr);
            }
        }
        return 0;
    }
    case WM_TIMER: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!gs) break;

        if (wParam == GRID_TIMER_SHOW) {
            KillTimer(hwnd, GRID_TIMER_SHOW);
            GridShowTip(hwnd, gs, gs->hotCell);
            return 0;
        }
        if (wParam == GRID_TIMER_HIDE) {
            KillTimer(hwnd, GRID_TIMER_HIDE);
            if (gs->tipHwnd && IsWindow(gs->tipHwnd)) {
                DestroyWindow(gs->tipHwnd); gs->tipHwnd = nullptr;
            }
            return 0;
        }
        if (wParam == GRID_TIMER_SUBCL) {
            KillTimer(hwnd, GRID_TIMER_SUBCL);
            if (gs->subClosePending) {
                gs->subClosePending = false;
                if (gs->subChild && IsWindow(gs->subChild)) {
                    // Check if cursor is inside the sub-grid
                    POINT cur; GetCursorPos(&cur);
                    RECT sr; GetWindowRect(gs->subChild, &sr);
                    if (!PtInRect(&sr, cur)) {
                        DestroyWindow(gs->subChild);
                        gs->subChild   = nullptr;
                        gs->hotSubCell = -1;
                    }
                }
            }
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!gs) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        auto items = gs->app->GridItems(gs->prefix);
        for (int i = 0; i < (int)items.size(); ++i) {
            int col, row;
            GridCellPos(gs, i, col, row);
            RECT cell = { col*gs->cellW, gs->topPad + row*gs->cellH,
                          col*gs->cellW + gs->cellW, gs->topPad + row*gs->cellH + gs->cellH };
            if (PtInRect(&cell, pt)) {
                auto& ci = gs->app->cache->items[items[i]];
                if (ci.is_submenu && (gs->grid_mode == GRID_CASCADE || gs->grid_mode == GRID_CASCADE_NAME || gs->grid_mode == GRID_F2 || gs->grid_mode == GRID_F2_NAME)) {
                    // toggle sub-grid
                    if (gs->subChild && IsWindow(gs->subChild) && gs->hotSubCell == i) {
                        DestroyWindow(gs->subChild);
                        gs->subChild = nullptr; gs->hotSubCell = -1;
                    } else {
                        if (gs->subChild && IsWindow(gs->subChild)) DestroyWindow(gs->subChild);
                        GridOpenSubChild(hwnd, gs, i);
                    }
                } else {
                    String cmd = gs->app->cache->path(ci.name);
                    ShellExecute(nullptr, nullptr, cmd.c_str(), nullptr, nullptr, SW_NORMAL);
                    Util::RegisterRecentLaunch(cmd);
                    {
                        String web_url = Util::GetWebUrl(cmd);
                        if (!web_url.empty() && !Util::HasCustomIcon(cmd))
                            Util::TriggerFaviconDownloadAsync(gs->app->cache->base_dir, ci.name, web_url, gs->app->cache->cache_path, cmd);
                    }
                    // Close entire grid hierarchy
                    HWND root = hwnd;
                    while (true) {
                        auto* s = (GridState*)GetWindowLongPtr(root, GWLP_USERDATA);
                        if (!s || !s->parentHwnd || !IsWindow(s->parentHwnd)) break;
                        root = s->parentHwnd;
                    }
                    PostMessage(root, WM_CLOSE, 0, 0);
                }
                return 0;
            }
        }
        // click outside any icon -> close
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (!gs) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        auto items = gs->app->GridItems(gs->prefix);
        for (int i = 0; i < (int)items.size(); ++i) {
            int col, row;
            GridCellPos(gs, i, col, row);
            RECT cell = { col*gs->cellW, gs->topPad + row*gs->cellH,
                          col*gs->cellW + gs->cellW, gs->topPad + row*gs->cellH + gs->cellH };
            if (PtInRect(&cell, pt)) {
                auto& ci = gs->app->cache->items[items[i]];
                if (ci.is_submenu) {
                    String submenu_path = ci.submenu_path.empty() ? gs->app->cache->path(ci.name) : ci.submenu_path;
                    gs->app->ShowSubfolderContextMenu(hwnd, submenu_path);
                } else {
                    String shortcut_path = gs->app->cache->path(ci.name);
                    gs->app->ShowShortcutContextMenu(hwnd, shortcut_path, gs->prefix.empty());
                }
                return 0;
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_KILLFOCUS: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        HWND nf = (HWND)wParam;
        // Don't close if focus moved to our sub-grid, or to any deeper
        // descendant (2nd level or beyond) of it, e.g. after a right-click
        // context menu was shown from a nested submenu.
        bool toChild = gs && gs->subChild && IsDescendantMenuWindow(gs->subChild, nf);
        bool isChild = gs && gs->parentHwnd != nullptr;
        if (!toChild && !isChild)
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_CLOSE: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (gs && gs->subChild && IsWindow(gs->subChild)) {
            DestroyWindow(gs->subChild);
            if (gs) { gs->subChild = nullptr; gs->hotSubCell = -1; }
        }
        DestroyWindow(hwnd);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**************************************************************************************************
 * App entry point
 **************************************************************************************************/
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPTSTR cmd_line, int) {
	ComInit com;

	String  stack_path, opts;
	int     cmd_line_error = Util::parse_cmd_line(cmd_line, stack_path, opts);

	// No arguments at all (plain double-click on stacky-plus.exe): open the
	// advanced Configuration window instead of showing the "parameter
	// missing" error message.
	if (cmd_line_error == ERR_PATH_MISSING && String(cmd_line).empty()) {
		return RunStackyConfigWindow(inst);
	}

	String  err_title = String(L"Stacky v") + STACKY_VERSION_STR + L": ";
	String  err_msg = L"Path: " + stack_path;

	Cache   cache(stack_path);
	App     app(&cache, opts);

	if (cmd_line_error == ERR_PATH_MISSING) {
		Util::msgt(
			err_title + L"Parameter missing",
			L"Pass path to the stack folder in the command line, for ex.: \n\n"
			L"        stacky-plus.exe D:\\Projects [options]\n\n"
			L"Options:\n"
			L"  --hide-shortcuts-folder  Hide the shortcuts folder item (base folder) and separator\n"
			L"  --hide-header            (deprecated: use --hide-shortcuts-folder)\n"
			L"  --compact-header         Show only folder name in the header\n"
			L"  --dark-mode              Use dark-mode for the menu\n"
			L"  --light-mode             Use light-mode for the menu\n"
			L"                           (default: follow the system color mode/accent)"
		);
	}
	else if (cmd_line_error == ERR_PATH_INVALID) {
		Util::msgt(
			err_title + L"Invalid parameter",
			L"Path: %s is not a valid directory",
			stack_path.c_str()
		);
	}
	else if (!cache.scan()) {
		Util::msgt(
			err_title + L"Invalid path",
			L"%s",
			err_msg.c_str()
		);
	}
	else if (!cache.load()) {
		Util::msgt(
			err_title + L"Failed to load stack cache",
			L"%s",
			err_msg.c_str()
		);
	}
	else if (!app.init()) {
		Util::msgt(
			err_title + L"App init failed",
			L"%s",
			err_msg.c_str()
		);
	}
	else
		app.run();

	return 0;
}
