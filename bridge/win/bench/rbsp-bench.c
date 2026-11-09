/* rbsp-bench.exe: runs a Demucs ONNX model on the GPU (DirectML) or the CPU outside rekordbox, to see
 * where its time and video memory go. It loads a given onnxruntime.dll (by default Pioneer's, kept by
 * the installer in %ProgramData%\rbstemsplus\ort) and DirectML.dll from rekordbox's folder, and uses
 * it directly: no bridge, no cache. Prints the adapters, each run's time and real-time factor, the
 * peak dedicated and shared video memory, and how ONNX Runtime placed the nodes (GPU, CPU, copies).
 *
 *   rbsp-bench --model FILE [--dml N | --cpu | --cuda] [--runs N] [--samples N] [--frames N] [--timeout S]
 *              [--ort DLL] [--rb DIR] [--profile] [--verbose]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#include "onnxruntime_c_api.h"

static const OrtApi *R;
static int verbose;
static int n_placed, n_memcpy;

static double now(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

/* ---------------------------------------------------------------- DXGI */

typedef struct { void *vtbl; } com;
typedef struct {
    WCHAR Description[128]; UINT VendorId, DeviceId, SubSysId, Revision;
    SIZE_T DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory; LUID AdapterLuid; UINT Flags;
} desc1;
typedef struct { UINT64 Budget, CurrentUsage, AvailableForReservation, CurrentReservation; } vminfo;
typedef HRESULT (WINAPI *qi_t)(com *, const GUID *, void **);
typedef HRESULT (WINAPI *qvmi_t)(com *, UINT, int, vminfo *);
typedef ULONG (WINAPI *rel_t)(com *);

static com *adapters[16];
static desc1 descs[16];
static int n_adapters;

static void list_adapters(void) {
    HMODULE dx = LoadLibraryExW(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    typedef HRESULT (WINAPI *create_t)(const GUID *, void **);
    create_t create = dx ? (create_t)(void *)GetProcAddress(dx, "CreateDXGIFactory1") : NULL;
    static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    static const GUID iid_adapter3 = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};
    com *f = NULL;
    if (!create || create(&iid_factory1, (void **)&f) < 0 || !f) { printf("no DXGI (DirectX) here\n"); return; }
    typedef HRESULT (WINAPI *enum1_t)(com *, UINT, com **);
    typedef HRESULT (WINAPI *desc1_t)(com *, desc1 *);
    void **fv = *(void ***)f;
    for (UINT i = 0; i < 16; i++) {
        com *ad = NULL;
        if (((enum1_t)fv[12])(f, i, &ad) < 0 || !ad) break;
        void **av = *(void ***)ad;
        if (((desc1_t)av[10])(ad, &descs[i]) < 0) { ((rel_t)av[2])(ad); continue; }
        char name[160];
        WideCharToMultiByte(CP_UTF8, 0, descs[i].Description, -1, name, sizeof name, NULL, NULL);
        printf("adapter %u: %s, %.1f GB dedicated%s\n", i, name, descs[i].DedicatedVideoMemory / 1073741824.0,
               (descs[i].Flags & 2) ? " (software)" : "");
        com *a3 = NULL;
        if (((qi_t)av[0])(ad, &iid_adapter3, (void **)&a3) >= 0) adapters[i] = a3;
        ((rel_t)av[2])(ad);
        n_adapters = (int)i + 1;
    }
}

/* the adapter the bridge would choose: the most dedicated memory, software ones skipped */
static int best_adapter(void) {
    int best = -1;
    SIZE_T m = 0;
    for (int i = 0; i < n_adapters; i++)
        if (adapters[i] && !(descs[i].Flags & 2) && (best < 0 || descs[i].DedicatedVideoMemory > m)) { best = i; m = descs[i].DedicatedVideoMemory; }
    return best;
}

static volatile int sampling;
static int sample_adapter = -1;
static double peak_local, peak_shared, start_local, start_shared, cur_local, cur_shared;
static void sample_once(void) {
    if (sample_adapter < 0 || !adapters[sample_adapter]) return;
    com *a = adapters[sample_adapter];
    vminfo l = {0}, s = {0};
    qvmi_t q = (qvmi_t)(*(void ***)a)[14];
    if (q(a, 0, 0, &l) >= 0) { cur_local = l.CurrentUsage / 1073741824.0; if (cur_local > peak_local) peak_local = cur_local; }
    if (q(a, 0, 1, &s) >= 0) { cur_shared = s.CurrentUsage / 1073741824.0; if (cur_shared > peak_shared) peak_shared = cur_shared; }
}
static DWORD WINAPI sampler(LPVOID p) {
    (void)p;
    while (sampling) { sample_once(); Sleep(50); }
    return 0;
}

