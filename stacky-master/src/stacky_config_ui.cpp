// stacky_config_ui.cpp
// Advanced Configuration window implementation ("CONFIGURACIÓN DE STACKY-PLUS").
// See stacky_config_ui.h for the entry point used from stacky.cpp's wWinMain.
//
// NOTE: this is being built incrementally. This first version registers and
// shows the two-pane window shell (left: folder tree: right: placeholder
// panel) and the bottom action buttons; per-mode option panels are added in
// a following iteration.

#include "stacky_config_ui.h"
#include "stacky_config.h"

#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shlwapi.lib")
#include <dwmapi.h>

// Opt into Common Controls version 6 (the "themed"/modern look used by
// Windows Explorer and other native Windows 10/11 dialogs) via a linker
// manifest, instead of relying on an external .manifest file. Without this,
// buttons/combos/treeview render with the old flat Windows 98-style visuals
// regardless of InitCommonControlsEx.
#pragma comment(linker, \
	"\"/manifestdependency:type='win32' "\
	"name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "\
	"processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

const wchar_t* kConfigWndClass = L"stacky_config_wnd";
const wchar_t* kConfigWndTitle = L"Configuración de Stacky-Plus";

// Reads the current Windows accent color (same source used by the main
// popup/grid menus) so the TreeView selection/hover highlight matches it.
COLORREF ReadAccentColor() {
	DWORD colorizationColor = 0; BOOL opaque = FALSE;
	if (SUCCEEDED(::DwmGetColorizationColor(&colorizationColor, &opaque))) {
		BYTE r = (BYTE)((colorizationColor >> 16) & 0xFF);
		BYTE g = (BYTE)((colorizationColor >> 8) & 0xFF);
		BYTE b = (BYTE)(colorizationColor & 0xFF);
		return RGB(r, g, b);
	}
	return RGB(0, 120, 215);
}

enum ControlId {
	IDC_TREE = 1001,
	IDC_BTN_CANCEL = 1002,
	IDC_BTN_SAVE = 1003,
	IDC_BTN_CREATE = 1004,
	IDC_BTN_DELETE = 1005,

	// Root-folder panel controls
	IDC_LBL_NAME = 1100,
	IDC_EDIT_NAME = 1101,
	IDC_LBL_ICON = 1102,
	IDC_EDIT_ICON = 1103,
	IDC_BTN_BROWSE_ICON = 1104,
	IDC_LBL_ICONSIZE = 1105,
	IDC_ICON_PREVIEW = 1119,
	IDC_SUBICON_PREVIEW = 1157,
	IDC_CHECK_MINI = 1106,
	IDC_LBL_THEME = 1107,
	IDC_COMBO_THEME = 1108,
	IDC_LBL_POSITION = 1109,
	IDC_CHECK_MOUSEPOS = 1110,
	IDC_LBL_MODE = 1111,
	IDC_COMBO_MODE = 1112,
	IDC_LBL_SORT = 1113,
	IDC_COMBO_SORT = 1114,
	IDC_LBL_COLS = 1115,
	IDC_EDIT_COLS = 1116,
	IDC_CHECK_NAMES_BELOW = 1117,
	IDC_CHECK_NAMES_RIGHT = 1118,
	IDC_CHECK_ADD_SEPARATOR = 1120,

	// Submenu-only panel controls
	IDC_LBL_SUBICON = 1150,
	IDC_EDIT_SUBICON = 1151,
	IDC_BTN_BROWSE_SUBICON = 1152,
	IDC_LBL_SUBCOLS = 1153,
	IDC_EDIT_SUBCOLS = 1154,
	IDC_LBL_SUBLAYOUT = 1155,
	IDC_COMBO_SUBLAYOUT = 1156,

	IDC_LBL_PLACEHOLDER = 1199,
};

// A folder node in the tree: full absolute path plus whether it is a
// top-level ("root") folder (direct child of the exe folder) or a nested
// subfolder (submenu).
struct TreeNodeInfo {
	std::wstring fullPath;
	bool isRoot = false;
	int parentIndex = -1;    // index into ConfigWindowState::nodes, or -1 for root nodes
	HTREEITEM hItem = nullptr;
	bool disabled = false;   // true when the parent folder's mode doesn't support submenus
	bool hasShortcut = false; // root nodes only: true if a .lnk currently exists for this menu
};

struct ConfigWindowState {
	HWND hwndTree = nullptr;
	HWND hwndRightPanel = nullptr; // placeholder label shown when nothing is selected
	HWND hwndBtnCancel = nullptr;
	HWND hwndBtnSave = nullptr;
	HWND hwndBtnCreate = nullptr;
	HWND hwndBtnDelete = nullptr;
	std::wstring exeFolder;
	COLORREF accentColor = RGB(0, 120, 215);

	// All folder nodes discovered while populating the tree, indexed by the
	// value stored in each TVITEM's lParam.
	std::vector<TreeNodeInfo> nodes;

	// Currently selected node (index into nodes, or -1 if none).
	int selectedNode = -1;

	// True while ShowPanelFor()/ApplyConfigToControls() are populating the
	// panel controls from a freshly-loaded .stacky-config. WM_COMMAND
	// notifications (EN_CHANGE/BN_CLICKED/CBN_SELCHANGE) fired by those
	// programmatic SetWindowText/Button_SetCheck/ComboBox_SetCurSel calls
	// must be ignored, otherwise the auto-save-on-change logic would
	// immediately overwrite the just-loaded config with a mix of new and
	// stale control values (e.g. the previous node's checkbox states).
	bool populatingControls = false;

	// Root-folder panel controls
	HWND hwndLblName = nullptr, hwndEditName = nullptr;
	HWND hwndLblIcon = nullptr, hwndEditIcon = nullptr, hwndBtnBrowseIcon = nullptr, hwndIconPreview = nullptr;
	HWND hwndCheckMini = nullptr;
	HWND hwndLblTheme = nullptr, hwndComboTheme = nullptr;
	HWND hwndLblPosition = nullptr, hwndCheckMousePos = nullptr;
	HWND hwndLblMode = nullptr, hwndComboMode = nullptr;
	HWND hwndLblSort = nullptr, hwndComboSort = nullptr;
	HWND hwndCheckAddSeparator = nullptr;
	HWND hwndLblCols = nullptr, hwndEditCols = nullptr;
	HWND hwndCheckNamesBelow = nullptr, hwndCheckNamesRight = nullptr;

	// Original root-panel positions (client coords, relative to hwndRightPanel's
	// parent window) of hwndCheckMini/hwndCheckAddSeparator, captured right
	// after they're created in CreateRightPanelControls(). ShowPanelFor()
	// temporarily moves both controls down (below the submenu layout combo)
	// while showing a submenu's panel, since their original spot overlaps the
	// submenu-only fields; these are used to move them back into place when
	// the root panel is shown again.
	RECT rcCheckMiniOrig{}, rcCheckAddSeparatorOrig{};

	// Submenu-only panel controls
	HWND hwndLblSubIcon = nullptr, hwndEditSubIcon = nullptr, hwndBtnBrowseSubIcon = nullptr, hwndSubIconPreview = nullptr;
	HWND hwndLblSubCols = nullptr, hwndEditSubCols = nullptr;
	HWND hwndLblSubLayout = nullptr, hwndComboSubLayout = nullptr;

	// Icons currently shown by the preview STATIC controls above; owned by
	// this state and destroyed/replaced whenever a new one is loaded.
	HICON hIconPreview = nullptr;
	HICON hSubIconPreview = nullptr;
};

std::wstring GetExeFolderPath() {
	wchar_t path[MAX_PATH] = {0};
	::GetModuleFileName(nullptr, path, MAX_PATH);
	std::wstring s(path);
	size_t pos = s.find_last_of(L'\\');
	return pos == std::wstring::npos ? s : s.substr(0, pos + 1);
}

void PopulateTreeFolder(HWND hwndTree, HTREEITEM parent, const std::wstring& folder,
						 std::vector<TreeNodeInfo>& nodes, bool isRootLevel, int parentIndex) {
	std::wstring search = folder + L"*";
	WIN32_FIND_DATA ffd = {0};
	HANDLE hFind = ::FindFirstFile(search.c_str(), &ffd);
	if (hFind == INVALID_HANDLE_VALUE) return;
	do {
		std::wstring name = ffd.cFileName;
		if (name == L"." || name == L"..") continue;
		if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;

		std::wstring fullPath = folder + name + L"\\";
		TreeNodeInfo info;
		info.fullPath = fullPath;
		info.isRoot = isRootLevel;
		info.parentIndex = parentIndex;
		// Actual disabled state (depends on the root menu's mode and this
		// node's depth) is computed afterwards by RefreshDisabledStates()
		// once the whole tree is built.
		info.disabled = false;
		nodes.push_back(info);
		int myIndex = (int)nodes.size() - 1;
		LPARAM lparam = (LPARAM)myIndex;

		TVINSERTSTRUCT tvi = {0};
		tvi.hParent = parent;
		tvi.hInsertAfter = TVI_LAST;
		tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
		tvi.item.pszText = const_cast<wchar_t*>(name.c_str());
		tvi.item.lParam = lparam;
		HTREEITEM node = TreeView_InsertItem(hwndTree, &tvi);
		nodes[myIndex].hItem = node;

		PopulateTreeFolder(hwndTree, node, fullPath, nodes, false, myIndex);
	} while (::FindNextFile(hFind, &ffd) != 0);
	::FindClose(hFind);
}

