/* Windows glue for rbsp-prepare: the program's real entry point (arguments as UTF-8, standard
 * output in binary mode so lines end in \n as on macOS), the app's pause and stop events, and the
 * pause point before each model run.
 *
 * Stop (the app's Stop, macOS's SIGTERM): the manual-reset event Local\rbsp-prepare-<pid>-stop;
 * set, the run finishes the chunk it is on, writes its kept set and manifest, prints its final
 * line and exits 0. Pause: Local\rbsp-prepare-<pid>-pause; while it is set, no new chunk starts
 * (the stop event still gets through). Both are created here when the program starts: a missing
 * event means "not signaled". */
#include <shellapi.h>
#include "../model.h"

int prepare_main(int argc, char **argv);              /* prepare.c, with main renamed */
int model_run_inner(model *m, const float *mix, size_t L, int ours, float **x, float **xt, size_t *F, char *why, size_t whyn);

char rbp_rekordbox_dir[RB_PATH_MAX];

static HANDLE ev_stop, ev_pause;
static rbp_handler term_fn;

static HANDLE make_event(const char *what) {
    wchar_t name[96];
    swprintf(name, 96, L"Local\\rbsp-prepare-%lu-%hs", (unsigned long)GetCurrentProcessId(), what);
    return CreateEventW(NULL, TRUE, FALSE, name);
}

static DWORD WINAPI stop_watch(LPVOID p) {
    (void)p;
    if (ev_stop && WaitForSingleObject(ev_stop, INFINITE) == WAIT_OBJECT_0 && term_fn) term_fn(SIGTERM);
    return 0;
}

static rbp_handler int_fn;
static BOOL WINAPI on_ctrl(DWORD t) {
    if ((t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT || t == CTRL_CLOSE_EVENT) && int_fn) { int_fn(SIGINT); return TRUE; }
    return FALSE;
}

rbp_handler rbp_signal(int sig, rbp_handler fn) {
    if (sig == SIGTERM) {
        if (!term_fn) {
            term_fn = fn;
            HANDLE t = ev_stop ? CreateThread(NULL, 0, stop_watch, NULL, 0, NULL) : NULL;
            if (t) CloseHandle(t);
        }
    } else if (sig == SIGINT) {
        int_fn = fn;
        SetConsoleCtrlHandler(on_ctrl, TRUE);
    }
    return NULL;
}

static int stopped(void) { return ev_stop && WaitForSingleObject(ev_stop, 0) == WAIT_OBJECT_0; }

void rbp_pause_wait(void) {
    while (ev_pause && WaitForSingleObject(ev_pause, 0) == WAIT_OBJECT_0 && !stopped()) Sleep(100);
}

int model_run(model *m, const float *mix, size_t L, int ours, float **x, float **xt, size_t *F, char *why, size_t whyn) {
    rbp_pause_wait();
    return model_run_inner(m, mix, L, ours, x, xt, F, why, whyn);
}

int main(void) {
    int argc = 0;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!w || !argv) return 1;
    for (int i = 0; i < argc; i++) {
        char u[4 * RB_PATH_MAX];
        argv[i] = rb_utf8(w[i], -1, u, sizeof u) == 0 ? _strdup(u) : _strdup("");
        if (i > 0 && !strcmp(argv[i - 1], "--rekordbox")) rb_strlcpy(rbp_rekordbox_dir, argv[i], sizeof rbp_rekordbox_dir);
    }
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    ev_stop = make_event("stop");
    ev_pause = make_event("pause");
    return prepare_main(argc, argv);
}
