#ifndef MININPP_PLUGIN_H
#define MININPP_PLUGIN_H

#include <windows.h>

typedef struct MiniHost {
    size_t size;
    HWND mainWnd;
    HWND sciWnd;
    LRESULT (*sci)(UINT msg, WPARAM w, LPARAM l);
} MiniHost;

#define MININPP_EXPORT __declspec(dllexport)

typedef void (__cdecl *PFN_PluginInit)(const MiniHost *host);
typedef const wchar_t *(__cdecl *PFN_PluginName)(void);
typedef int (__cdecl *PFN_PluginCommandCount)(void);
typedef const wchar_t *(__cdecl *PFN_PluginCommandName)(int i);
typedef void (__cdecl *PFN_PluginRun)(int i);

/* Plugins export: mininpp_init, mininpp_name, mininpp_command_count,
 * mininpp_command_name, and mininpp_run. */

#endif

