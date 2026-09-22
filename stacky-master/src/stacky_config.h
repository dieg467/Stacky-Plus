#pragma once
// stacky_config.h
// Per-folder configuration persisted in a hidden ".stacky-config" text file.
// This lets the Configuration window (stacky_config_ui.h/.cpp) apply menu
// behaviour (grid mode, columns, icon size, theme, position, sort order,
// custom name/icon) WITHOUT requiring the user to rename folders with
// magic suffixes (.mini, .icononly-NN, .NN-name-right, etc). Folder-name
// suffixes are still supported for backward compatibility and are used as
// a fallback when no ".stacky-config" is present (see stacky.cpp).

#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

typedef std::wstring StackyString;

// Folder-level "grid mode" duplicated here (kept as plain ints/strings to
// avoid a hard dependency on stacky.cpp's GridMode enum from this header).
enum StackyConfigMode {
	SCFG_MODE_DEFAULT      = 0, // list with submenus (--) 
	SCFG_MODE_ICONGRID     = 1, // --iconmenu-NN
	SCFG_MODE_DOUBLE_COL   = 2, // --iconmenu-C2
	SCFG_MODE_DOUBLE_ROW   = 3, // --iconmenu-F2
	SCFG_MODE_SINGLESUB    = 4  // --singlesubmenu
};

enum StackyConfigTheme {
	SCFG_THEME_SYSTEM = 0,
	SCFG_THEME_LIGHT  = 1,
	SCFG_THEME_DARK   = 2
};

enum StackyConfigSort {
	SCFG_SORT_ALPHA        = 0,
	SCFG_SORT_FOLDERSFIRST = 1,
	SCFG_SORT_CUSTOM_PREFIX= 2  // user manually added %NN% prefixes; detected, not settable directly
};

enum StackySubmenuLayout {
	SCFG_LAYOUT_ICONONLY   = 0, // .icononly-NN
	SCFG_LAYOUT_NAME_RIGHT = 1, // .NN-name-right
	SCFG_LAYOUT_NAME_BELOW = 2  // .NN-name-below
};

// The name of the hidden per-folder config file.
static const StackyString STACKY_CONFIG_FILE_NAME = L".stacky-config";

// Full set of settings that can apply to a ROOT menu folder (the ones
// directly beside stacky-plus.exe) as well as to a SUBMENU folder. Not all
// fields are meaningful for every folder type; the Configuration UI only
// shows/edits the relevant subset per node type, but this single struct is
// used for both to keep load/save logic simple.
struct StackyFolderConfig {
	bool     loaded = false;      // true if a .stacky-config file was found and parsed

	// ROOT-only fields
	StackyString    menu_name;            // display name used for the created .lnk
	StackyString    menu_icon_path;       // custom icon (.ico/.exe/.dll,idx) for the .lnk; empty = use stacky-plus.exe icon
	StackyConfigMode mode = SCFG_MODE_DEFAULT;
	int      grid_cols = 3;               // NN for --iconmenu-NN / --singlesubmenu default cols
	bool     grid_names_below = false;     // --iconmenu-NN-name
	bool     grid_names_right = false;     // --iconmenu-NN-name-right
	bool     doublecol_names = false;      // --iconmenu-C2-name
	bool     doublerow_names = false;      // --iconmenu-F2-name
	StackyConfigTheme theme = SCFG_THEME_SYSTEM;
	bool     mouse_position = false;       // --mouseposition
	StackyConfigSort sort_mode = SCFG_SORT_ALPHA;

	// Shared (root and submenu) fields
	bool     mini_icons = false;           // --mini / .submenu-mini / .mini
	bool     add_separator = false;        // draws an automatic separator below the last
											// submenu, before the plain shortcuts, in this
											// menu/submenu (list, single-submenu and double-column modes only)

	// SUBMENU-only fields
	StackyString    submenu_icon_path;    // custom icon override for this submenu folder
	int      submenu_cols = 3;             // NN for .submenu-NN (--singlesubmenu variant)
	StackySubmenuLayout submenu_layout = SCFG_LAYOUT_ICONONLY;

	static StackyString TrimW(const StackyString& s) {
		size_t a = s.find_first_not_of(L" \t\r\n");
		if (a == StackyString::npos) return L"";
		size_t b = s.find_last_not_of(L" \t\r\n");
		return s.substr(a, b - a + 1);
	}

	static bool ToBool(const StackyString& v) { return v == L"1" || v == L"true"; }
	static StackyString FromBool(bool b) { return b ? L"1" : L"0"; }

