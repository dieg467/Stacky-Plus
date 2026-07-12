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

 /**************************************************************************************************
  * Standard libs
  **************************************************************************************************/
#include <cstdio>
#include <vector>
#include <string>
#include <unordered_map>

#include "resource.h" // for version info

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
const String STACKY_EXEC_NAME = L"stacky.exe";
const Char* STACKY_WINDOW_NAME = L"stacky";
const Char* STACKY_POPUP_CLASS = L"stacky_popup";
const Char* STACKY_GRID_CLASS  = L"stacky_grid";
const Char* STACKY_TIP_CLASS   = L"stacky_tip";
const Char* DIR_SEP = L"\\";
const String SUBMENU_SUFFIX = L".submenu";
const String DESKTOP_INI = L"desktop.ini";
const String FAVICON_FOLDER = L"favicon-icon-web";
const DWORD CACHE_VERSION = 10; // Increment this when cache format changes

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

// Grid display modes
enum GridMode {
	GRID_NONE    = 0,  // standard list popup
	GRID_ICON    = 1,  // NN cols, icons only, tooltip immediately
	GRID_NAME    = 2,  // NN cols, icons + truncated name below, tooltip 300 ms
	GRID_CASCADE = 3,  // 2 cols, icons only, submenus open as 1-col grid
};

// Timer IDs used by PopupWndProc
static const UINT_PTR POPUP_TIMER_CLOSE_CHILD = 10;  // grace period before destroying child submenu

// State passed as lpCreateParams when creating each popup window.
struct PopupState {
	App*   app;
	String prefix;            // empty for root, "FolderName.submenu\\" for submenus
	HWND   parentHwnd = nullptr; // non-null for hover-opened child popups
	HWND   childHwnd  = nullptr; // currently open hover-child popup
	int    hotSubIdx  = -1;      // cache item index of the hover-child
	int    hotItem    = -1;      // hovered row index, replaces g_hotItem
	bool   closePending = false; // WM_MOUSELEAVE fired but grace period active
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