// Recomputes TreeNodeInfo::disabled for every node based on its top-level
// root folder's configured mode and its depth below that root. This must be
// re-run whenever a folder's mode is changed and saved, since
// PopulateTreeFolder only computes this once (at tree population time) and
// the tree isn't rebuilt when a config is edited - without this, a submenu
// that becomes selectable (e.g. switching the root mode) would stay greyed
// out/unselectable until the window was closed and reopened.
//
// Rules (per root mode):
//   - SCFG_MODE_DEFAULT (Lista con submenus), SCFG_MODE_DOUBLE_COL (Doble
//     columna) and SCFG_MODE_DOUBLE_ROW (Doble fila): every nested
//     subfolder at any depth is enabled/selectable, so its "ICONO DEL
//     SUBMENU" field can be edited.
//   - SCFG_MODE_SINGLESUB (Submenu unico): only the first level of
//     subfolders (direct children of the root) is enabled; deeper nested
//     subfolders are disabled, since --singlesubmenu flattens everything
//     into a single submenu and doesn't support further recursive nesting.
//   - SCFG_MODE_ICONGRID (Cuadricula de iconos): the root renders as a flat
//     icon grid with no clickable submenus at all, so every subfolder is
//     disabled.
void RefreshDisabledStates(ConfigWindowState* state) {
	for (int i = 0; i < (int)state->nodes.size(); ++i) {
		TreeNodeInfo& node = state->nodes[i];
		if (node.isRoot || node.parentIndex < 0) {
			node.disabled = false;
			continue;
		}

		// Walk up to the top-level root ancestor, counting depth along the
		// way (1 = direct child of the root, 2 = grandchild, etc.).
		int depth = 0;
		int idx = i;
		while (!state->nodes[idx].isRoot && state->nodes[idx].parentIndex >= 0) {
			idx = state->nodes[idx].parentIndex;
			++depth;
		}

		StackyFolderConfig rootCfg = StackyFolderConfig::Load(state->nodes[idx].fullPath);
		switch (rootCfg.mode) {
		case SCFG_MODE_ICONGRID:
			node.disabled = true;
			break;
		case SCFG_MODE_SINGLESUB:
			node.disabled = depth > 1;
			break;
		default: // SCFG_MODE_DEFAULT, SCFG_MODE_DOUBLE_COL, SCFG_MODE_DOUBLE_ROW
			node.disabled = false;
			break;
		}
	}
	if (state->hwndTree) ::InvalidateRect(state->hwndTree, nullptr, TRUE);
}

// Opens the standard Windows "Cambiar icono" (Change Icon) picker, seeded
// with the current path if any. Returns true and fills outPath with
// "path,index" (matching the format Windows itself uses for icon locations)
// on success.
bool PickIcon(HWND owner, std::wstring& outPath) {
	typedef BOOL(WINAPI* PickIconDlgProc)(HWND, LPWSTR, UINT, int*);
	HMODULE hShell32 = ::LoadLibrary(L"shell32.dll");
	if (!hShell32) return false;
	PickIconDlgProc pickIconDlg = reinterpret_cast<PickIconDlgProc>(
		::GetProcAddress(hShell32, MAKEINTRESOURCEA(62)));
	bool result = false;
	if (pickIconDlg) {
		wchar_t buffer[MAX_PATH] = {0};
		int iconIndex = 0;
		if (!outPath.empty()) {
			// outPath may already be in "path,index" form; split it so the
			// picker re-selects the previously chosen icon.
			std::wstring seedPath = outPath;
			size_t comma = seedPath.find_last_of(L',');
			if (comma != std::wstring::npos) {
				iconIndex = _wtoi(seedPath.c_str() + comma + 1);
				seedPath = seedPath.substr(0, comma);
			}
			wcsncpy_s(buffer, seedPath.c_str(), _TRUNCATE);
		} else {
			wchar_t sysDir[MAX_PATH] = {0};
			::GetSystemDirectory(sysDir, MAX_PATH);
			swprintf_s(buffer, L"%s\\shell32.dll", sysDir);
		}
		if (pickIconDlg(owner, buffer, MAX_PATH, &iconIndex)) {
			wchar_t combined[MAX_PATH + 16] = {0};
			swprintf_s(combined, L"%s,%d", buffer, iconIndex);
			outPath = combined;
			result = true;
		}
	}
	::FreeLibrary(hShell32);
	return result;
}

// Resolves the icon currently shown by Explorer for folderPath (honoring a
// custom desktop.ini icon override if present) into "path,index" form, the
// same format used by menu_icon_path/submenu_icon_path. Used to pre-fill the
// icon path edit box with the folder's actual default icon when no custom
// icon has been configured yet, so the field always shows a valid path
// instead of being blank.
std::wstring ResolveFolderIconPath(const std::wstring& folderPath) {
	if (folderPath.empty()) return L"";
	SHFILEINFO sfi = {0};
	if (::SHGetFileInfo(folderPath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICONLOCATION) && sfi.szDisplayName[0] != L'\0') {
		return std::wstring(sfi.szDisplayName) + L"," + std::to_wstring(sfi.iIcon);
	}
	return L"";
}

// Extracts a small (typically 16x16/32x32, whatever the shell gives back for
// SHGFI_SMALLICON/SHGFI_ICON) HICON from an icon spec in "path,index" form
// (the same format used for menu_icon_path/submenu_icon_path and produced by
// PickIcon()). If spec is empty, falls back to the shell's icon for
// fallbackFolder (i.e. the folder's own icon, honoring desktop.ini). Returns
// nullptr if nothing could be loaded. Caller owns the returned HICON and
// must DestroyIcon() it eventually.
HICON LoadIconFromSpec(const std::wstring& spec, const std::wstring& fallbackFolder) {
	if (!spec.empty()) {
		std::wstring path = spec;
		int index = 0;
		size_t comma = path.find_last_of(L',');
		if (comma != std::wstring::npos) {
			index = _wtoi(path.c_str() + comma + 1);
			path = path.substr(0, comma);
		}
		HICON hIcon = nullptr;
		UINT extracted = ::ExtractIconEx(path.c_str(), index, nullptr, &hIcon, 1);
		if (extracted > 0 && hIcon) return hIcon;
	}
	if (!fallbackFolder.empty()) {
		SHFILEINFO sfi = {0};
		if (::SHGetFileInfo(fallbackFolder.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon) {
			return sfi.hIcon;
		}
	}
	return nullptr;
}

// Updates one of the preview STATIC controls (STM_SETICON) with the icon
// loaded from iconSpec (falling back to fallbackFolder's own icon when
// iconSpec is empty), destroying whatever icon was previously shown so GDI
// icon handles don't leak as the user browses different menus/submenus.
void SetPreviewIcon(HWND hwndPreview, HICON& ownedIcon, const std::wstring& iconSpec, const std::wstring& fallbackFolder) {
	if (!hwndPreview) return;
	HICON newIcon = LoadIconFromSpec(iconSpec, fallbackFolder);
	::SendMessage(hwndPreview, STM_SETICON, (WPARAM)newIcon, 0);
	if (ownedIcon) ::DestroyIcon(ownedIcon);
	ownedIcon = newIcon;
}

// Creates a label + returns it; small helper to reduce repetition.
HWND CreateLabel(HWND parent, HINSTANCE hInst, const wchar_t* text, int x, int y, int w, int h, int id) {
	return ::CreateWindowEx(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
		x, y, w, h, parent, (HMENU)(INT_PTR)id, hInst, nullptr);
}

// Scans folder's direct children and reports whether it contains at least
// one subfolder and at least one plain file (shortcut/exe/etc, ignoring
// hidden files and the internal ".stacky-config"/"!stacky.cache" files).
// Used to decide whether the "Agregar separador..." checkbox makes sense:
// it only does when a menu/submenu mixes submenu folders with simple
// shortcuts, since its purpose is to visually separate the two groups.
void FolderContentKind(const std::wstring& folder, bool& hasSubfolder, bool& hasPlainItem) {
	hasSubfolder = false;
	hasPlainItem = false;
	std::wstring search = folder + L"*";
	WIN32_FIND_DATA ffd = {0};
	HANDLE hFind = ::FindFirstFile(search.c_str(), &ffd);
	if (hFind == INVALID_HANDLE_VALUE) return;
	do {
		std::wstring name = ffd.cFileName;
		if (name == L"." || name == L"..") continue;
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
		if (_wcsicmp(name.c_str(), L".stacky-config") == 0) continue;
		if (_wcsicmp(name.c_str(), L"!stacky.cache") == 0) continue;
		if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) hasSubfolder = true;
		else hasPlainItem = true;
	} while (::FindNextFile(hFind, &ffd) != 0);
	::FindClose(hFind);
}

// EnumChildWindows callback used by ApplyDefaultFontToChildren: applies the
// system's DEFAULT_GUI_FONT to every child control via WM_SETFONT so labels,
// edit boxes, checkboxes, combo boxes and buttons all match the modern
// Windows UI font instead of the old System font used by raw CreateWindow.
BOOL CALLBACK SetChildFontProc(HWND childHwnd, LPARAM lParam) {
	HFONT font = reinterpret_cast<HFONT>(lParam);
	::SendMessage(childHwnd, WM_SETFONT, (WPARAM)font, TRUE);
	return TRUE;
}

// Applies DEFAULT_GUI_FONT to every direct/indirect child of hwnd (labels,
// edits, combos, checkboxes, buttons, the tree view, etc.).
void ApplyDefaultFontToChildren(HWND hwnd) {
	HFONT font = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
	::EnumChildWindows(hwnd, SetChildFontProc, (LPARAM)font);
}

// Scans exeFolder for a .lnk shortcut whose target arguments point at
// targetFolder and returns its full path, or an empty string if none is
// found. Used both for the "has a menu created" tree indicator and for the
// explicit "Eliminar menú" action.
std::wstring FindShortcutForFolder(const std::wstring& exeFolder, const std::wstring& targetFolder) {
	std::wstring search = exeFolder + L"*.lnk";
	WIN32_FIND_DATA ffd = {0};
	HANDLE hFind = ::FindFirstFile(search.c_str(), &ffd);
	if (hFind == INVALID_HANDLE_VALUE) return L"";
	std::wstring found;
	do {
		std::wstring lnkPath = exeFolder + ffd.cFileName;
		IShellLink* psl = nullptr;
		if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, (void**)&psl))) {
			IPersistFile* ppf = nullptr;
			if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
				if (SUCCEEDED(ppf->Load(lnkPath.c_str(), STGM_READ))) {
					wchar_t args[MAX_PATH] = {0};
					if (SUCCEEDED(psl->GetArguments(args, MAX_PATH)) && args[0] != L'\0') {
						std::wstring argFolder = args;
						if (!argFolder.empty() && argFolder.back() != L'\\') argFolder += L'\\';
						if (_wcsicmp(argFolder.c_str(), targetFolder.c_str()) == 0) {
							found = lnkPath;
						}
					}
				}
				ppf->Release();
			}
			psl->Release();
		}
	} while (found.empty() && ::FindNextFile(hFind, &ffd) != 0);
	::FindClose(hFind);
	return found;
}

