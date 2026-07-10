// DLL entry points for EuroScope.
//
// EuroScope loads the DLL and calls EuroScopePlugInInit, which must hand
// back a heap-allocated CPlugIn subclass. EuroScopePlugInExit is called
// right before the DLL is unloaded. Both are declared (with C++ linkage)
// at the bottom of EuroScopePlugIn.h; the definitions here must match.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ConnectorPlugin.h"

namespace
{
    ConnectorPlugin* g_plugin = nullptr;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}

void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
    g_plugin = new ConnectorPlugin();
    *ppPlugInInstance = g_plugin;
}

void __declspec(dllexport) EuroScopePlugInExit(void)
{
    delete g_plugin;
    g_plugin = nullptr;
}