	static void kill_other_stackies() {
		PROCESSENTRY32 entry = { 0 };
		entry.dwSize = sizeof(PROCESSENTRY32);
		BOOL found = false;
		HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPALL, 0);
		do {
			found = ::Process32Next(snapshot, &entry);
			if (entry.th32ProcessID != ::GetCurrentProcessId() && entry.szExeFile == STACKY_EXEC_NAME) {
				HANDLE hOtherStacky = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, entry.th32ProcessID);
				if (hOtherStacky) {
					::TerminateProcess(hOtherStacky, 0);
					::CloseHandle(hOtherStacky);
				}
				else {
					Util::msg(L"Failed to open another stacky.exe process. Kill stacky.exe manually.");
				}
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

	// Get taskbar position and dimensions
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
				// Try getting the path – for internet shortcuts it will be a URL
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

	struct FaviconThreadParams {
		String url;
		String dest_path;
		String base_dir;
		String cache_path;  // !stacky.cache path to delete so it rebuilds next time
	};

	static DWORD WINAPI FaviconThreadProc(LPVOID param) {
		FaviconThreadParams* p = (FaviconThreadParams*)param;
		// Sleep briefly so the launched page starts loading
		::Sleep(2000);
		bool saved = DownloadAndSaveFavicon(p->url, p->dest_path, p->base_dir);
		if (saved) {
			// Delete cache so next launch rebuilds with favicon icon
			::DeleteFile(p->cache_path.c_str());
		}
		delete p;
		return 0;
	}

	// Launch favicon download in background thread.
	static void TriggerFaviconDownloadAsync(const String& base_dir, const String& item_name,
											 const String& url, const String& cache_file_path) {
		// Don't re-download if favicon already cached
		String dest = GetFaviconCachePath(base_dir, item_name);
		if (::GetFileAttributes(dest.c_str()) != INVALID_FILE_ATTRIBUTES)
			return;  // already have it

		FaviconThreadParams* p = new FaviconThreadParams();
		p->url        = url;
		p->dest_path  = dest;
		p->base_dir   = base_dir;
		p->cache_path = cache_file_path;
		HANDLE hThread = ::CreateThread(nullptr, 0, FaviconThreadProc, p, 0, nullptr);
		if (hThread) ::CloseHandle(hThread);
		else delete p;
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
		String  submenu_path;
		String  relative_path; // For items in submenus

		Item() : is_submenu(false) {}

		bool create(const String& file_name, const String& file_path, const String& base_dir = L"") {
			name = file_name;
			is_submenu = false;
			submenu_path.clear();
			relative_path.clear();

			DWORD attrs = ::GetFileAttributes(file_path.c_str());
			if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {

				// Mark as submenu if needed
				if (Util::ends_with(file_name, SUBMENU_SUFFIX)) {
					is_submenu = true;
					submenu_path = file_path;
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

			// If this is a .submenu folder, recursively scan it
			if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
				Util::ends_with(filename, SUBMENU_SUFFIX)) {
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
	struct Entry { HBITMAP bmp; SIZE sz; };
	std::unordered_map<const void*, Entry> map;

	Entry& get(HWND hwnd, HBITMAP src) {
		auto it = map.find(src);
		if (it != map.end()) return it->second;

		UINT dpi = GetDpiForWindow(hwnd);
		int s = MulDiv(32, dpi, 96);

		// Get source bitmap dimensions
		BITMAP bm;
		GetObject(src, sizeof(BITMAP), &bm);

		// If already at target size, just use it as-is
		if (bm.bmWidth == s && bm.bmHeight == s) {
			return map[src] = { src, {s, s} };
		}

		HDC hdcSrc = CreateCompatibleDC(nullptr);
		HBITMAP hOldSrc = (HBITMAP)SelectObject(hdcSrc, src);

		// Create 32-bit ARGB DIB section to preserve alpha channel
		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = s;
		bmi.bmiHeader.biHeight = -s; // negative for top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		void* pBits = nullptr;
		HBITMAP scaled = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);

		HDC hdcDst = CreateCompatibleDC(nullptr);
		HBITMAP hOldDst = (HBITMAP)SelectObject(hdcDst, scaled);

		// Set high-quality stretching mode
		SetStretchBltMode(hdcDst, HALFTONE);
		SetBrushOrgEx(hdcDst, 0, 0, nullptr);

		// Stretch the bitmap with high quality
		StretchBlt(hdcDst, 0, 0, s, s, hdcSrc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);

		SelectObject(hdcSrc, hOldSrc);
		SelectObject(hdcDst, hOldDst);
		DeleteDC(hdcSrc);
		DeleteDC(hdcDst);

		return map[src] = { scaled, {s, s} };
	}

	~IconCache() {
		for (auto& kv : map) DeleteObject(kv.second.bmp);
	}
};

/**************************************************************************************************
 * Lazy submenu payload
 **************************************************************************************************/
struct LazySubmenuData {
	Cache* cache;
	String folder_path;
};

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
		dark_mode = options.find(L"--dark-mode") != String::npos;

		// Parse iconmenu-NN / iconmenu-NN-name / iconmenu-C2
		grid_mode = GRID_NONE;
		icon_cols = 4;
		size_t im = options.find(L"iconmenu-");
		if (im != String::npos) {
			const wchar_t* p = options.c_str() + im + 9; // skip "iconmenu-"
			if (p[0] == L'C' || p[0] == L'c') {
				// iconmenu-C2 (cascade mode, always 2 cols)
				grid_mode = GRID_CASCADE;
				icon_cols = 2;
			} else {
				wchar_t* end = nullptr;
				long n = wcstol(p, &end, 10);
				if (end != p && n >= 1 && n <= 99) icon_cols = (int)n;
				// check for -name suffix
				if (end && wcsncmp(end, L"-name", 5) == 0)
					grid_mode = GRID_NAME;
				else
					grid_mode = GRID_ICON;
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

	friend LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	friend LRESULT CALLBACK GridWndProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	friend LRESULT CALLBACK TipWndProc  (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	// Data members accessed by free helper functions (GridShowTip, GridOpenSubChild)
	Cache*    cache;
	IconCache icon_cache;
	bool      dark_mode;
	GridMode  grid_mode;
	int       icon_cols;

private:
	HWND    window;
	bool    hide_header;
	bool    compact_header;

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
		RECT taskbarRect = Util::GetTaskbarRect();

		HMONITOR hMonitor = ::MonitorFromRect(&taskbarRect, MONITOR_DEFAULTTOPRIMARY);
		RECT workArea = Util::GetWorkAreaForMonitor(hMonitor);

		// Y: bottom of menu aligns with top of taskbar
		int menuY = taskbarRect.top - menuHeight;
		if (menuY < workArea.top) menuY = workArea.top;

		// X: center menu over cursor (cursor is over the taskbar icon at launch)
		POINT cursor{};
		GetCursorPos(&cursor);
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
		int iconSz    = MulDiv(32, dpi, 96);  // matches IconCache and MakeLayout
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

		for (size_t i = (isRoot ? 1 : 0); i < cache->items.size(); ++i) {
			auto& ci = cache->items[i];

			if (isRoot) {
				if (ci.name.find(DIR_SEP) != String::npos) continue;
			} else {
				if (ci.name.rfind(prefix, 0) != 0) continue;
				String rel = ci.name.substr(prefix.size());
				if (rel.empty()) continue;
				if (rel.find(DIR_SEP) != String::npos) continue; // skip non-direct children
			}

			if (IsSeparatorFile(ci.name)) {
					totalH += sepH;
					continue;
				}

			String disp = ci.name;
			if (!isRoot) disp = ci.name.substr(prefix.size());
			disp = Util::StripSortPrefix(disp);
			if (Util::ends_with(disp, SUBMENU_SUFFIX))
				disp = Util::rtrim(disp, SUBMENU_SUFFIX);
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
			if (ci.is_submenu && grid_mode != GRID_CASCADE) continue;
			v.push_back(i);
		}
		return v;
	}

	struct GridMetrics {
		int cellSz;   // icon-area side = iconSz + cellPad*2
		int cellPad;  // padding around icon inside cell
		int labelH;   // extra height for name label (0 unless GRID_NAME)
		int cellH;    // total cell height = cellSz + labelH
		int cols;
		int rows;
		int totalW;
		int totalH;
	};

	// Measure grid for a given prefix (root = empty).
	GridMetrics MeasureGridSize(const String& prefix = L"") const {
		UINT dpi    = GetDpiForWindow(window);
		int iconSz  = MulDiv(32, dpi, 96);
		int cellPad = MulDiv(8,  dpi, 96);
		int cellSz  = iconSz + cellPad * 2;
		int labelH  = (grid_mode == GRID_NAME) ? MulDiv(16, dpi, 96) : 0;
		int cellH   = cellSz + labelH;
		int cols    = icon_cols;
		int n       = (int)GridItems(prefix).size();
		int rows    = n > 0 ? (n + cols - 1) / cols : 1;
		return { cellSz, cellPad, labelH, cellH, cols, rows, cellSz * cols, cellH * rows };
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

		for (size_t i = 1; i < cache->items.size(); ++i) {
			auto& it = cache->items[i];

			// root: only direct children
			if (it.name.find(DIR_SEP) != String::npos) continue;

			if (IsSeparatorFile(it.name)) {
				InsertSeparator(menu);
				continue;
			}

			// create MenuEntry once; never store mixed pointer types
			auto* e = new MenuEntry{};
			e->item = &it;
			e->is_submenu = it.is_submenu;
			e->populated = false;

			// display text
			if (it.is_submenu) {
				String t = it.name;
				t = Util::rtrim(t, SUBMENU_SUFFIX);
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
		for (size_t i = 0; i < cache->items.size(); ++i) {
			auto& it = cache->items[i];

			// must match relative prefix
			if (it.name.rfind(prefix, 0) != 0) continue;

			String rel = it.name.substr(prefix.size());

			if (IsSeparatorFile(rel)) {
				InsertSeparator(menu);
				continue;
			}

			// direct children only (unless it.is_submenu)
			if (!it.is_submenu && rel.find(DIR_SEP) != String::npos) continue;

			auto* e = new MenuEntry{};
			e->item = &it;
			e->is_submenu = it.is_submenu;
			e->populated = false;

			if (it.is_submenu) {
				// must be direct child submenu folder
				if (rel.find(DIR_SEP) != String::npos) { delete e; continue; }
				e->text = Util::rtrim(rel, SUBMENU_SUFFIX);
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
			COLORREF bg = dark_mode ? RGB(32, 32, 32) : GetSysColor(COLOR_MENU);
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
		const COLORREF bg = dark_mode ? RGB(32, 32, 32) : GetSysColor(COLOR_MENU);
		const COLORREF fg = dark_mode ? RGB(240, 240, 240) : GetSysColor(COLOR_MENUTEXT);
		const COLORREF disfg = dark_mode ? RGB(140, 140, 140) : GetSysColor(COLOR_GRAYTEXT);

		// Selection colors (avoid the bright default blue in dark mode)
		const COLORREF selBg = dark_mode ? RGB(64, 64, 64) : GetSysColor(COLOR_HIGHLIGHT);
		const COLORREF selFg = dark_mode ? RGB(255, 255, 255) : GetSysColor(COLOR_HIGHLIGHTTEXT);

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

		AlphaBlend(dis->hDC, x, y, ic.sz.cx, ic.sz.cy, mem, 0, 0, ic.sz.cx, ic.sz.cy, bf);

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
					{
						String web_url = Util::GetWebUrl(cmd);
						if (!web_url.empty() && !Util::HasCustomIcon(cmd))
							Util::TriggerFaviconDownloadAsync(app->cache->base_dir, it.name, web_url, app->cache->cache_path);
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

static PopupLayout MakeLayout(HWND hwnd) {
	PopupLayout l{};
	l.dpi     = (int)GetDpiForWindow(hwnd);
	l.iconSz  = MulDiv(32, l.dpi, 96);   // matches IconCache scaling
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
	case WM_ERASEBKGND:
		return 1; // suppress – WM_PAINT fills every pixel with double-buffer
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
			HBRUSH hbr = CreateSolidBrush(GetSysColor(COLOR_MENU));
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

		PopupLayout L = MakeLayout(hwnd);

		COLORREF bgColor    = app->dark_mode ? RGB(32,32,32)   : GetSysColor(COLOR_MENU);
		COLORREF selColor   = app->dark_mode ? RGB(64,64,64)   : GetSysColor(COLOR_HIGHLIGHT);
		COLORREF fgColor    = app->dark_mode ? RGB(240,240,240): GetSysColor(COLOR_MENUTEXT);
		COLORREF fgSelColor = app->dark_mode ? RGB(255,255,255): GetSysColor(COLOR_HIGHLIGHTTEXT);
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

			auto& ic = app->icon_cache.get(hwnd, ci.bmp.hBmp);
			int ix = ir.left + L.hPad;
			int iy = ir.top  + (L.itemH - ic.sz.cy) / 2;
			HDC mem = CreateCompatibleDC(hdc_); HGDIOBJ old = SelectObject(mem, ic.bmp);
			BLENDFUNCTION bf{}; bf.BlendOp=AC_SRC_OVER; bf.SourceConstantAlpha=255; bf.AlphaFormat=AC_SRC_ALPHA;
			AlphaBlend(hdc_, ix, iy, ic.sz.cx, ic.sz.cy, mem, 0,0, ic.sz.cx, ic.sz.cy, bf);
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

		for (size_t i = (isRoot ? 1 : 0); i < app->cache->items.size(); ++i) {
			auto& ci = app->cache->items[i];

			if (isRoot) {
				if (ci.name.find(DIR_SEP) != String::npos) continue;
			} else {
				if (ci.name.rfind(prefix, 0) != 0) continue;
				String rel = ci.name.substr(prefix.size());
				if (rel.empty()) continue;
				if (rel.find(DIR_SEP) != String::npos) continue; // skip non-direct children
			}

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

			auto& ic = app->icon_cache.get(hwnd, ci.bmp.hBmp);
			int ix = ir.left + L.hPad;
			int iy = ir.top  + (L.itemH - ic.sz.cy) / 2;
			HDC mem = CreateCompatibleDC(hdc_); HGDIOBJ old = SelectObject(mem, ic.bmp);
			BLENDFUNCTION bf{}; bf.BlendOp=AC_SRC_OVER; bf.SourceConstantAlpha=255; bf.AlphaFormat=AC_SRC_ALPHA;
			AlphaBlend(hdc_, ix, iy, ic.sz.cx, ic.sz.cy, mem, 0,0, ic.sz.cx, ic.sz.cy, bf);
			SelectObject(mem, old); DeleteDC(mem);

			// Strip sort-prefix (%NN%), suffix and extensions for display
			String disp = isRoot ? ci.name : ci.name.substr(prefix.size());
			disp = Util::StripSortPrefix(disp);
			if (Util::ends_with(disp, SUBMENU_SUFFIX))
				disp = Util::rtrim(disp, SUBMENU_SUFFIX);
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

		PopupLayout L = MakeLayout(hwnd);
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
			for (size_t i = (isRoot ? 1 : 0); i < app->cache->items.size(); ++i) {
				auto& it = app->cache->items[i];
				if (isRoot) {
					if (it.name.find(DIR_SEP) != String::npos) continue;
				} else {
					if (it.name.rfind(prefix, 0) != 0) continue;
					String rel = it.name.substr(prefix.size());
					if (rel.empty()) continue;
					if (rel.find(DIR_SEP) != String::npos) continue; // skip non-direct children
				}
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
		if (newHotSubIdx != state->hotSubIdx) {
			// Close previous child if different row
			if (state->childHwnd && IsWindow(state->childHwnd)) {
				DestroyWindow(state->childHwnd);
				state->childHwnd = nullptr;
			}
			state->hotSubIdx = newHotSubIdx;

			if (newHotSubIdx != -1) {
				// Open new child hover-popup without stealing focus
				RECT wr; GetWindowRect(hwnd, &wr);
				POINT mid = { 0, y + L.itemH / 2 }; // y was left at the matching row top
				ClientToScreen(hwnd, &mid);
				int sx = wr.right;
				int sy = mid.y;

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
					// else: cursor entered child – keep it alive
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
			PopupLayout L = MakeLayout(hwnd);
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

			for (size_t i = (isRoot ? 1 : 0); i < app->cache->items.size(); ++i) {
				auto& it = app->cache->items[i];
				if (isRoot) {
					if (it.name.find(DIR_SEP) != String::npos) continue;
				} else {
					if (it.name.rfind(prefix, 0) != 0) continue;
					String rel = it.name.substr(prefix.size());
					if (rel.empty()) continue;
					if (rel.find(DIR_SEP) != String::npos) continue; // skip non-direct children
				}
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
							{
								String web_url = Util::GetWebUrl(cmd);
								if (!web_url.empty() && !Util::HasCustomIcon(cmd))
									Util::TriggerFaviconDownloadAsync(app->cache->base_dir, it.name, web_url, app->cache->cache_path);
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
		// Don't close if focus moved to our hover-child
		bool focusToChild = state && state->childHwnd && (newFocus == state->childHwnd);
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
    int       cellSz;
    int       cellH;        // cellSz + labelH
    int       labelH;
    int       cols;
    int       rows;
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

// Timer IDs for the grid window
static const UINT_PTR GRID_TIMER_SHOW      = 1;  // delay before showing tooltip
static const UINT_PTR GRID_TIMER_HIDE      = 2;  // auto-hide tooltip
static const UINT_PTR GRID_TIMER_SUBCL     = 3;  // grace period before closing sub-grid

// Helper: build display name from a cache item name + prefix
static String GridDisplayName(const String& name, const String& prefix) {
    String s = name.substr(prefix.size());
    s = Util::StripSortPrefix(s);
    // strip known extensions
    for (auto& ext : { L".lnk", L".bat", L".cmd", L".exe", L".vbs", L".url" })
        if (Util::ends_with(s, ext)) { s = Util::rtrim(s, ext); break; }
    // strip .submenu suffix
    if (Util::ends_with(s, SUBMENU_SUFFIX)) s = Util::rtrim(s, SUBMENU_SUFFIX);
    return s;
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
    int col = cellIdx % gs->cols;
    int row = cellIdx / gs->cols;
    POINT origin = {0,0}; ClientToScreen(hwnd, &origin);
    int tipX = origin.x + col * gs->cellSz + gs->cellSz/2 - tipW/2;
    int tipY = origin.y + row * gs->cellH - tipH - 2;
    // if above screen, place below icon
    if (tipY < 0) tipY = origin.y + row * gs->cellH + gs->cellSz + 2;

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

// Helper: open a 1-column sub-grid for a submenu item (GRID_CASCADE)
static void GridOpenSubChild(HWND hwnd, GridState* gs, int cellIdx) {
    auto items = gs->app->GridItems(gs->prefix);
    if (cellIdx < 0 || cellIdx >= (int)items.size()) return;
    auto& ci = gs->app->cache->items[items[cellIdx]];
    if (!ci.is_submenu) return;

    String childPrefix = ci.name + DIR_SEP;
    UINT dpi = GetDpiForWindow(hwnd);
    int iconSz  = MulDiv(32, dpi, 96);
    int cellPad = MulDiv(8,  dpi, 96);
    int cSz     = iconSz + cellPad*2;
    int subN    = (int)gs->app->GridItems(childPrefix).size();
    int subW    = cSz;
    int subH    = cSz * subN;
    if (subN == 0) return;

    // Determine preferred open direction:
    // - Root grid (no parent): column of cellIdx in the 2-col root decides direction.
    // - Sub-grid: inherit the direction already established by the root column.
    int openRight;
    if (gs->parentHwnd == nullptr) {
        // Root grid: col 0 (left column) ? open submenu to the left; col 1 (right) ? right.
        int col = cellIdx % gs->cols;
        openRight = (col == 0) ? -1 : +1;
    } else {
        // Inherited direction from the chain started at the root.
        openRight = gs->openRight;
    }

    int row = cellIdx / gs->cols;
    RECT wr; GetWindowRect(hwnd, &wr);
    POINT origin = {0, 0}; ClientToScreen(hwnd, &origin);
    int cy = origin.y + row * gs->cellH;

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

    // Clamp vertically
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
        // For GRID_CASCADE sub-grids force 1 column
        int cols = cp->prefix.empty() ? gm.cols : 1;
        auto* gs  = new GridState{};
        gs->app           = cp->app;
        gs->prefix        = cp->prefix;
        gs->parentHwnd    = cp->parentHwnd;
        gs->grid_mode     = cp->app->grid_mode;
        gs->cellSz        = gm.cellSz;
        gs->cellH         = gm.cellH;
        gs->labelH        = gm.labelH;
        gs->cols          = cols;
        gs->rows          = gm.rows;
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
    case WM_DESTROY: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (gs) {
            KillTimer(hwnd, GRID_TIMER_SHOW);
            KillTimer(hwnd, GRID_TIMER_HIDE);
            KillTimer(hwnd, GRID_TIMER_SUBCL);
            if (gs->tipHwnd && IsWindow(gs->tipHwnd)) DestroyWindow(gs->tipHwnd);
            if (gs->subChild && IsWindow(gs->subChild)) DestroyWindow(gs->subChild);
            // notify parent
            if (gs->parentHwnd && IsWindow(gs->parentHwnd)) {
                auto* ps = (GridState*)GetWindowLongPtr(gs->parentHwnd, GWLP_USERDATA);
                if (ps && ps->subChild == hwnd) {
                    ps->subChild   = nullptr;
                    ps->hotSubCell = -1;
                }
            }
            delete gs;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        }
        // only post quit for root grid (no parent)
        {
            auto* gs2 = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            // gs2 is now null (we deleted it), check parentHwnd via local copy
        }
        // We always post quit; sub-grids are destroyed by their parent before root quits
        PostQuitMessage(0);
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
        COLORREF gridBg  = dm ? RGB(32,32,32)  : RGB(240,240,240);
        COLORREF gridHov = dm ? RGB(64,64,64)  : RGB(204,228,247);
        COLORREF labelFg = dm ? RGB(220,220,220): RGB(30,30,30);
        HBRUSH bgBr = CreateSolidBrush(gridBg);
        FillRect(memDC, &rc, bgBr); DeleteObject(bgBr);

        auto items = gs->app->GridItems(gs->prefix);
        int  sz    = gs->cellSz;
        int  cH    = gs->cellH;
        int  pad   = gs->cellSz - MulDiv(32, GetDpiForWindow(hwnd), 96); // = cellPad*2 approx
        int  iconSz= sz - pad;  // = iconSz approx
        // more precise: derive from cellSz
        UINT dpi   = GetDpiForWindow(hwnd);
        int  iSz   = MulDiv(32, dpi, 96);
        int  iPad  = MulDiv(8,  dpi, 96);

        // Prepare label font (used for GRID_NAME and also submenu arrow)
        HFONT labelFnt = nullptr;
        if (gs->grid_mode == GRID_NAME || gs->grid_mode == GRID_CASCADE) {
            LOGFONT lf{}; lf.lfHeight = -MulDiv(9, dpi, 96); lf.lfWeight = FW_NORMAL;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            labelFnt = CreateFontIndirect(&lf);
        }

        for (int i = 0; i < (int)items.size(); ++i) {
            int col = i % gs->cols;
            int row = i / gs->cols;
            int x = col * sz;
            int y = row * cH;
            RECT cell = { x, y, x + sz, y + sz };

            // hover highlight (icon area)
            if (i == gs->hotCell) {
                HBRUSH hlBr = CreateSolidBrush(gridHov);
                FillRect(memDC, &cell, hlBr); DeleteObject(hlBr);
            }

            // draw icon
            auto& ci = gs->app->cache->items[items[i]];
            auto& ic = gs->app->icon_cache.get(hwnd, ci.bmp.hBmp);
            HDC tmpDC = CreateCompatibleDC(memDC);
            HGDIOBJ oldTmp = SelectObject(tmpDC, ic.bmp);
            BLENDFUNCTION bf{}; bf.BlendOp = AC_SRC_OVER; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
            int drawSz = min(iSz, (int)min(ic.sz.cx, ic.sz.cy));
            int ox = x + iPad + (iSz - drawSz) / 2;
            int oy = y + iPad + (iSz - drawSz) / 2;
            AlphaBlend(memDC, ox, oy, drawSz, drawSz, tmpDC, 0, 0, ic.sz.cx, ic.sz.cy, bf);
            SelectObject(tmpDC, oldTmp); DeleteDC(tmpDC);

            // draw label below icon (GRID_NAME mode)
            if (gs->grid_mode == GRID_NAME && gs->labelH > 0 && labelFnt) {
                String disp = GridDisplayName(ci.name, gs->prefix);
                HFONT oldF = (HFONT)SelectObject(memDC, labelFnt);
                SetTextColor(memDC, labelFg);
                SetBkMode(memDC, TRANSPARENT);
                // Measure width of 'a' as lateral margin
                SIZE aSz{}; GetTextExtentPoint32(memDC, L"a", 1, &aSz);
                int margin = aSz.cx;
                RECT lr = { x + margin, y + sz, x + sz - margin, y + cH };
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

            // draw small submenu indicator for GRID_CASCADE submenu items
            if (gs->grid_mode == GRID_CASCADE && ci.is_submenu) {
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
        }
        if (labelFnt) DeleteObject(labelFnt);
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
            int col = i % gs->cols;
            int row = i / gs->cols;
            RECT cell = { col*gs->cellSz, row*gs->cellH,
                          col*gs->cellSz + gs->cellSz, row*gs->cellH + gs->cellSz };
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
                bool isSubmenu = (gs->grid_mode == GRID_CASCADE) &&
                    newHot < (int)items.size() &&
                    gs->app->cache->items[items[newHot]].is_submenu;

                // 500 ms delay before showing tooltip for all modes
                SetTimer(hwnd, GRID_TIMER_SHOW, 1000, nullptr);

                // GRID_CASCADE: manage sub-grid on hover
                if (gs->grid_mode == GRID_CASCADE) {
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
                if (gs->grid_mode == GRID_CASCADE && gs->subChild && IsWindow(gs->subChild)) {
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
            if (gs->grid_mode == GRID_CASCADE && gs->subChild && IsWindow(gs->subChild)) {
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
            int col = i % gs->cols;
            int row = i / gs->cols;
            RECT cell = { col*gs->cellSz, row*gs->cellH,
                          col*gs->cellSz + gs->cellSz, row*gs->cellH + gs->cellSz };
            if (PtInRect(&cell, pt)) {
                auto& ci = gs->app->cache->items[items[i]];
                if (ci.is_submenu && gs->grid_mode == GRID_CASCADE) {
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
                    {
                        String web_url = Util::GetWebUrl(cmd);
                        if (!web_url.empty() && !Util::HasCustomIcon(cmd))
                            Util::TriggerFaviconDownloadAsync(gs->app->cache->base_dir, ci.name, web_url, gs->app->cache->cache_path);
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
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_KILLFOCUS: {
        auto* gs = (GridState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        HWND nf = (HWND)wParam;
        bool toChild = gs && gs->subChild && (nf == gs->subChild);
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
	String  err_title = String(L"Stacky v") + STACKY_VERSION_STR + L": ";
	String  err_msg = L"Path: " + stack_path;

	Cache   cache(stack_path);
	App     app(&cache, opts);

	if (cmd_line_error == ERR_PATH_MISSING) {
		Util::msgt(
			err_title + L"Parameter missing",
			L"Pass path to the stack folder in the command line, for ex.: \n\n"
			L"        stacky.exe D:\\Projects [options]\n\n"
			L"Options:\n"
			L"  --hide-shortcuts-folder  Hide the shortcuts folder item (base folder) and separator\n"
			L"  --hide-header            (deprecated: use --hide-shortcuts-folder)\n"
			L"  --compact-header         Show only folder name in the header\n"
			L"  --dark-mode              Use dark-mode for the menu"
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