// Scans exeFolder for .lnk shortcuts whose target arguments point at
// targetFolder and deletes any of them other than keepPath. This is used to
// clean up the previous shortcut when "Crear menú" is used after the menu's
// display name (and therefore its .lnk file name) has changed, so a stale
// duplicate shortcut isn't left behind.
void RemoveStaleShortcutsForFolder(const std::wstring& exeFolder, const std::wstring& targetFolder,
									const std::wstring& keepPath) {
	std::wstring search = exeFolder + L"*.lnk";
	WIN32_FIND_DATA ffd = {0};
	HANDLE hFind = ::FindFirstFile(search.c_str(), &ffd);
	if (hFind == INVALID_HANDLE_VALUE) return;
	do {
		std::wstring lnkPath = exeFolder + ffd.cFileName;
		if (_wcsicmp(lnkPath.c_str(), keepPath.c_str()) == 0) continue;

		IShellLink* psl = nullptr;
		if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, (void**)&psl))) {
			IPersistFile* ppf = nullptr;
			if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
				if (SUCCEEDED(ppf->Load(lnkPath.c_str(), STGM_READ))) {
					wchar_t args[MAX_PATH] = {0};
					if (SUCCEEDED(psl->GetArguments(args, MAX_PATH)) && args[0] != L'\0') {
						std::wstring argFolder = args;
						if (!argFolder.empty() && argFolder.back() != L'\\') argFolder += L'\\';
						if (_wcsicmp(argFolder.c_str(), targetFolder.c_str()) == 0) {
							::DeleteFile(lnkPath.c_str());
						}
					}
				}
				ppf->Release();
			}
			psl->Release();
		}
	} while (::FindNextFile(hFind, &ffd) != 0);
	::FindClose(hFind);
}

// Creates (or overwrites) a .lnk shortcut beside the executable that targets
// stacky-plus.exe with the folder path as its command-line argument, so
// double-clicking the shortcut opens the corresponding stack menu. The
// shortcut's display name and icon come from the folder's .stacky-config
// (menu_name / menu_icon_path), falling back to the folder name / exe icon.
bool CreateShortcutForNode(HWND owner, const std::wstring& exeFolder, const TreeNodeInfo& info) {
	if (!info.isRoot) return false; // only root (top-level) folders get a launcher shortcut

	StackyFolderConfig cfg = StackyFolderConfig::Load(info.fullPath);

	// Derive the folder's own name (last path segment) for the default title.
	std::wstring path = info.fullPath;
	if (!path.empty() && path.back() == L'\\') path.pop_back();
	size_t slash = path.find_last_of(L'\\');
	std::wstring folderName = (slash == std::wstring::npos) ? path : path.substr(slash + 1);

	std::wstring displayName = !cfg.menu_name.empty() ? cfg.menu_name : folderName;

	wchar_t exePath[MAX_PATH] = {0};
	::GetModuleFileName(nullptr, exePath, MAX_PATH);

	std::wstring lnkPath = exeFolder + displayName + L".lnk";

	// Overwrite any previous shortcut(s) for this same folder, in case its
	// display name (and thus .lnk file name) changed since it was created.
	RemoveStaleShortcutsForFolder(exeFolder, info.fullPath, lnkPath);

	bool ok = false;
	IShellLink* psl = nullptr;
	if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink, (void**)&psl))) {
		psl->SetPath(exePath);
		psl->SetArguments(info.fullPath.c_str());
		psl->SetWorkingDirectory(exeFolder.c_str());

		// menu_icon_path may be stored as "path,index" (as produced by the
		// "Cambiar icono" picker) or as a plain path (custom .ico via
		// "Examinar..."), which defaults to index 0.
		std::wstring iconPath = !cfg.menu_icon_path.empty() ? cfg.menu_icon_path : std::wstring(exePath);
		int iconIdx = 0;
		size_t comma = iconPath.find_last_of(L',');
		if (comma != std::wstring::npos) {
			iconIdx = _wtoi(iconPath.c_str() + comma + 1);
			iconPath = iconPath.substr(0, comma);
		}
		psl->SetIconLocation(iconPath.c_str(), iconIdx);

		IPersistFile* ppf = nullptr;
		if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
			ok = SUCCEEDED(ppf->Save(lnkPath.c_str(), TRUE));
			ppf->Release();
		}
		psl->Release();
	}

	if (!ok) {
		::MessageBox(owner, L"No se pudo crear el acceso directo del menú.", L"Stacky", MB_OK | MB_ICONERROR);
	}
	return ok;
}

// Fills theme/mode/sort/layout combo boxes with their fixed option sets.
void PopulateCombos(ConfigWindowState* state) {
	ComboBox_AddString(state->hwndComboTheme, L"Sistema");
	ComboBox_AddString(state->hwndComboTheme, L"Claro");
	ComboBox_AddString(state->hwndComboTheme, L"Oscuro");

	ComboBox_AddString(state->hwndComboMode, L"Lista con submenús");
	ComboBox_AddString(state->hwndComboMode, L"Cuadrícula de iconos");
	ComboBox_AddString(state->hwndComboMode, L"Doble columna");
	ComboBox_AddString(state->hwndComboMode, L"Doble fila");
	ComboBox_AddString(state->hwndComboMode, L"Submenú único");

	ComboBox_AddString(state->hwndComboSort, L"Alfabético");
	ComboBox_AddString(state->hwndComboSort, L"Carpetas primero");

	ComboBox_AddString(state->hwndComboSubLayout, L"Solo icono");
	ComboBox_AddString(state->hwndComboSubLayout, L"Nombre a la derecha");
	ComboBox_AddString(state->hwndComboSubLayout, L"Nombre debajo");
}

// Creates every control used by the right-hand panel (both root and submenu
// variants); visibility is toggled afterwards depending on selection.
void CreateRightPanelControls(HWND hwnd, HINSTANCE hInst, ConfigWindowState* state, const RECT& panelRc) {
	int x = panelRc.left;
	int w = panelRc.right - panelRc.left;
	int y = panelRc.top;
	const int rowH = 22;
	const int gap = 30;

	state->hwndLblName = CreateLabel(hwnd, hInst, L"NOMBRE DEL MENÚ", x, y, w, 18, IDC_LBL_NAME); y += 20;
	state->hwndEditName = ::CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
		x, y, w, rowH, hwnd, (HMENU)IDC_EDIT_NAME, hInst, nullptr); y += gap;

	state->hwndLblIcon = CreateLabel(hwnd, hInst, L"ÍCONO DEL MENÚ", x, y, w, 18, IDC_LBL_ICON); y += 20;
	state->hwndEditIcon = ::CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
		x, y, w - 122, rowH, hwnd, (HMENU)IDC_EDIT_ICON, hInst, nullptr);
	state->hwndIconPreview = ::CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
		x + w - 118, y - 1, rowH + 2, rowH + 2, hwnd, (HMENU)IDC_ICON_PREVIEW, hInst, nullptr);
	state->hwndBtnBrowseIcon = ::CreateWindow(L"BUTTON", L"Cambiar icono...", WS_CHILD | WS_VISIBLE,
		x + w - 85, y, 85, rowH, hwnd, (HMENU)IDC_BTN_BROWSE_ICON, hInst, nullptr); y += gap;

	state->hwndCheckMini = ::CreateWindow(L"BUTTON", L"Usar iconos pequeños (mini)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
		x, y, w, rowH, hwnd, (HMENU)IDC_CHECK_MINI, hInst, nullptr); y += gap;

	state->hwndLblTheme = CreateLabel(hwnd, hInst, L"COLOR DEL MENÚ", x, y, w, 18, IDC_LBL_THEME); y += 20;
	state->hwndComboTheme = ::CreateWindowEx(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
		x, y, w, 200, hwnd, (HMENU)IDC_COMBO_THEME, hInst, nullptr); y += gap;

	state->hwndLblPosition = CreateLabel(hwnd, hInst, L"POSICIÓN DEL MENÚ", x, y, w, 18, IDC_LBL_POSITION); y += 20;
	state->hwndCheckMousePos = ::CreateWindow(L"BUTTON", L"Abrir junto al cursor del mouse", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
		x, y, w, rowH, hwnd, (HMENU)IDC_CHECK_MOUSEPOS, hInst, nullptr); y += gap;

	state->hwndLblMode = CreateLabel(hwnd, hInst, L"MODO DE MENÚ", x, y, w, 18, IDC_LBL_MODE); y += 20;
	state->hwndComboMode = ::CreateWindowEx(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
		x, y, w, 200, hwnd, (HMENU)IDC_COMBO_MODE, hInst, nullptr); y += gap;

	state->hwndLblCols = CreateLabel(hwnd, hInst, L"COLUMNAS DE LA CUADRÍCULA", x, y, w, 18, IDC_LBL_COLS); y += 20;
	state->hwndEditCols = ::CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"3", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
		x, y, 60, rowH, hwnd, (HMENU)IDC_EDIT_COLS, hInst, nullptr); y += gap;

	state->hwndCheckNamesBelow = ::CreateWindow(L"BUTTON", L"Mostrar nombres debajo de los iconos", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
		x, y, w, rowH, hwnd, (HMENU)IDC_CHECK_NAMES_BELOW, hInst, nullptr); y += rowH + 4;
	state->hwndCheckNamesRight = ::CreateWindow(L"BUTTON", L"Mostrar nombres a la derecha de los iconos", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
		x, y, w, rowH, hwnd, (HMENU)IDC_CHECK_NAMES_RIGHT, hInst, nullptr); y += gap;

	state->hwndLblSort = CreateLabel(hwnd, hInst, L"ORDEN", x, y, w, 18, IDC_LBL_SORT); y += 20;
	state->hwndComboSort = ::CreateWindowEx(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
		x, y, w, 200, hwnd, (HMENU)IDC_COMBO_SORT, hInst, nullptr); y += gap;

	state->hwndCheckAddSeparator = ::CreateWindow(L"BUTTON",
		L"Agregar separador de submenús y accesos directos simples", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_MULTILINE,
		x, y, w, rowH * 2, hwnd, (HMENU)IDC_CHECK_ADD_SEPARATOR, hInst, nullptr); y += rowH * 2 + 8;

	// Submenu-only controls (share the same vertical band as some root
	// controls but are shown exclusively when a submenu node is selected).
	int ySub = panelRc.top;
	state->hwndLblSubIcon = CreateLabel(hwnd, hInst, L"ÍCONO DEL SUBMENÚ", x, ySub, w, 18, IDC_LBL_SUBICON); ySub += 20;
	state->hwndEditSubIcon = ::CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
		x, ySub, w - 122, rowH, hwnd, (HMENU)IDC_EDIT_SUBICON, hInst, nullptr);
	state->hwndSubIconPreview = ::CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
		x + w - 118, ySub - 1, rowH + 2, rowH + 2, hwnd, (HMENU)IDC_SUBICON_PREVIEW, hInst, nullptr);
	state->hwndBtnBrowseSubIcon = ::CreateWindow(L"BUTTON", L"Cambiar icono...", WS_CHILD | WS_VISIBLE,
		x + w - 85, ySub, 85, rowH, hwnd, (HMENU)IDC_BTN_BROWSE_SUBICON, hInst, nullptr); ySub += gap;

	state->hwndLblSubCols = CreateLabel(hwnd, hInst, L"COLUMNAS (SUBMENÚ ÚNICO)", x, ySub, w, 18, IDC_LBL_SUBCOLS); ySub += 20;
	state->hwndEditSubCols = ::CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"3", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
		x, ySub, 60, rowH, hwnd, (HMENU)IDC_EDIT_SUBCOLS, hInst, nullptr); ySub += gap;

	state->hwndLblSubLayout = CreateLabel(hwnd, hInst, L"DISPOSICIÓN DEL SUBMENÚ", x, ySub, w, 18, IDC_LBL_SUBLAYOUT); ySub += 20;
	state->hwndComboSubLayout = ::CreateWindowEx(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
		x, ySub, w, 200, hwnd, (HMENU)IDC_COMBO_SUBLAYOUT, hInst, nullptr); ySub += gap;

	// Capture the root-panel positions of hwndCheckMini/hwndCheckAddSeparator
	// (client coords relative to their parent) now, right after creation,
	// before ShowPanelFor() ever moves them for a submenu's panel.
	{
		RECT r;
		::GetWindowRect(state->hwndCheckMini, &r);
		POINT tl = {r.left, r.top};
		::ScreenToClient(hwnd, &tl);
		state->rcCheckMiniOrig = {tl.x, tl.y, tl.x + (r.right - r.left), tl.y + (r.bottom - r.top)};

		::GetWindowRect(state->hwndCheckAddSeparator, &r);
		tl = {r.left, r.top};
		::ScreenToClient(hwnd, &tl);
		state->rcCheckAddSeparatorOrig = {tl.x, tl.y, tl.x + (r.right - r.left), tl.y + (r.bottom - r.top)};
	}

	PopulateCombos(state);
}

