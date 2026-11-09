/* drive.exe MODE [PRE_SET_MS] helper.exe args...: runs rbsp-prepare.exe and signals its pause and
 * stop events (Local\rbsp-prepare-<pid>-pause / -stop) as the app does, printing its output lines
 * with the time each arrived (ms since the start) and what it did itself.
 *   pause-then-resume  the pause event is set before the helper starts; reset after 3 s
 *   stop-at-chunk0     the stop event is set when the first {"at":..} line arrives
 *   pause-and-stop     paused from the start, stop set after 2 s (stop must get through the pause) */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static DWORD t0;
static HANDLE ev(DWORD pid, const char *what) {
    wchar_t n[96];
    swprintf(n, 96, L"Local\\rbsp-prepare-%lu-%hs", (unsigned long)pid, what);
    return CreateEventW(NULL, TRUE, FALSE, n);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *mode = argv[1];
    char cmd[8192] = "";
    for (int i = 2; i < argc; i++) { strcat(cmd, i > 2 ? " \"" : "\""); strcat(cmd, argv[i]); strcat(cmd, "\""); }
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = GetStdHandle(STD_ERROR_HANDLE); si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) { printf("drive: can't start\n"); return 2; }
    CloseHandle(wr);
    HANDLE pause = ev(pi.dwProcessId, "pause"), stop = ev(pi.dwProcessId, "stop");
    int paused_start = !strcmp(mode, "pause-then-resume") || !strcmp(mode, "pause-and-stop");
    if (paused_start) SetEvent(pause);
    t0 = GetTickCount();
    ResumeThread(pi.hThread);
    char line[8192];
    size_t n = 0;
    int resumed = 0, stopped = 0;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
        DWORD now = GetTickCount() - t0;
        if (!strcmp(mode, "pause-then-resume") && !resumed && now >= 3000) { ResetEvent(pause); resumed = 1; printf("drive: resumed at %lu\n", (unsigned long)now); }
        if (!strcmp(mode, "pause-and-stop") && !stopped && now >= 2000) { SetEvent(stop); stopped = 1; printf("drive: stop set at %lu\n", (unsigned long)now); }
        if (!avail) { Sleep(20); continue; }
        char c;
        DWORD got;
        if (!ReadFile(rd, &c, 1, &got, NULL) || !got) break;
        if (c == '\n' || n + 1 >= sizeof line) {
            line[n] = 0;
            if (n && line[n - 1] == '\r') line[n - 1] = 0;
            printf("%lu %s\n", (unsigned long)(GetTickCount() - t0), line);
            if (!strcmp(mode, "stop-at-chunk0") && !stopped && strstr(line, "\"at\":0,")) { SetEvent(stop); stopped = 1; printf("drive: stop set\n"); }
            n = 0;
        } else line[n++] = c;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("drive: exit %lu\n", (unsigned long)code);
    return 0;
}