/* ---------------------------------------------------------------- ORT log */

static void ORT_API_CALL logger(void *param, OrtLoggingLevel sev, const char *cat, const char *id, const char *loc, const char *msg) {
    (void)param; (void)sev; (void)cat; (void)id; (void)loc;
    int memcpy_line = strstr(msg, "Memcpy") || strstr(msg, "memcpy");
    int place = strstr(msg, "placed on") || strstr(msg, "Node placements") || strstr(msg, "Number of nodes");
    if (memcpy_line) n_memcpy++;
    if (place) n_placed++;
    if (verbose || place || strstr(msg, "partition") || strstr(msg, "Partition") || strstr(msg, "fall back") || strstr(msg, "fallback"))
        printf("  [ort] %.220s\n", msg);
}

/* ---------------------------------------------------------------- watchdog */

static OrtRunOptions *g_ro;
static HANDLE g_done;
static double g_timeout;
static volatile int g_fired;
static DWORD WINAPI watch(LPVOID p) {
    (void)p;
    double waited = 0;
    while (WaitForSingleObject(g_done, 5000) == WAIT_TIMEOUT) {
        waited += 5;
        sample_once();
        if (sample_adapter >= 0) printf("  ... still running after %.0f s; video memory now: dedicated %.2f GB, shared %.2f GB\n", waited, cur_local, cur_shared);
        else printf("  ... still running after %.0f s\n", waited);
        if (waited >= g_timeout) { g_fired = 1; R->RunOptionsSetTerminate(g_ro); break; }
    }
    return 0;
}

static void die(const char *what, OrtStatus *st) {
    printf("FAIL %s: %s\n", what, st ? R->GetErrorMessage(st) : "?");
    exit(1);
}