// Hides every panel control; ShowPanelFor() then reveals the relevant subset.
void HideAllPanelControls(ConfigWindowState* state) {
	HWND all[] = {
		state->hwndLblName, state->hwndEditName,
		state->hwndLblIcon, state->hwndEditIcon, state->hwndBtnBrowseIcon, state->hwndIconPreview,
		state->hwndCheckMini,
		state->hwndLblTheme, state->hwndComboTheme,
		state->hwndLblPosition, state->hwndCheckMousePos,
		state->hwndLblMode, state->hwndComboMode,
		state->hwndLblSort, state->hwndComboSort,
		state->hwndCheckAddSeparator,
		state->hwndLblCols, state->hwndEditCols,
		state->hwndCheckNamesBelow, state->hwndCheckNamesRight,
		state->hwndLblSubIcon, state->hwndEditSubIcon, state->hwndBtnBrowseSubIcon, state->hwndSubIconPreview,
		state->hwndLblSubCols, state->hwndEditSubCols,
		state->hwndLblSubLayout, state->hwndComboSubLayout,
	};
	for (HWND h : all) if (h) ::ShowWindow(h, SW_HIDE);
	if (state->hwndRightPanel) ::ShowWindow(state->hwndRightPanel, SW_HIDE);
}

// Enables/disables the "Agregar separador de submenús y accesos directos
// simples" checkbox for a given effective mode and folder: only enabled in
// "Lista con submenús" (SCFG_MODE_DEFAULT), "Submenú único" (SCFG_MODE_SINGLESUB)
// and "Doble columna" (SCFG_MODE_DOUBLE_COL), only when the folder actually
// mixes subfolders (submenus) with plain shortcuts (a separator makes no
// sense if the folder has only one or the other), and only when the
// effective sort order (always driven by the top-level root's ORDEN, since
// --foldersfirst is a whole-menu setting) is NOT alphabetical: the automatic
// separator groups submenu folders before plain shortcuts, which only makes
// sense together with "Carpetas primero". isAlphabetical must be computed
// by the caller (live combo selection for the currently-visible root panel,
// or the persisted root's sort_mode for a submenu, since the ORDEN combo
// isn't shown/editable for submenus).
void UpdateAddSeparatorEnabled(ConfigWindowState* state, StackyConfigMode mode, const std::wstring& folderPath, bool isAlphabetical) {
	if (!state->hwndCheckAddSeparator) return;
	bool modeAllows = mode == SCFG_MODE_DEFAULT || mode == SCFG_MODE_SINGLESUB || mode == SCFG_MODE_DOUBLE_COL;
	bool hasSubfolder = false, hasPlainItem = false;
	if (!folderPath.empty()) FolderContentKind(folderPath, hasSubfolder, hasPlainItem);
	bool mixedContent = hasSubfolder && hasPlainItem;
	if (isAlphabetical) Button_SetCheck(state->hwndCheckAddSeparator, BST_UNCHECKED);
	::EnableWindow(state->hwndCheckAddSeparator, modeAllows && mixedContent && !isAlphabetical);
}

// Enables the grid-columns edit box only when "Cuadrícula de iconos" is the
// selected mode; other modes (list, double column/row, single submenu) don't
// use that field, so it's disabled to make this clear to the user. Also:
// - "Doble columna" (--iconmenu-C2) and "Doble fila" (--iconmenu-F2) don't
//   support showing names to the right of icons (only below), so that
//   checkbox is disabled in those modes.
// - "Lista con submenús" (default mode) and "Submenú único" (--singlesubmenu)
//   always show names next to the icons (like a regular submenu list) and
//   have no separate below/grid layout, so both name checkboxes are
//   disabled: "debajo" unchecked (not applicable) and "a la derecha" checked
//   (that's how the root menu already looks in both modes) but locked.
void UpdateGridColsEnabled(ConfigWindowState* state) {
	if (!state->hwndComboMode) return;
	int mode = ComboBox_GetCurSel(state->hwndComboMode);
	bool isIconGrid = mode == (int)SCFG_MODE_ICONGRID;
	bool isDoubleCol = mode == (int)SCFG_MODE_DOUBLE_COL;
	bool isDoubleRow = mode == (int)SCFG_MODE_DOUBLE_ROW;
	bool isDefault = mode == (int)SCFG_MODE_DEFAULT;
	bool isSingleSub = mode == (int)SCFG_MODE_SINGLESUB;
	bool namesLocked = isDefault || isSingleSub;

	if (state->hwndEditCols) ::EnableWindow(state->hwndEditCols, isIconGrid);

	if (state->hwndCheckNamesBelow) {
		::EnableWindow(state->hwndCheckNamesBelow, !namesLocked);
		if (namesLocked) Button_SetCheck(state->hwndCheckNamesBelow, BST_UNCHECKED);
	}
	if (state->hwndCheckNamesRight) {
		::EnableWindow(state->hwndCheckNamesRight, !isDoubleCol && !isDoubleRow && !namesLocked);
		if (isDoubleCol || isDoubleRow) Button_SetCheck(state->hwndCheckNamesRight, BST_UNCHECKED);
		else if (namesLocked) Button_SetCheck(state->hwndCheckNamesRight, BST_CHECKED);
	}

	// In "Cuadrícula de iconos" mode, "debajo" and "a la derecha" are
	// mutually exclusive: --iconmenu-NN-name and --iconmenu-NN-name-right
	// can't both apply at once. If both ended up checked (e.g. right after
	// switching into this mode from another one), keep "debajo" checked and
	// uncheck "a la derecha".
	if (isIconGrid && state->hwndCheckNamesBelow && state->hwndCheckNamesRight) {
		bool belowChecked = Button_GetCheck(state->hwndCheckNamesBelow) == BST_CHECKED;
		bool rightChecked = Button_GetCheck(state->hwndCheckNamesRight) == BST_CHECKED;
		if (belowChecked && rightChecked) {
			Button_SetCheck(state->hwndCheckNamesRight, BST_UNCHECKED);
		}
	}
}

// Loads a StackyFolderConfig into the currently-visible controls.
void ApplyConfigToControls(ConfigWindowState* state, const StackyFolderConfig& cfg, bool isRoot, const std::wstring& folderPath) {
	::SetWindowText(state->hwndEditName, cfg.menu_name.c_str());
	std::wstring menuIconPath = !cfg.menu_icon_path.empty() ? cfg.menu_icon_path : ResolveFolderIconPath(folderPath);
	::SetWindowText(state->hwndEditIcon, menuIconPath.c_str());
	SetPreviewIcon(state->hwndIconPreview, state->hIconPreview, menuIconPath, folderPath);
	Button_SetCheck(state->hwndCheckMini, cfg.mini_icons ? BST_CHECKED : BST_UNCHECKED);
	ComboBox_SetCurSel(state->hwndComboTheme, (int)cfg.theme);
	Button_SetCheck(state->hwndCheckMousePos, cfg.mouse_position ? BST_CHECKED : BST_UNCHECKED);
	ComboBox_SetCurSel(state->hwndComboMode, (int)cfg.mode);
	ComboBox_SetCurSel(state->hwndComboSort, (int)cfg.sort_mode);
	Button_SetCheck(state->hwndCheckAddSeparator, cfg.add_separator ? BST_CHECKED : BST_UNCHECKED);
	::SetWindowText(state->hwndEditCols, std::to_wstring(cfg.grid_cols).c_str());
	bool namesBelow;
	if (cfg.mode == SCFG_MODE_DOUBLE_COL) namesBelow = cfg.doublecol_names;
	else if (cfg.mode == SCFG_MODE_DOUBLE_ROW) namesBelow = cfg.doublerow_names;
	else namesBelow = cfg.grid_names_below;
	Button_SetCheck(state->hwndCheckNamesBelow, namesBelow ? BST_CHECKED : BST_UNCHECKED);
	Button_SetCheck(state->hwndCheckNamesRight, cfg.grid_names_right ? BST_CHECKED : BST_UNCHECKED);

	std::wstring subIconPath = !cfg.submenu_icon_path.empty() ? cfg.submenu_icon_path : ResolveFolderIconPath(folderPath);
	::SetWindowText(state->hwndEditSubIcon, subIconPath.c_str());
	SetPreviewIcon(state->hwndSubIconPreview, state->hSubIconPreview, subIconPath, folderPath);
	::SetWindowText(state->hwndEditSubCols, std::to_wstring(cfg.submenu_cols).c_str());
	ComboBox_SetCurSel(state->hwndComboSubLayout, (int)cfg.submenu_layout);

	UpdateGridColsEnabled(state);
}

