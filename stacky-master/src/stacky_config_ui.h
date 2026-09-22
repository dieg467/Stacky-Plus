#pragma once
// stacky_config_ui.h
// Advanced Configuration window ("CONFIGURACIÓN DE STACKY-PLUS"): a two-pane
// dialog (left: tree of root menu folders/submenus beside stacky-plus.exe;
// right: per-folder options) that reads/writes the hidden ".stacky-config"
// file for each folder (see stacky_config.h) instead of requiring manual
// folder-name suffixes or command-line arguments.
//
// Entry point used from wWinMain() when stacky-plus.exe is launched without
// any command-line arguments (plain double-click).

#include <windows.h>
#include <string>

// Runs the Configuration window as the application's main UI. Blocks until
// the window is closed (standard modal message loop) and returns the value
// to be returned from wWinMain.
int RunStackyConfigWindow(HINSTANCE hInstance);

// Forces an immediate rescan/rebuild of the hidden "!stacky.cache" file for
// the given root menu folder (implemented in stacky.cpp, where the Cache
// type lives). Used by the Configuration window so that changes made to a
// ".stacky-config" file (icon, mini flag, layout, etc.) are picked up right
// away instead of only on the menu's *second* opening, which previously
// happened because deleting the cache file alone doesn't rebuild it - the
// rebuild only happened lazily the next time stacky-plus.exe scanned the
// folder to show the menu.
void RebuildStackyCache(const std::wstring& folderPath);
