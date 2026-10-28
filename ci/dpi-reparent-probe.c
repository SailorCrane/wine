/* Diagnostic for SetParent DPI behavior. No application data is accessed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define SNAPSHOT (WM_APP + 61)
#define REPARENT (WM_APP + 62)
typedef struct { int process, thread, window, sibling, fresh; UINT dpi; } snapshot;
typedef struct {
    HWND child, host, original;
    snapshot before, after;
    HWND result;
    DWORD error;
    BOOL initialized;
} shared_data;
static shared_data *shared;
static int failures;

static int awareness(DPI_AWARENESS_CONTEXT ctx)
{
    return GetAwarenessFromDpiAwarenessContext(ctx);
}
static snapshot capture(HWND child, HWND sibling)
{
    snapshot s = { -1, -1, -1, -1, -1, 0 };
    HMODULE module = LoadLibraryA("shcore.dll");
    HRESULT (WINAPI *query)(HANDLE, int *) = module ? (void *)GetProcAddress(module,"GetProcessDpiAwareness") : NULL;
    HWND fresh;
    if (query) query(NULL, &s.process);
    if (module) FreeLibrary(module);
    s.thread = awareness(GetThreadDpiAwarenessContext());
    s.window = awareness(GetWindowDpiAwarenessContext(child));
    s.sibling = awareness(GetWindowDpiAwarenessContext(sibling));
    s.dpi = GetDpiForWindow(child);
    fresh = CreateWindowA("STATIC", "dpi-probe-fresh", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    if (fresh) { s.fresh = awareness(GetWindowDpiAwarenessContext(fresh)); DestroyWindow(fresh); }
    return s;
}
static void reparent(void)
{
    SetLastError(0xdeadbeef);
    shared->result = SetParent(shared->child, shared->host);
    shared->error = GetLastError();
}
static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == SNAPSHOT) { shared->after = capture(hwnd, shared->original); return 1; }
    if (msg == REPARENT) { reparent(); return 1; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
}
static DWORD wait_pumping(HANDLE handle, DWORD timeout)
{
    DWORD start = GetTickCount(), ret;
    for (;;) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeout) return WAIT_TIMEOUT;
        ret = MsgWaitForMultipleObjects(1, &handle, FALSE, timeout - elapsed, QS_ALLINPUT);
        if (ret != WAIT_OBJECT_0 + 1) return ret;
        pump();
    }
}
static int worker(int argc, char **argv)
{
    HANDLE map = (HANDLE)(uintptr_t)strtoull(argv[2], NULL, 16);
    HANDLE ready = (HANDLE)(uintptr_t)strtoull(argv[3], NULL, 16);
    HANDLE stop = (HANDLE)(uintptr_t)strtoull(argv[4], NULL, 16);
    int mode = atoi(argv[5]);
    WNDCLASSA cls = {0};
    (void)argc;
    shared = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shared));
    if (!shared) return 20;
    if (!strcmp(argv[1], "third")) { reparent(); UnmapViewOfFile(shared); return 0; }
    if (!SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(intptr_t)~mode)) return 21;
    cls.lpfnWndProc = wndproc; cls.lpszClassName = "DpiReparentProbe"; cls.hInstance = GetModuleHandleA(NULL);
    if (!RegisterClassA(&cls)) return 22;
    shared->original = CreateWindowA(cls.lpszClassName, "dpi-probe-original", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, cls.hInstance, NULL);
    shared->child = CreateWindowA(cls.lpszClassName, "dpi-probe-child", WS_CHILD, 0, 0, 80, 80, shared->original, NULL, cls.hInstance, NULL);
    if (!shared->original || !shared->child) return 23;
    shared->before = capture(shared->child, shared->original);
    shared->initialized = TRUE;
    SetEvent(ready);
    wait_pumping(stop, 20000);
    DestroyWindow(shared->child); DestroyWindow(shared->original);
    UnmapViewOfFile(shared); CloseHandle(map); CloseHandle(ready); CloseHandle(stop);
    return 0;
}
static BOOL launch(const char *exe, const char *role, HANDLE map, HANDLE ready, HANDLE stop, int dpi, PROCESS_INFORMATION *pi)
{
    char cmd[2048]; STARTUPINFOA si = {0}; si.cb = sizeof(si);
    snprintf(cmd, sizeof(cmd), "\"%s\" %s %llx %llx %llx %d", exe, role,
             (unsigned long long)(uintptr_t)map, (unsigned long long)(uintptr_t)ready,
             (unsigned long long)(uintptr_t)stop, dpi);
    return CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, pi);
}
static void print_snapshot(const char *label, snapshot s)
{
    printf(" %s[p=%d,t=%d,w=%d,s=%d,n=%d,dpi=%u]", label, s.process, s.thread, s.window, s.sibling, s.fresh, s.dpi);
}
int main(int argc, char **argv)
{
    char exe[MAX_PATH]; int i, j, caller;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (argc == 6) return worker(argc, argv);
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    setbuf(stdout, NULL);
    printf("DPI SetParent probe: awareness 0=unaware 1=system 2=per-monitor; p=process t=thread w=window s=sibling n=new-window\n");
    /* Same-process control cases use mixed thread contexts. */
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) {
        DPI_AWARENESS_CONTEXT orig = SetThreadDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(intptr_t)~i);
        HWND host = CreateWindowA("STATIC", "dpi-probe-host", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
        HWND child, old, parent; DWORD error; int pass;
        SetThreadDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(intptr_t)~j);
        child = CreateWindowA("STATIC", "dpi-probe-child", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
        SetLastError(0xdeadbeef); old = SetParent(child, host); error = GetLastError(); parent = GetAncestor(child, GA_PARENT);
        pass = host && child && (i == j ? (old != NULL && parent == host) : (old == NULL && error == ERROR_INVALID_STATE && parent == GetDesktopWindow()));
        printf("same host=%d child=%d old=%p error=%lu actual=%p %s\n", i,j,old,error,parent,pass?"PASS":"FAIL");
        failures += !pass;
        DestroyWindow(child); DestroyWindow(host); SetThreadDpiAwarenessContext(orig);
    }
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) for (caller = 0; caller < 3; caller++) {
        HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(shared_data), NULL);
        HANDLE ready = CreateEventA(&sa, TRUE, FALSE, NULL), stop = CreateEventA(&sa, TRUE, FALSE, NULL);
        PROCESS_INFORMATION pi = {0}; HWND host, parent; int pass; DWORD_PTR reply;
        DPI_AWARENESS_CONTEXT orig = SetThreadDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(intptr_t)~i);
        host = CreateWindowA("STATIC", "dpi-probe-host", WS_OVERLAPPED, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
        shared = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shared));
        if (!map || !ready || !stop || !host || !shared) { printf("infrastructure allocation failure\n"); return 30; }
        ZeroMemory(shared,sizeof(*shared)); shared->host=host;
        if (!launch(exe,"worker",map,ready,stop,j,&pi)) { printf("worker launch failure %lu\n",GetLastError());return 31; }
        if (wait_pumping(ready,10000) != WAIT_OBJECT_0 || !shared->initialized) { SetEvent(stop); wait_pumping(pi.hProcess,10000); printf("worker setup failure\n");return 32; }
        if (!caller) reparent();
        else if (caller == 1) {
            if (!SendMessageTimeoutA(shared->child,REPARENT,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,3000,&reply)) { printf("child request timed out\n"); return 33; }
        } else {
            PROCESS_INFORMATION third = {0};
            if (!launch(exe,"third",map,ready,stop,j,&third)) return 34;
            if (wait_pumping(third.hProcess,5000)!=WAIT_OBJECT_0) return 35;
            CloseHandle(third.hProcess); CloseHandle(third.hThread);
        }
        if (!SendMessageTimeoutA(shared->child,SNAPSHOT,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,3000,&reply)) return 36;
        parent=GetAncestor(shared->child,GA_PARENT);
        pass=shared->result==shared->original && parent==host;
        printf("cross host=%d child=%d caller=%d old=%p error=%lu attached=%d %s",i,j,caller,shared->result,shared->error,parent==host,pass?"PASS":"FAIL");
        print_snapshot("before",shared->before);print_snapshot("after",shared->after);printf("\n");failures+=!pass;
        SetEvent(stop);
        if(wait_pumping(pi.hProcess,5000)!=WAIT_OBJECT_0){printf("worker cleanup timeout\n");return 37;}
        CloseHandle(pi.hProcess);CloseHandle(pi.hThread);UnmapViewOfFile(shared);shared=NULL;
        CloseHandle(map);CloseHandle(ready);CloseHandle(stop);DestroyWindow(host);SetThreadDpiAwarenessContext(orig);
    }
    printf("Total: 36 cases, %d failures; awareness reset observations are diagnostic, not asserted\n",failures);
    return failures ? 1 : 0;
}