// Reads current control values back into a StackyFolderConfig.
void ReadControlsIntoConfig(ConfigWindowState* state, StackyFolderConfig& cfg) {
	wchar_t buf[MAX_PATH];
	::GetWindowText(state->hwndEditName, buf, MAX_PATH); cfg.menu_name = buf;
	::GetWindowText(state->hwndEditIcon, buf, MAX_PATH); cfg.menu_icon_path = buf;
	cfg.mini_icons = Button_GetCheck(state->hwndCheckMini) == BST_CHECKED;
	cfg.theme = (StackyConfigTheme)ComboBox_GetCurSel(state->hwndComboTheme);
	cfg.mouse_position = Button_GetCheck(state->hwndCheckMousePos) == BST_CHECKED;
	cfg.mode = (StackyConfigMode)ComboBox_GetCurSel(state->hwndComboMode);
	cfg.sort_mode = (StackyConfigSort)ComboBox_GetCurSel(state->hwndComboSort);
	cfg.add_separator = Button_GetCheck(state->hwndCheckAddSeparator) == BST_CHECKED;
	::GetWindowText(state->hwndEditCols, buf, MAX_PATH); cfg.grid_cols = _wtoi(buf);
	bool namesBelowChecked = Button_GetCheck(state->hwndCheckNamesBelow) == BST_CHECKED;
	if (cfg.mode == SCFG_MODE_DOUBLE_COL) {
		cfg.doublecol_names = namesBelowChecked; // --iconmenu-C2-name
	} else if (cfg.mode == SCFG_MODE_DOUBLE_ROW) {
		cfg.doublerow_names = namesBelowChecked; // --iconmenu-F2-name
	} else {
		cfg.grid_names_below = namesBelowChecked;
	}
	cfg.grid_names_right = Button_GetCheck(state->hwndCheckNamesRight) == BST_CHECKED;

	::GetWindowText(state->hwndEditSubIcon, buf, MAX_PATH); cfg.submenu_icon_path = buf;
	::GetWindowText(state->hwndEditSubCols, buf, MAX_PATH); cfg.submenu_cols = _wtoi(buf);
	cfg.submenu_layout = (StackySubmenuLayout)ComboBox_GetCurSel(state->hwndComboSubLayout);
}

// Forward declarations (defined below SaveSelectedNodeConfig); needed here
// because ShowPanelFor uses IsMiniForcedByAncestor before its definition.
bool IsDescendantOf(ConfigWindowState* state, int candidateIndex, int ancestorIndex);
bool IsMiniForcedByAncestor(ConfigWindowState* state, int nodeIndex);

// Walks up from nodeIndex to the top-level ("root") ancestor folder and
// returns that root's configured mode. If nodeIndex is itself a root node,
// its own mode is returned. The root's mode decides which submenu-only
// fields are meaningful for its descendant subfolders: e.g. per-submenu
// "Columnas"/"Disposicion del submenu" only apply under "Submenu unico"
// (--singlesubmenu); under the default list-with-submenus mode every
// submenu just renders as a plain list, so those fields don't apply.
StackyConfigMode RootModeFor(ConfigWindowState* state, int nodeIndex) {
	int idx = nodeIndex;
	while (!state->nodes[idx].isRoot && state->nodes[idx].parentIndex >= 0) {
		idx = state->nodes[idx].parentIndex;
	}
	StackyFolderConfig rootCfg = StackyFolderConfig::Load(state->nodes[idx].fullPath);
	return rootCfg.mode;
}

// Same idea as RootModeFor(), but returns the top-level root's sort_mode:
// --foldersfirst / the automatic separator are whole-menu settings driven
// solely by the root folder's ORDEN, so a submenu's "add_separator"
// enablement must always be gated on the root's sort order, not any
// per-submenu setting (submenus have no ORDEN control of their own).
bool RootIsAlphabeticalFor(ConfigWindowState* state, int nodeIndex) {
	int idx = nodeIndex;
	while (!state->nodes[idx].isRoot && state->nodes[idx].parentIndex >= 0) {
		idx = state->nodes[idx].parentIndex;
	}
	StackyFolderConfig rootCfg = StackyFolderConfig::Load(state->nodes[idx].fullPath);
	return rootCfg.sort_mode == SCFG_SORT_ALPHA;
}

// Shows the appropriate subset of controls for the given node type and
// populates them from that folder's .stacky-config (if any).
void ShowPanelFor(ConfigWindowState* state, int nodeIndex) {
	HideAllPanelControls(state);
	if (nodeIndex < 0 || nodeIndex >= (int)state->nodes.size()) {
		if (state->hwndRightPanel) ::ShowWindow(state->hwndRightPanel, SW_SHOW);
		return;
	}

	const TreeNodeInfo& info = state->nodes[nodeIndex];
	StackyFolderConfig cfg = StackyFolderConfig::Load(info.fullPath);

	if (info.isRoot) {
		HWND rootControls[] = {
			state->hwndLblName, state->hwndEditName,
			state->hwndLblIcon, state->hwndEditIcon, state->hwndBtnBrowseIcon, state->hwndIconPreview,
			state->hwndCheckMini,
			state->hwndLblTheme, state->hwndComboTheme,
			state->hwndLblPosition, state->hwndCheckMousePos,
			state->hwndLblMode, state->hwndComboMode,
			state->hwndLblSort, state->hwndComboSort,
			state->hwndCheckAddSeparator,
			state->hwndLblCols, state->hwndEditCols,
			state->hwndCheckNamesBelow, state->hwndCheckNamesRight,
		};
		for (HWND h : rootControls) if (h) ::ShowWindow(h, SW_SHOW);

		// hwndCheckMini/hwndCheckAddSeparator may have been moved down by a
		// previous submenu panel display (see the "else" branch below);
		// restore their original root-panel positions (below ORDEN) here.
		if (state->hwndCheckMini) {
			const RECT& r = state->rcCheckMiniOrig;
			::SetWindowPos(state->hwndCheckMini, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
				SWP_NOZORDER | SWP_NOACTIVATE);
		}
		if (state->hwndCheckAddSeparator) {
			const RECT& r = state->rcCheckAddSeparatorOrig;
			::SetWindowPos(state->hwndCheckAddSeparator, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
				SWP_NOZORDER | SWP_NOACTIVATE);
		}

		UpdateAddSeparatorEnabled(state, cfg.mode, info.fullPath, cfg.sort_mode == SCFG_SORT_ALPHA);
	} else {
		HWND subControls[] = {
			state->hwndCheckMini,
			state->hwndCheckAddSeparator,
			state->hwndLblSubIcon, state->hwndEditSubIcon, state->hwndBtnBrowseSubIcon, state->hwndSubIconPreview,
			state->hwndLblSubCols, state->hwndEditSubCols,
			state->hwndLblSubLayout, state->hwndComboSubLayout,
		};
		for (HWND h : subControls) if (h) ::ShowWindow(h, SW_SHOW);

		// The submenu-only "Columnas" and "Disposicion del submenu" fields
		// only make sense when the top-level (root) folder's mode is
		// "Submenu unico" (--singlesubmenu); under any other root mode
		// (e.g. the default list-with-submenus) every submenu just renders
		// as a plain list, so those fields are disabled to make that clear.
		bool rootIsSingleSub = RootModeFor(state, nodeIndex) == SCFG_MODE_SINGLESUB;
		if (state->hwndEditSubCols) ::EnableWindow(state->hwndEditSubCols, rootIsSingleSub);
		if (state->hwndComboSubLayout) ::EnableWindow(state->hwndComboSubLayout, rootIsSingleSub);

		// "ICONO DEL SUBMENU" only makes sense for a submenu that can
		// actually be selected/used as such (see RefreshDisabledStates):
		// disabled here mirrors info.disabled as an extra safety net, since
		// TVN_SELCHANGING already blocks the user from selecting a disabled
		// node in the tree.
		bool subIconEnabled = !info.disabled;
		if (state->hwndEditSubIcon) ::EnableWindow(state->hwndEditSubIcon, subIconEnabled);
		if (state->hwndBtnBrowseSubIcon) ::EnableWindow(state->hwndBtnBrowseSubIcon, subIconEnabled);

		// If an ancestor submenu already has "mini" enabled, it propagates
		// down to every descendant, so lock the checkbox checked here to
		// make clear it can't be turned off for this folder individually.
		bool miniForced = IsMiniForcedByAncestor(state, nodeIndex);
		::EnableWindow(state->hwndCheckMini, miniForced ? FALSE : TRUE);
		if (miniForced) cfg.mini_icons = true;

		// hwndCheckMini's original position (set in CreateRightPanelControls)
		// sits within the root panel's vertical band, which overlaps the
		// submenu-only fields above (icon/columns/layout). Move it below
		// the submenu layout combo so it's visible and doesn't overlap.
		RECT rc;
		::GetWindowRect(state->hwndComboSubLayout, &rc);
		POINT topLeft = {rc.left, rc.bottom};
		HWND parent = ::GetParent(state->hwndComboSubLayout);
		::ScreenToClient(parent, &topLeft);
		RECT selfRc;
		::GetWindowRect(state->hwndCheckMini, &selfRc);
		int width = selfRc.right - selfRc.left;
		int height = selfRc.bottom - selfRc.top;
		::SetWindowPos(state->hwndCheckMini, nullptr, topLeft.x, topLeft.y + 8, width, height,
			SWP_NOZORDER | SWP_NOACTIVATE);

		// Move the separator checkbox right below "mini" (same reasoning as
		// above: its original position sits within the root panel's band).
		RECT miniRc;
		::GetWindowRect(state->hwndCheckMini, &miniRc);
		POINT sepTopLeft = {miniRc.left, miniRc.bottom + 4};
		::ScreenToClient(parent, &sepTopLeft);
		RECT sepRc;
		::GetWindowRect(state->hwndCheckAddSeparator, &sepRc);
		int sepWidth = sepRc.right - sepRc.left;
		int sepHeight = sepRc.bottom - sepRc.top;
		::SetWindowPos(state->hwndCheckAddSeparator, nullptr, sepTopLeft.x, sepTopLeft.y, sepWidth, sepHeight,
			SWP_NOZORDER | SWP_NOACTIVATE);

		UpdateAddSeparatorEnabled(state, RootModeFor(state, nodeIndex), info.fullPath, RootIsAlphabeticalFor(state, nodeIndex));
	}

	state->populatingControls = true;
	ApplyConfigToControls(state, cfg, info.isRoot, info.fullPath);
	state->populatingControls = false;
}