	// Serializes all fields as "key=value" lines (UTF-16LE text file).
	StackyString Serialize() const {
		std::wstringstream out;
		out << L"menu_name=" << menu_name << L"\r\n";
		out << L"menu_icon_path=" << menu_icon_path << L"\r\n";
		out << L"mode=" << (int)mode << L"\r\n";
		out << L"grid_cols=" << grid_cols << L"\r\n";
		out << L"grid_names_below=" << FromBool(grid_names_below) << L"\r\n";
		out << L"grid_names_right=" << FromBool(grid_names_right) << L"\r\n";
		out << L"doublecol_names=" << FromBool(doublecol_names) << L"\r\n";
		out << L"doublerow_names=" << FromBool(doublerow_names) << L"\r\n";
		out << L"theme=" << (int)theme << L"\r\n";
		out << L"mouse_position=" << FromBool(mouse_position) << L"\r\n";
		out << L"sort_mode=" << (int)sort_mode << L"\r\n";
		out << L"mini_icons=" << FromBool(mini_icons) << L"\r\n";
		out << L"add_separator=" << FromBool(add_separator) << L"\r\n";
		out << L"submenu_icon_path=" << submenu_icon_path << L"\r\n";
		out << L"submenu_cols=" << submenu_cols << L"\r\n";
		out << L"submenu_layout=" << (int)submenu_layout << L"\r\n";
		return out.str();
	}

	void ParseLine(const StackyString& key, const StackyString& value) {
		if (key == L"menu_name") menu_name = value;
		else if (key == L"menu_icon_path") menu_icon_path = value;
		else if (key == L"mode") mode = (StackyConfigMode)_wtoi(value.c_str());
		else if (key == L"grid_cols") grid_cols = _wtoi(value.c_str());
		else if (key == L"grid_names_below") grid_names_below = ToBool(value);
		else if (key == L"grid_names_right") grid_names_right = ToBool(value);
		else if (key == L"doublecol_names") doublecol_names = ToBool(value);
		else if (key == L"doublerow_names") doublerow_names = ToBool(value);
		else if (key == L"theme") theme = (StackyConfigTheme)_wtoi(value.c_str());
		else if (key == L"mouse_position") mouse_position = ToBool(value);
		else if (key == L"sort_mode") sort_mode = (StackyConfigSort)_wtoi(value.c_str());
		else if (key == L"mini_icons") mini_icons = ToBool(value);
		else if (key == L"add_separator") add_separator = ToBool(value);
		else if (key == L"submenu_icon_path") submenu_icon_path = value;
		else if (key == L"submenu_cols") submenu_cols = _wtoi(value.c_str());
		else if (key == L"submenu_layout") submenu_layout = (StackySubmenuLayout)_wtoi(value.c_str());
	}

	static StackyString ConfigPathFor(const StackyString& folder_path) {
		StackyString p = folder_path;
		if (!p.empty() && p.back() != L'\\') p += L'\\';
		return p + STACKY_CONFIG_FILE_NAME;
	}

	// Loads config from "<folder_path>\.stacky-config". Returns a default
	// (unloaded) config if the file doesn't exist.
	static StackyFolderConfig Load(const StackyString& folder_path) {
		StackyFolderConfig cfg;
		StackyString path = ConfigPathFor(folder_path);
		HANDLE hFile = ::CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE) return cfg;

		DWORD size = ::GetFileSize(hFile, nullptr);
		std::vector<char> raw(size);
		DWORD read = 0;
		BOOL ok = size > 0 ? ::ReadFile(hFile, raw.data(), size, &read, nullptr) : TRUE;
		::CloseHandle(hFile);
		if (!ok) return cfg;

		// Expect UTF-16LE with optional BOM (as written by Save()).
		size_t offset = 0;
		if (size >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) offset = 2;
		const wchar_t* wtext = reinterpret_cast<const wchar_t*>(raw.data() + offset);
		size_t wlen = (size - offset) / sizeof(wchar_t);
		StackyString content(wtext, wlen);

		std::wstringstream ss(content);
		StackyString line;
		while (std::getline(ss, line)) {
			if (!line.empty() && line.back() == L'\r') line.pop_back();
			if (line.empty()) continue;
			size_t eq = line.find(L'=');
			if (eq == StackyString::npos) continue;
			StackyString key = TrimW(line.substr(0, eq));
			StackyString value = line.substr(eq + 1);
			cfg.ParseLine(key, value);
		}
		cfg.loaded = true;
		return cfg;
	}

	// Saves this config as a hidden UTF-16LE text file inside folder_path.
	bool Save(const StackyString& folder_path) const {
		StackyString path = ConfigPathFor(folder_path);
		::SetFileAttributes(path.c_str(), FILE_ATTRIBUTE_NORMAL); // clear hidden so we can rewrite
		HANDLE hFile = ::CreateFile(path.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE) return false;

		StackyString content = Serialize();
		WORD bom = 0xFEFF;
		DWORD written = 0;
		::WriteFile(hFile, &bom, sizeof(bom), &written, nullptr);
		::WriteFile(hFile, content.c_str(), (DWORD)(content.size() * sizeof(wchar_t)), &written, nullptr);
		::CloseHandle(hFile);
		::SetFileAttributes(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
		return true;
	}
};
