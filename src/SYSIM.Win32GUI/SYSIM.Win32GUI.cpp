#include "framework.h"
#include "core/app.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    App app;
    if (!app.Init(hInstance)) return -1;
    return app.Run(nCmdShow);
}