// Returns true if candidateIndex is a (direct or indirect) descendant of
// ancestorIndex, by walking up the parentIndex chain.
bool IsDescendantOf(ConfigWindowState* state, int candidateIndex, int ancestorIndex) {
	int p = state->nodes[candidateIndex].parentIndex;
	while (p >= 0) {
		if (p == ancestorIndex) return true;
		p = state->nodes[p].parentIndex;
	}
	return false;
}

// Returns true if any ancestor folder (submenu parent) of nodeIndex has
// "mini_icons" enabled in its .stacky-config, which forces this node (and
// all of its own descendants) to use mini icons too.
bool IsMiniForcedByAncestor(ConfigWindowState* state, int nodeIndex) {
	int p = state->nodes[nodeIndex].parentIndex;
	while (p >= 0) {
		StackyFolderConfig parentCfg = StackyFolderConfig::Load(state->nodes[p].fullPath);
		if (parentCfg.mini_icons) return true;
		p = state->nodes[p].parentIndex;
	}
	return false;
}

// When a submenu is set to use mini icons, all of its nested submenus should
// automatically use mini icons too, so the setting doesn't have to be
// re-applied by hand on every descendant folder.
void PropagateMiniToDescendants(ConfigWindowState* state, int nodeIndex) {
	for (int i = 0; i < (int)state->nodes.size(); ++i) {
		if (i == nodeIndex) continue;
		if (!IsDescendantOf(state, i, nodeIndex)) continue;
		StackyFolderConfig childCfg = StackyFolderConfig::Load(state->nodes[i].fullPath);
		if (childCfg.mini_icons) continue; // already mini, nothing to change/save
		childCfg.mini_icons = true;
		childCfg.Save(state->nodes[i].fullPath);
	}
}

// When the root menu's ORDEN is switched to "Alfabético", the automatic
// separator ("add_separator") is meaningless for every descendant submenu
// too (--foldersfirst/the separator grouping are whole-menu settings driven
// solely by the root), so clear it in each descendant's own
// .stacky-config so a later visit to that submenu shows the checkbox
// unchecked/disabled and the runtime stops drawing its separator.
void PropagateAlphabeticalToDescendants(ConfigWindowState* state, int nodeIndex) {
	for (int i = 0; i < (int)state->nodes.size(); ++i) {
		if (i == nodeIndex) continue;
		if (!IsDescendantOf(state, i, nodeIndex)) continue;
		StackyFolderConfig childCfg = StackyFolderConfig::Load(state->nodes[i].fullPath);
		if (!childCfg.add_separator) continue; // nothing to change/save
		childCfg.add_separator = false;
		childCfg.Save(state->nodes[i].fullPath);
	}
}

// Applies (or clears) a custom folder icon in Windows Explorer for
// folderPath by writing/updating its hidden "desktop.ini". iconSpec is in
// "path,index" form (same as menu_icon_path/submenu_icon_path); if empty,
// any existing custom icon entry is removed so Explorer falls back to the
// default folder icon. Also toggles the FILE_ATTRIBUTE_SYSTEM flag on the
// folder itself, which Explorer requires to honor a folder-level
// desktop.ini icon override, and notifies the shell so open Explorer
// windows refresh the icon without needing a restart.
void ApplyIconToExplorerFolder(const std::wstring& folderPath, const std::wstring& iconSpec) {
	if (folderPath.empty()) return;
	std::wstring iniPath = folderPath + L"desktop.ini";

	if (iconSpec.empty()) {
		// Remove the custom icon entry (if any) and restore normal attributes.
		if (::GetFileAttributes(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
			::SetFileAttributes(iniPath.c_str(), FILE_ATTRIBUTE_NORMAL);
			::WritePrivateProfileString(L".ShellClassInfo", L"IconResource", nullptr, iniPath.c_str());
			::WritePrivateProfileString(L".ShellClassInfo", L"IconFile", nullptr, iniPath.c_str());
			::WritePrivateProfileString(L".ShellClassInfo", nullptr, nullptr, iniPath.c_str()); // flush cached handle
			// If the file ended up with no other content, delete it outright.
			wchar_t probe[8] = {0};
			::GetPrivateProfileString(L".ShellClassInfo", L"IconIndex", L"", probe, 8, iniPath.c_str());
			::DeleteFile(iniPath.c_str());
		}
		DWORD attrs = ::GetFileAttributes(folderPath.c_str());
		if (attrs != INVALID_FILE_ATTRIBUTES) {
			::SetFileAttributes(folderPath.c_str(), attrs & ~FILE_ATTRIBUTE_SYSTEM);
		}
	} else {
		std::wstring path = iconSpec;
		int index = 0;
		size_t comma = path.find_last_of(L',');
		if (comma != std::wstring::npos) {
			index = _wtoi(path.c_str() + comma + 1);
			path = path.substr(0, comma);
		}
		// desktop.ini may already exist and be hidden/system; clear those
		// attributes first so WritePrivateProfileString can update it.
		::SetFileAttributes(iniPath.c_str(), FILE_ATTRIBUTE_NORMAL);
		::WritePrivateProfileString(L".ShellClassInfo", L"IconResource",
			(path + L"," + std::to_wstring(index)).c_str(), iniPath.c_str());
		::WritePrivateProfileString(L".ShellClassInfo", L"IconFile", path.c_str(), iniPath.c_str());
		::WritePrivateProfileString(L".ShellClassInfo", L"IconIndex", std::to_wstring(index).c_str(), iniPath.c_str());
		::SetFileAttributes(iniPath.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);

		DWORD attrs = ::GetFileAttributes(folderPath.c_str());
		if (attrs != INVALID_FILE_ATTRIBUTES) {
			::SetFileAttributes(folderPath.c_str(), attrs | FILE_ATTRIBUTE_SYSTEM);
		}
	}

	::SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, folderPath.c_str(), nullptr);
}

// Deletes the ".stacky-config" hidden file (if present) for a single
// folder path.
void DeleteStackyConfigFile(const std::wstring& folderPath) {
	std::wstring cfgPath = folderPath + STACKY_CONFIG_FILE_NAME;
	::SetFileAttributes(cfgPath.c_str(), FILE_ATTRIBUTE_NORMAL);
	::DeleteFile(cfgPath.c_str());
}

// When a menu's launcher shortcut (.lnk) is removed (either explicitly via
// "Eliminar menú" or because the user deleted it by hand in Explorer), the
// per-folder state that only made sense while the menu existed - the root's
// cached scan ("!stacky.cache") and every ".stacky-config" file for that
// root folder and all its descendant submenus - is no longer meaningful and
// is deleted immediately so a future re-creation of the menu starts clean.
void DeleteMenuArtifactsForFolder(ConfigWindowState* state, int rootIndex) {
	if (rootIndex < 0 || rootIndex >= (int)state->nodes.size()) return;
	const std::wstring& rootPath = state->nodes[rootIndex].fullPath;

	std::wstring cachePath = rootPath + L"!stacky.cache";
	::SetFileAttributes(cachePath.c_str(), FILE_ATTRIBUTE_NORMAL);
	::DeleteFile(cachePath.c_str());

	DeleteStackyConfigFile(rootPath);
	for (int i = 0; i < (int)state->nodes.size(); ++i) {
		if (i == rootIndex) continue;
		if (!IsDescendantOf(state, i, rootIndex)) continue;
		DeleteStackyConfigFile(state->nodes[i].fullPath);
	}
}

// Updates TreeNodeInfo::hasShortcut for every root node by checking whether
// a .lnk currently exists that targets it, then forces the tree to repaint
// so the green-dot indicator reflects the current state. Root folders whose
// shortcut has disappeared (e.g. deleted manually in Explorer) also get
// their leftover ".stacky-config"/"!stacky.cache" cleaned up here, so stale
// per-folder settings don't linger once the menu no longer exists.
void RefreshShortcutIndicators(ConfigWindowState* state) {
	for (int i = 0; i < (int)state->nodes.size(); ++i) {
		if (!state->nodes[i].isRoot) continue;
		std::wstring lnk = FindShortcutForFolder(state->exeFolder, state->nodes[i].fullPath);
		state->nodes[i].hasShortcut = !lnk.empty();
		if (lnk.empty()) {
			// No shortcut currently exists for this root folder (either it
			// was never created, or it was deleted - e.g. manually in
			// Explorer). If leftover config/cache exists, clean it up.
			std::wstring cfgPath = state->nodes[i].fullPath + STACKY_CONFIG_FILE_NAME;
			std::wstring cachePath = state->nodes[i].fullPath + L"!stacky.cache";
			bool hasCfg = ::GetFileAttributes(cfgPath.c_str()) != INVALID_FILE_ATTRIBUTES;
			bool hasCache = ::GetFileAttributes(cachePath.c_str()) != INVALID_FILE_ATTRIBUTES;
			if (hasCfg || hasCache) {
				DeleteMenuArtifactsForFolder(state, i);
			}
		}
	}
	if (state->hwndTree) ::InvalidateRect(state->hwndTree, nullptr, TRUE);
}