static void usage(void) {
    printf("usage: rbsp-bench --model FILE [--dml N | --cpu | --cuda] [--runs N] [--samples N] [--frames N] [--timeout S]\n"
           "                  [--threads N] [--opt 0|1|2|99] [--ort DLL] [--rb DIR] [--profile] [--verbose]\n"
           "  --dml N     the DXGI adapter to run on (default: the one rekordbox's bridge picks); --cpu: no GPU\n"
           "  --runs N    runs after a first, warm-up one (default 3)\n"
           "  --samples N / --frames N   the chunk: mix [1,2,N], mag [1,4,2048,F] (defaults 485100 / ceil(samples/1024))\n"
           "  --opt L     ONNX Runtime graph optimization: 0 none, 1 basic, 2 extended, 99 all (default)\n"
           "  --threads N CPU threads with --cpu (default 4)\n"
           "  --timeout S end a run that takes longer than S seconds (default 180)\n"
           "  --ort DLL   the onnxruntime.dll to use (default %%ProgramData%%\\rbstemsplus\\ort\\onnxruntime.dll)\n"
           "  --rb DIR    the folder with DirectML.dll (default: the newest rekordbox under Program Files)\n"
           "  --profile   write ONNX Runtime's profile (a json file) to the current folder\n"
           "  --verbose   print every ONNX Runtime log line (very long for a big model)\n");
}

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int find_rb(wchar_t *out, size_t n) {
    WIN32_FIND_DATAW fd;
    wchar_t pat[MAX_PATH], best[MAX_PATH] = L"";
    const wchar_t *roots[] = {L"C:\\Program Files\\rekordbox", L"C:\\Program Files\\Pioneer\\rekordbox"};
    for (int r = 0; r < 2; r++) {
        swprintf(pat, MAX_PATH, L"%ls\\*", roots[r]);
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            wchar_t d[MAX_PATH];
            swprintf(d, MAX_PATH, L"%ls\\%ls\\DirectML.dll", roots[r], fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && GetFileAttributesW(d) != INVALID_FILE_ATTRIBUTES &&
                wcscmp(fd.cFileName, best[0] ? wcsrchr(best, L'\\') + 1 : L"") > 0)
                swprintf(best, MAX_PATH, L"%ls\\%ls", roots[r], fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!best[0]) return -1;
    wcsncpy(out, best, n);
    return 0;
}

int main(int argc, char **argv) {
    const char *model = NULL, *ort_arg = NULL, *rb_arg = NULL;
    int dml = -2, cpu = 0, cuda = 0, runs = 3, profile = 0;
    long samples = 485100, frames = 0, threads = 4, opt = -1;
    g_timeout = 180;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--model") && more) model = argv[++i];
        else if (!strcmp(a, "--dml") && more) dml = atoi(argv[++i]);
        else if (!strcmp(a, "--cpu")) cpu = 1;
        else if (!strcmp(a, "--cuda")) cuda = 1;
        else if (!strcmp(a, "--runs") && more) runs = atoi(argv[++i]);
        else if (!strcmp(a, "--samples") && more) samples = atol(argv[++i]);
        else if (!strcmp(a, "--frames") && more) frames = atol(argv[++i]);
        else if (!strcmp(a, "--threads") && more) threads = atol(argv[++i]);
        else if (!strcmp(a, "--opt") && more) opt = atol(argv[++i]);
        else if (!strcmp(a, "--timeout") && more) g_timeout = atof(argv[++i]);
        else if (!strcmp(a, "--ort") && more) ort_arg = argv[++i];
        else if (!strcmp(a, "--rb") && more) rb_arg = argv[++i];
        else if (!strcmp(a, "--profile")) profile = 1;
        else if (!strcmp(a, "--verbose")) verbose = 1;
        else { usage(); return 2; }
    }
    if (frames < 1) frames = (samples + 1023) / 1024;
    if (!model || runs < 1 || samples < 1024 || threads < 1) { usage(); return 2; }

    list_adapters();
    if (!cpu && dml == -2) dml = best_adapter();
    if (!cpu && (dml < 0 || dml >= n_adapters)) { printf("no usable GPU: try --cpu\n"); return 1; }
    sample_adapter = cpu ? -1 : dml;

    /* the libraries: DirectML.dll from rekordbox's folder, then the chosen onnxruntime.dll */
    wchar_t rb[MAX_PATH] = L"", ort[MAX_PATH] = L"";
    if (rb_arg) { wchar_t *w = wide(rb_arg); wcsncpy(rb, w, MAX_PATH - 1); free(w); }
    else if (find_rb(rb, MAX_PATH)) wcscpy(rb, L"");
    if (rb[0] && !cuda) { SetDllDirectoryW(rb); wprintf(L"DirectML from %ls\n", rb); }
    if (ort_arg) { wchar_t *w = wide(ort_arg); wcsncpy(ort, w, MAX_PATH - 1); free(w); }
    else {
        wchar_t pd[MAX_PATH];
        if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) wcscpy(pd, L"C:\\ProgramData");
        swprintf(ort, MAX_PATH, L"%ls\\rbstemsplus\\ort\\onnxruntime.dll", pd);
    }
    if (cuda) {   /* the CUDA libraries sit next to its onnxruntime.dll */
        wchar_t dir[MAX_PATH];
        wcsncpy(dir, ort, MAX_PATH - 1); dir[MAX_PATH - 1] = 0;
        wchar_t *sl = wcsrchr(dir, L'\\');
        if (sl) { *sl = 0; SetDllDirectoryW(dir); }
    }
    HMODULE h = LoadLibraryExW(ort, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!h) h = LoadLibraryW(ort);
    if (!h) { wprintf(L"FAIL cannot load %ls (error %lu): use --ort\n", ort, GetLastError()); return 1; }
    const OrtApiBase *(ORT_API_CALL *get)(void) = (const OrtApiBase *(ORT_API_CALL *)(void))(void *)GetProcAddress(h, "OrtGetApiBase");
    const OrtApiBase *base = get ? get() : NULL;
    if (!base) { printf("FAIL no OrtGetApiBase in that library\n"); return 1; }
    R = base->GetApi(18);
    wprintf(L"ONNX Runtime %hs from %ls\n", base->GetVersionString(), ort);
    if (!R) { printf("FAIL this library has no 1.18 API\n"); return 1; }
    OrtStatus *(ORT_API_CALL *append_dml)(OrtSessionOptions *, int) =
        (OrtStatus *(ORT_API_CALL *)(OrtSessionOptions *, int))(void *)GetProcAddress(h, "OrtSessionOptionsAppendExecutionProvider_DML");
    if (!cpu && !cuda && !append_dml) { printf("FAIL this ONNX Runtime has no DirectML: use --cpu or --ort\n"); return 1; }

    OrtEnv *env = NULL;
    OrtSessionOptions *so = NULL;
    OrtStatus *st = R->CreateEnvWithCustomLogger(logger, NULL, ORT_LOGGING_LEVEL_VERBOSE, "rbsp-bench", &env);
    if (st) die("env", st);
    if ((st = R->CreateSessionOptions(&so))) die("options", st);
    R->SetSessionLogSeverityLevel(so, 0);
    if (opt >= 0) {                                         /* 0 none, 1 basic, 2 extended, 99 all (the default) */
        R->SetSessionGraphOptimizationLevel(so, opt == 0 ? ORT_DISABLE_ALL : opt == 1 ? ORT_ENABLE_BASIC : opt == 2 ? ORT_ENABLE_EXTENDED : ORT_ENABLE_ALL);
        printf("graph optimization level %ld\n", opt);
    }
    if (cpu) { R->SetIntraOpNumThreads(so, (int)threads); printf("CPU threads: %ld\n", threads); }
    if (cuda) {   /* a CUDA ONNX Runtime (onnxruntime-gpu 1.18, CUDA 12, cuDNN 9) */
        OrtCUDAProviderOptionsV2 *co = NULL;
        const char *keys[] = {"device_id", "cudnn_conv_algo_search"}, *vals[] = {"0", "HEURISTIC"};
        if ((st = R->CreateCUDAProviderOptions(&co))) die("CUDA options", st);
        if ((st = R->UpdateCUDAProviderOptions(co, keys, vals, 2))) die("CUDA options", st);
        if ((st = R->SessionOptionsAppendExecutionProvider_CUDA_V2(so, co))) die("CUDA", st);
        R->ReleaseCUDAProviderOptions(co);
        printf("running on the GPU: CUDA device 0 (video memory sampled on DXGI adapter %d)\n", dml);
    } else if (!cpu) {
        R->DisableMemPattern(so);
        R->SetSessionExecutionMode(so, ORT_SEQUENTIAL);
        if ((st = append_dml(so, dml))) die("DirectML", st);
        char name[160];
        WideCharToMultiByte(CP_UTF8, 0, descs[dml].Description, -1, name, sizeof name, NULL, NULL);
        printf("running on the GPU: adapter %d, %s\n", dml, name);
    } else printf("running on the CPU\n");
    if (profile) R->EnableProfiling(so, L"rbsp-bench-profile");

    sample_once();
    start_local = peak_local; start_shared = peak_shared;
    sampling = 1;
    HANDLE st_thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);

    wchar_t *wm = wide(model);
    OrtSession *s = NULL;
    double t0 = now();
    printf("opening %s ...\n", model);
    if ((st = R->CreateSession(env, wm, so, &s))) die("session", st);
    printf("session created in %.1f s; ORT placed nodes: %d placement lines, %d memcpy lines\n", now() - t0, n_placed, n_memcpy);

    /* inputs from the model's own declared shapes: rank 3 = mix, rank 4 = mag */
    OrtAllocator *al = NULL;
    R->GetAllocatorWithDefaultOptions(&al);
    size_t nin = 0, nout = 0;
    R->SessionGetInputCount(s, &nin);
    R->SessionGetOutputCount(s, &nout);
    char *in_names[8] = {0}, *out_names[8] = {0};
    OrtValue *in[8] = {0}, *out[8] = {0};
    OrtMemoryInfo *mi = NULL;
    R->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi);
    if (nin > 8 || nout > 8) { printf("FAIL too many inputs or outputs\n"); return 1; }
    for (size_t i = 0; i < nin; i++) {
        R->SessionGetInputName(s, i, al, &in_names[i]);
        OrtTypeInfo *ti = NULL;
        const OrtTensorTypeAndShapeInfo *ts = NULL;
        size_t rank = 0;
        int64_t dims[8] = {0};
        if (R->SessionGetInputTypeInfo(s, i, &ti) || R->CastTypeInfoToTensorInfo(ti, &ts) || !ts || R->GetDimensionsCount(ts, &rank) || rank > 8 ||
            R->GetDimensions(ts, dims, rank)) { printf("FAIL input %zu isn't a tensor\n", i); return 1; }
        size_t count = 1;
        for (size_t d = 0; d < rank; d++) {
            if (dims[d] <= 0) dims[d] = rank == 3 && d == 2 ? samples : rank == 4 && d == 3 ? frames : rank == 4 && d == 2 ? 2048 : rank == 4 && d == 1 ? 4 : rank == 3 && d == 1 ? 2 : 1;
            count *= (size_t)dims[d];
        }
        float *data = malloc(count * sizeof(float));
        for (size_t k = 0; k < count; k++) data[k] = (float)(0.1 * sin(0.01 * (double)(k % 100000)) + 0.02 * sin(0.37 * (double)k));
        printf("input %s: [", in_names[i]);
        for (size_t d = 0; d < rank; d++) printf("%s%lld", d ? "," : "", (long long)dims[d]);
        printf("]\n");
        if ((st = R->CreateTensorWithDataAsOrtValue(mi, data, count * sizeof(float), dims, rank, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[i]))) die("input tensor", st);
        R->ReleaseTypeInfo(ti);
    }
    for (size_t i = 0; i < nout; i++) R->SessionGetOutputName(s, i, al, &out_names[i]);

    OrtRunOptions *ro = NULL;
    R->CreateRunOptions(&ro);
    g_ro = ro;
    double chunk = (double)samples / 44100.0, total = 0;
    int ok = 0;
    for (int r = 0; r <= runs; r++) {
        g_fired = 0;
        g_done = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE wt = CreateThread(NULL, 0, watch, NULL, 0, NULL);
        for (size_t i = 0; i < nout; i++) if (out[i]) { R->ReleaseValue(out[i]); out[i] = NULL; }
        double t = now();
        st = R->Run(s, ro, (const char *const *)in_names, (const OrtValue *const *)in, nin, (const char *const *)out_names, nout, out);
        t = now() - t;
        SetEvent(g_done);
        WaitForSingleObject(wt, INFINITE);
        CloseHandle(wt); CloseHandle(g_done);
        if (st) {
            printf("run %d: %s after %.1f s: %s\n", r, g_fired ? "ENDED at the timeout" : "FAILED", t, R->GetErrorMessage(st));
            R->ReleaseStatus(st);
            break;
        }
        printf("run %d%s: %.2f s = %.2fx real time (a %.1f s chunk)\n", r, r ? "" : " (warm-up)", t, chunk / t, chunk);
        if (r == 0 && nout > 1) {                           /* the second output (xt, the stems): finite, and its size, to compare with the CPU */
            float *xt = NULL;
            OrtTensorTypeAndShapeInfo *ti = NULL;
            size_t cnt = 0;
            if (!R->GetTensorMutableData(out[1], (void **)&xt) && !R->GetTensorTypeAndShape(out[1], &ti) && !R->GetTensorShapeElementCount(ti, &cnt) && xt) {
                double sum = 0, mx = 0;
                size_t bad = 0;
                for (size_t k = 0; k < cnt; k++) { if (!isfinite(xt[k])) bad++; else { sum += fabs(xt[k]); if (fabs(xt[k]) > mx) mx = fabs(xt[k]); } }
                printf("output %s: %zu values, %zu not finite, mean|.| %.6g, max|.| %.6g\n", out_names[1], cnt, bad, cnt ? sum / (double)cnt : 0.0, mx);
            }
            if (ti) R->ReleaseTensorTypeAndShapeInfo(ti);
        }
        if (r) { total += t; ok++; }
    }
    if (ok) printf("average of %d runs: %.2f s = %.2fx real time\n", ok, total / ok, chunk / (total / ok));
    sampling = 0;
    WaitForSingleObject(st_thread, 2000);
    if (!cpu) {
        printf("video memory, this process's adapter, peak: dedicated %.2f GB (%.2f before), shared %.2f GB (%.2f before)\n", peak_local, start_local, peak_shared, start_shared);
        if (descs[dml].DedicatedVideoMemory && peak_local * 1073741824.0 > 0.9 * (double)descs[dml].DedicatedVideoMemory)
            printf("=> dedicated memory is nearly full: anything more spills into shared memory over PCIe (very slow)\n");
    }
    printf("ORT placement lines: %d (memcpy lines: %d): run with --verbose to see them all\n", n_placed, n_memcpy);
    if (profile) {
        char *pf = NULL;
        if (!R->SessionEndProfiling(s, al, &pf) && pf) printf("profile written to %s\n", pf);
    }
    return ok ? 0 : 1;
}