// Persists the currently-visible panel's values into the selected folder's
// .stacky-config file.
void SaveSelectedNodeConfig(ConfigWindowState* state) {
	if (state->selectedNode < 0 || state->selectedNode >= (int)state->nodes.size()) return;
	const TreeNodeInfo& info = state->nodes[state->selectedNode];
	StackyFolderConfig cfg = StackyFolderConfig::Load(info.fullPath);
	ReadControlsIntoConfig(state, cfg);
	cfg.Save(info.fullPath);

	// Apply the chosen icon (or clear it) to the folder in Windows Explorer
	// too, so the menu icon and the folder's icon in File Explorer stay in
	// sync. Root nodes use menu_icon_path, submenus use submenu_icon_path.
	ApplyIconToExplorerFolder(info.fullPath, info.isRoot ? cfg.menu_icon_path : cfg.submenu_icon_path);

	// The icon path fields are freely editable (not just via "Cambiar
	// icono..."), so refresh the preview icons here too in case the user
	// typed/pasted a new path/index directly into the edit box.
	if (info.isRoot) {
		SetPreviewIcon(state->hwndIconPreview, state->hIconPreview, cfg.menu_icon_path, info.fullPath);
	} else {
		SetPreviewIcon(state->hwndSubIconPreview, state->hSubIconPreview, cfg.submenu_icon_path, info.fullPath);
	}

	// Whether this is the root (main menu) or a submenu, enabling mini icons
	// propagates down to every nested submenu below it.
	if (cfg.mini_icons) {
		PropagateMiniToDescendants(state, state->selectedNode);
	}

	// stacky.cpp caches folder scans (icons, submenu/mini flags, etc.) in a
	// hidden "!stacky.cache" file at the root menu folder. Since
	// ".stacky-config" is itself a hidden file, editing it here doesn't
	// touch the root folder's own modification time, so the cache's
	// staleness check never notices the change. Simply deleting the cache
	// file isn't enough either: Cache::load()/rebuild() only run lazily the
	// next time stacky-plus.exe scans the folder, so the very first menu
	// open right after a change could still render without icons (as if
	// built from a stale/incomplete scan) - only the second open picked up
	// the fresh config. Rebuild the cache synchronously right here instead,
	// so it's already correct and complete on disk before the menu is ever
	// opened again.
	int rootIdx = state->selectedNode;
	while (!state->nodes[rootIdx].isRoot && state->nodes[rootIdx].parentIndex >= 0) {
		rootIdx = state->nodes[rootIdx].parentIndex;
	}
	RebuildStackyCache(state->nodes[rootIdx].fullPath);

	// The folder's mode may have just changed (e.g. from a grid layout that
	// disallows submenus to the default list-with-submenus mode, or vice
	// versa), which affects whether its subfolders can act as selectable
	// submenus. Recompute that immediately instead of requiring the window
	// to be closed and reopened.
	RefreshDisabledStates(state);
}

LRESULT CALLBACK ConfigWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	ConfigWindowState* state = reinterpret_cast<ConfigWindowState*>(
		::GetWindowLongPtr(hwnd, GWLP_USERDATA));

	switch (msg) {
	case WM_CREATE: {
		state = new ConfigWindowState();
		::SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
		state->exeFolder = GetExeFolderPath();

		RECT rc;
		::GetClientRect(hwnd, &rc);

		state->hwndTree = ::CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, L"",
			WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_TRACKSELECT,
			10, 10, 220, rc.bottom - 100, hwnd, (HMENU)IDC_TREE, ::GetModuleHandle(nullptr), nullptr);
		state->accentColor = ReadAccentColor();

		state->hwndRightPanel = ::CreateWindowEx(0, L"STATIC", L"Seleccione un menú a la izquierda.",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			240, 10, rc.right - 250, rc.bottom - 100, hwnd, nullptr, ::GetModuleHandle(nullptr), nullptr);

		RECT panelRc = {240, 10, rc.right - 10, rc.bottom - 100};
		CreateRightPanelControls(hwnd, ::GetModuleHandle(nullptr), state, panelRc);
		HideAllPanelControls(state);
		::ShowWindow(state->hwndRightPanel, SW_SHOW);

		state->hwndBtnSave = ::CreateWindow(L"BUTTON", L"Guardar",
			WS_CHILD | WS_VISIBLE, 10, rc.bottom - 40, 100, 28, hwnd, (HMENU)IDC_BTN_SAVE, ::GetModuleHandle(nullptr), nullptr);
		state->hwndBtnCreate = ::CreateWindow(L"BUTTON", L"Crear menú",
			WS_CHILD | WS_VISIBLE, 120, rc.bottom - 40, 120, 28, hwnd, (HMENU)IDC_BTN_CREATE, ::GetModuleHandle(nullptr), nullptr);
		state->hwndBtnDelete = ::CreateWindow(L"BUTTON", L"Eliminar menú",
			WS_CHILD | WS_VISIBLE, 250, rc.bottom - 40, 120, 28, hwnd, (HMENU)IDC_BTN_DELETE, ::GetModuleHandle(nullptr), nullptr);
		state->hwndBtnCancel = ::CreateWindow(L"BUTTON", L"Cerrar",
			WS_CHILD | WS_VISIBLE, 380, rc.bottom - 40, 100, 28, hwnd, (HMENU)IDC_BTN_CANCEL, ::GetModuleHandle(nullptr), nullptr);

		PopulateTreeFolder(state->hwndTree, TVI_ROOT, state->exeFolder, state->nodes, true, -1);
		RefreshShortcutIndicators(state);

		// Give every child control (labels, edits, combos, checkboxes,
		// buttons, tree view) the standard system UI font instead of the
		// legacy default GDI font, matching the look of native Windows
		// dialogs.
		ApplyDefaultFontToChildren(hwnd);
		return 0;
	}
	case WM_NOTIFY: {
		NMHDR* nmhdr = reinterpret_cast<NMHDR*>(lParam);
		if (nmhdr->hwndFrom == state->hwndTree && nmhdr->code == TVN_SELCHANGING) {
			NMTREEVIEW* nmtv = reinterpret_cast<NMTREEVIEW*>(lParam);
			int newIdx = (int)nmtv->itemNew.lParam;
			if (nmtv->itemNew.hItem != nullptr && newIdx >= 0 && newIdx < (int)state->nodes.size() &&
				state->nodes[newIdx].disabled) {
				// Block selecting folders that can't act as submenus because
				// their parent uses a flat grid layout (icon grid / double
				// column / double row).
				::SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
				return TRUE;
			}
			return 0;
		}
		if (nmhdr->hwndFrom == state->hwndTree && nmhdr->code == TVN_SELCHANGED) {
			// Persist the previously-selected node before switching, so edits
			// are not silently lost when navigating the tree.
			if (state->selectedNode >= 0) SaveSelectedNodeConfig(state);

			NMTREEVIEW* nmtv = reinterpret_cast<NMTREEVIEW*>(lParam);
			int idx = (int)nmtv->itemNew.lParam;
			state->selectedNode = (nmtv->itemNew.hItem != nullptr) ? idx : -1;
			ShowPanelFor(state, state->selectedNode);
			return 0;
		}
		if (nmhdr->hwndFrom == state->hwndTree && nmhdr->code == NM_CUSTOMDRAW) {
			NMTVCUSTOMDRAW* cd = reinterpret_cast<NMTVCUSTOMDRAW*>(lParam);
			switch (cd->nmcd.dwDrawStage) {
			case CDDS_PREPAINT:
				::SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
				return CDRF_NOTIFYITEMDRAW;
			case CDDS_ITEMPREPAINT: {
				HTREEITEM item = reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec);
				UINT itemState = TreeView_GetItemState(state->hwndTree, item, TVIS_SELECTED | TVIS_DROPHILITED);
				bool isHot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
				bool isSelected = (itemState & TVIS_SELECTED) != 0;

				int idx = -1;
				TVITEM tvItem = {0};
				tvItem.mask = TVIF_PARAM;
				tvItem.hItem = item;
				if (TreeView_GetItem(state->hwndTree, &tvItem)) idx = (int)tvItem.lParam;
				bool isDisabled = idx >= 0 && idx < (int)state->nodes.size() && state->nodes[idx].disabled;

				if (isDisabled) {
					cd->clrText = ::GetSysColor(COLOR_GRAYTEXT);
				} else if (isSelected || isHot) {
					cd->clrTextBk = state->accentColor;
					cd->clrText = RGB(255, 255, 255);
				}
				::SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT);
				return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
			}
			case CDDS_ITEMPOSTPAINT: {
				// Draw a small green dot to the right of the item text for
				// root folders that currently have their launcher .lnk
				// shortcut created, so the user can see at a glance which
				// menus already exist.
				HTREEITEM item = reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec);
				TVITEM tvItem = {0};
				tvItem.mask = TVIF_PARAM;
				tvItem.hItem = item;
				int idx = -1;
				if (TreeView_GetItem(state->hwndTree, &tvItem)) idx = (int)tvItem.lParam;
				if (idx >= 0 && idx < (int)state->nodes.size() &&
					state->nodes[idx].isRoot && state->nodes[idx].hasShortcut) {
					RECT textRc = {0};
					TreeView_GetItemRect(state->hwndTree, item, &textRc, TRUE);
					int diameter = 8;
					int cx = textRc.right + 8;
					int cy = (textRc.top + textRc.bottom) / 2 - diameter / 2;
					HDC hdc = cd->nmcd.hdc;
					HBRUSH brush = ::CreateSolidBrush(RGB(40, 170, 60));
					HBRUSH oldBrush = (HBRUSH)::SelectObject(hdc, brush);
					HPEN pen = ::CreatePen(PS_SOLID, 1, RGB(30, 130, 45));
					HPEN oldPen = (HPEN)::SelectObject(hdc, pen);
					::Ellipse(hdc, cx, cy, cx + diameter, cy + diameter);
					::SelectObject(hdc, oldBrush);
					::SelectObject(hdc, oldPen);
					::DeleteObject(brush);
					::DeleteObject(pen);
				}
				::SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_DODEFAULT);
				return CDRF_DODEFAULT;
			}
			}
		}
		break;
	}
	case WM_COMMAND: {
		if (state->populatingControls) return 0; // ignore notifications from programmatic control updates
		if (HIWORD(wParam) == CBN_SELCHANGE && (HWND)lParam == state->hwndComboMode) {
			UpdateGridColsEnabled(state);
			if (state->selectedNode >= 0 && state->selectedNode < (int)state->nodes.size()) {
				int mode = ComboBox_GetCurSel(state->hwndComboMode);
				int sortSel = state->hwndComboSort ? ComboBox_GetCurSel(state->hwndComboSort) : CB_ERR;
				bool isAlphabetical = (sortSel == (int)SCFG_SORT_ALPHA) || (sortSel == CB_ERR);
				UpdateAddSeparatorEnabled(state, (StackyConfigMode)mode, state->nodes[state->selectedNode].fullPath, isAlphabetical);
			}
			SaveSelectedNodeConfig(state);
			return 0;
		}
		if (HIWORD(wParam) == CBN_SELCHANGE && (HWND)lParam == state->hwndComboSort) {
			if (state->selectedNode >= 0 && state->selectedNode < (int)state->nodes.size()) {
				StackyConfigMode mode = state->hwndComboMode
					? (StackyConfigMode)ComboBox_GetCurSel(state->hwndComboMode)
					: RootModeFor(state, state->selectedNode);
				int sortSel = ComboBox_GetCurSel(state->hwndComboSort);
				bool isAlphabetical = (sortSel == (int)SCFG_SORT_ALPHA) || (sortSel == CB_ERR);
				UpdateAddSeparatorEnabled(state, mode, state->nodes[state->selectedNode].fullPath, isAlphabetical);
			}
			SaveSelectedNodeConfig(state);

			// The root's sort order is a whole-menu setting: switching to
			// alphabetical must propagate to every descendant submenu so their
			// own persisted "add_separator" is cleared too (otherwise a submenu
			// that had the separator enabled while "Carpetas primero" was active
			// would keep drawing it even though the root is now alphabetical,
			// and its checkbox would appear checked-but-disabled if revisited).
			if (state->selectedNode >= 0 && state->selectedNode < (int)state->nodes.size() &&
				state->nodes[state->selectedNode].isRoot) {
				int sortSel = ComboBox_GetCurSel(state->hwndComboSort);
				if (sortSel == (int)SCFG_SORT_ALPHA) {
					PropagateAlphabeticalToDescendants(state, state->selectedNode);
				}
			}
			return 0;
		}
		switch (LOWORD(wParam)) {
		case IDC_BTN_CANCEL:
			::DestroyWindow(hwnd);
			return 0;
		case IDC_BTN_SAVE:
			SaveSelectedNodeConfig(state);
			return 0;
		case IDC_BTN_CREATE:
			SaveSelectedNodeConfig(state);
			if (state->selectedNode >= 0 && state->selectedNode < (int)state->nodes.size() &&
				state->nodes[state->selectedNode].isRoot) {
				if (CreateShortcutForNode(hwnd, state->exeFolder, state->nodes[state->selectedNode])) {
					::MessageBox(hwnd, L"Acceso directo del menú creado correctamente.", L"Stacky", MB_OK | MB_ICONINFORMATION);
					RefreshShortcutIndicators(state);
				}
			} else {
				::MessageBox(hwnd, L"Seleccione una carpeta raíz para crear su menú.", L"Stacky", MB_OK | MB_ICONWARNING);
			}
			return 0;
		case IDC_BTN_DELETE: {
			if (state->selectedNode < 0 || state->selectedNode >= (int)state->nodes.size() ||
				!state->nodes[state->selectedNode].isRoot) {
				::MessageBox(hwnd, L"Seleccione una carpeta raíz cuyo menú desee eliminar.", L"Stacky", MB_OK | MB_ICONWARNING);
				return 0;
			}
			int rootIdx = state->selectedNode;
			std::wstring lnkPath = FindShortcutForFolder(state->exeFolder, state->nodes[rootIdx].fullPath);
			if (lnkPath.empty()) {
				::MessageBox(hwnd, L"Este menú no tiene un acceso directo creado.", L"Stacky", MB_OK | MB_ICONWARNING);
				return 0;
			}
			int answer = ::MessageBox(hwnd,
				L"¿Eliminar el acceso directo de este menú? También se borrarán su cache y la configuración guardada para esta carpeta y sus subcarpetas.",
				L"Stacky", MB_YESNO | MB_ICONQUESTION);
			if (answer == IDYES) {
				::SetFileAttributes(lnkPath.c_str(), FILE_ATTRIBUTE_NORMAL);
				::DeleteFile(lnkPath.c_str());
				DeleteMenuArtifactsForFolder(state, rootIdx);
				RefreshShortcutIndicators(state);
				::MessageBox(hwnd, L"Menú eliminado correctamente.", L"Stacky", MB_OK | MB_ICONINFORMATION);
			}
			return 0;
		}
		case IDC_BTN_BROWSE_ICON: {
			wchar_t buf[MAX_PATH] = {0};
			::GetWindowText(state->hwndEditIcon, buf, MAX_PATH);
			std::wstring path = buf;
			if (PickIcon(hwnd, path)) {
				::SetWindowText(state->hwndEditIcon, path.c_str());
				std::wstring folderPath = (state->selectedNode >= 0 && state->selectedNode < (int)state->nodes.size())
					? state->nodes[state->selectedNode].fullPath : std::wstring();
				SetPreviewIcon(state->hwndIconPreview, state->hIconPreview, path, folderPath);
				SaveSelectedNodeConfig(state);
			}
			return 0;
		}
		case IDC_BTN_BROWSE_SUBICON: {
			wchar_t buf[MAX_PATH] = {0};
			::GetWindowText(state->hwndEditSubIcon, buf, MAX_PATH);
			std::wstring path = buf;
			if (PickIcon(hwnd, path)) {
				::SetWindowText(state->hwndEditSubIcon, path.c_str());
				std::wstring folderPath = (state->selectedNode >= 0 && state->selectedNode < (int)state->nodes.size())
					? state->nodes[state->selectedNode].fullPath : std::wstring();
				SetPreviewIcon(state->hwndSubIconPreview, state->hSubIconPreview, path, folderPath);
				SaveSelectedNodeConfig(state);
			}
			return 0;
		}
		// Any other change notification coming from a panel control (text
		// edits, checkboxes, combo boxes) is persisted immediately, so
		// edits to an already-created menu take effect on the fly instead
		// of requiring an explicit "Guardar" click.
		case IDC_CHECK_NAMES_BELOW:
		case IDC_CHECK_NAMES_RIGHT:
			if (HIWORD(wParam) == BN_CLICKED) {
				// In "Cuadrícula de iconos" mode, "debajo" (--iconmenu-NN-name)
				// and "a la derecha" (--iconmenu-NN-name-right) are mutually
				// exclusive: checking one unchecks the other, and both can be
				// left unchecked to fall back to plain --iconmenu-NN.
				int mode = state->hwndComboMode ? ComboBox_GetCurSel(state->hwndComboMode) : -1;
				if (mode == (int)SCFG_MODE_ICONGRID) {
					if (LOWORD(wParam) == IDC_CHECK_NAMES_BELOW &&
						Button_GetCheck(state->hwndCheckNamesBelow) == BST_CHECKED) {
						Button_SetCheck(state->hwndCheckNamesRight, BST_UNCHECKED);
					} else if (LOWORD(wParam) == IDC_CHECK_NAMES_RIGHT &&
						Button_GetCheck(state->hwndCheckNamesRight) == BST_CHECKED) {
						Button_SetCheck(state->hwndCheckNamesBelow, BST_UNCHECKED);
					}
				}
				SaveSelectedNodeConfig(state);
			}
			return 0;
		case IDC_EDIT_NAME:
		case IDC_EDIT_ICON:
		case IDC_CHECK_MINI:
		case IDC_CHECK_ADD_SEPARATOR:
		case IDC_COMBO_THEME:
		case IDC_CHECK_MOUSEPOS:
		case IDC_COMBO_SORT:
		case IDC_EDIT_COLS:
		case IDC_EDIT_SUBICON:
		case IDC_EDIT_SUBCOLS:
		case IDC_COMBO_SUBLAYOUT:
			if (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == BN_CLICKED ||
				HIWORD(wParam) == CBN_SELCHANGE) {
				SaveSelectedNodeConfig(state);
			}
			return 0;
		}
		break;
	}
	case WM_DESTROY: {
		if (state) {
			if (state->hIconPreview) ::DestroyIcon(state->hIconPreview);
			if (state->hSubIconPreview) ::DestroyIcon(state->hSubIconPreview);
		}
		delete state;
		::SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
		::PostQuitMessage(0);
		return 0;
	}
	}
	return ::DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace

int RunStackyConfigWindow(HINSTANCE hInstance) {
	INITCOMMONCONTROLSEX icc = {0};
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
	::InitCommonControlsEx(&icc);

	WNDCLASS wc = {0};
	wc.lpfnWndProc = ConfigWndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = kConfigWndClass;
	wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	::RegisterClass(&wc);

	const int winW = 760, winH = 620;
	int screenW = ::GetSystemMetrics(SM_CXSCREEN);
	int screenH = ::GetSystemMetrics(SM_CYSCREEN);
	int posX = (screenW - winW) / 2;
	int posY = (screenH - winH) / 2;
	if (posX < 0) posX = 0;
	if (posY < 0) posY = 0;

	HWND hwnd = ::CreateWindowEx(0, kConfigWndClass, kConfigWndTitle,
		WS_OVERLAPPEDWINDOW,
		posX, posY, winW, winH,
		nullptr, nullptr, hInstance, nullptr);
	if (!hwnd) return 0;

	::ShowWindow(hwnd, SW_SHOWNORMAL);
	::UpdateWindow(hwnd);

	MSG msg;
	while (::GetMessage(&msg, nullptr, 0, 0)) {
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);
	}
	return (int)msg.wParam;
}
