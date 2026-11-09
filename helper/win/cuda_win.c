/* The CUDA path (step 2 of the Windows helper): our Demucs v4 on an NVIDIA GPU through the CUDA ONNX
 * Runtime the installer keeps in %ProgramData%\rbstemsplus\ort-cuda (the bridge's own path: it runs
 * 20 to 24x real time on an RTX 4060 with these provider options, bridge/win GPU-BENCH.md), else the
 * CPU. model.c is compiled twice, as cpu_* (Pioneer's ONNX Runtime, --ort) and cuda_* (that
 * library, with the CUDA provider added to the session options by rbp_cuda_options); these wrappers
 * are the model.h the program sees. Any failure of the CUDA side falls back to the CPU session. */
#include "../model.h"
#include "onnxruntime_c_api.h"

void *cpu_model_open(const char *path, int threads, int ours, char *why, size_t whyn);
int cpu_model_load_ort(const char *lib, char *why, size_t whyn);
void cpu_model_close(void *m);
int cpu_model_run(void *m, const float *mix, size_t L, int ours, float **x, float **xt, size_t *F, char *why, size_t whyn);
void *cuda_model_open(const char *path, int threads, int ours, char *why, size_t whyn);
int cuda_model_load_ort(const char *lib, char *why, size_t whyn);
void cuda_model_close(void *m);
int cuda_model_run(void *m, const float *mix, size_t L, int ours, float **x, float **xt, size_t *F, char *why, size_t whyn);

/* the CUDA provider, with the bridge's options: device 0, heuristic cuDNN search, an arena sized to
 * the requests and capped at 5 GiB (it peaks at 3.9 GB; the default arena took all 8 GB of a 4060
 * and spilled into shared memory) */
OrtStatus *rbp_cuda_options(const OrtApi *api, OrtSessionOptions *o) {
    OrtCUDAProviderOptionsV2 *p = NULL;
    OrtStatus *st = api->CreateCUDAProviderOptions(&p);
    if (!st) {
        const char *keys[] = {"device_id", "cudnn_conv_algo_search", "arena_extend_strategy", "gpu_mem_limit"};
        const char *vals[] = {"0", "HEURISTIC", "kSameAsRequested", "5368709120"};
        st = api->UpdateCUDAProviderOptions(p, keys, vals, 4);
    }
    if (!st) st = api->SessionOptionsAppendExecutionProvider_CUDA_V2(o, p);
    if (!st) st = api->SetSessionGraphOptimizationLevel(o, ORT_ENABLE_ALL);
    if (p) api->ReleaseCUDAProviderOptions(p);
    return st;
}

struct model { void *impl; int cuda; };
static char cuda_path[RB_PATH_MAX];            /* the CUDA ONNX Runtime, when installed and loaded */
static int cuda_tried, cuda_ok;

static void note(const char *s, const char *detail) { fprintf(stderr, "rbsp-prepare: CUDA: %s%s%s\n", s, detail[0] ? ": " : "", detail); }

/* %ProgramData%\rbstemsplus\ort-cuda\onnxruntime.dll when it is there */
static int find_cuda(char *path, size_t n) {
    const char *t = getenv("RBSP_TEST_CUDA_PATH");
    if (getenv("RBSP_NO_CUDA")) return 0;
#ifdef RBP_TEST
    if (t) { rb_strlcpy(path, t, n); return 1; }
#endif
    (void)t;
    PWSTR w = NULL;
    char pd[RB_PATH_MAX];
    int ok = 0;
    if (SHGetKnownFolderPath(&FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, NULL, &w) == S_OK && !rb_utf8(w, -1, pd, sizeof pd)) {
        snprintf(path, n, "%s\\rbstemsplus\\ort-cuda\\onnxruntime.dll", pd);
        wchar_t *wp = rb_wide(path);
        ok = wp && GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES;
        free(wp);
    }
    CoTaskMemFree(w);
    return ok;
}

/* cuDNN loads its own parts by bare name and aborts the process when it can't find them: every DLL
 * of the folder is loaded by full path first (as the bridge does; ONNX Runtime itself is left for
 * model_load_ort) */
static void preload(const char *path) {
    wchar_t *wdir = rb_wide(path), pat[RB_PATH_MAX], full[RB_PATH_MAX];
    wchar_t *sl = wdir ? wcsrchr(wdir, L'\\') : NULL;
    WIN32_FIND_DATAW fd;
    if (!sl) { free(wdir); return; }
    *sl = 0;
    swprintf(pat, RB_PATH_MAX, L"%ls\\*.dll", wdir);
    HANDLE ff = FindFirstFileW(pat, &fd);
    if (ff != INVALID_HANDLE_VALUE) {
        do {
            if (!_wcsnicmp(fd.cFileName, L"onnxruntime", 11)) continue;
            swprintf(full, RB_PATH_MAX, L"%ls\\%ls", wdir, fd.cFileName);
            LoadLibraryExW(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        } while (FindNextFileW(ff, &fd));
        FindClose(ff);
    }
    free(wdir);
}

int model_load_ort(const char *lib, char *why, size_t whyn) {
    int r = cpu_model_load_ort(lib, why, whyn);
    if (r) return r;
    if (!cuda_tried) {
        cuda_tried = 1;
        char path[RB_PATH_MAX] = "", w2[300];
        if (find_cuda(path, sizeof path)) {
            preload(path);
            if (cuda_model_load_ort(path, w2, sizeof w2) == 0) { cuda_ok = 1; rb_strlcpy(cuda_path, path, sizeof cuda_path); }
            else note("the CUDA ONNX Runtime can't be loaded, the CPU is used", w2);
        }
    }
    return 0;
}

char rbp_model_path[RB_PATH_MAX];
int rbp_model_threads;

model *model_open(const char *path, int threads, int ours, char *why, size_t whyn) {
    model *m = calloc(1, sizeof *m);
    rb_strlcpy(rbp_model_path, path, sizeof rbp_model_path);
    rbp_model_threads = threads;
    if (!m) { snprintf(why, whyn, "out of memory"); return NULL; }
    if (ours && cuda_ok) {                               /* only our model has been run on CUDA */
        char w2[400];
        m->impl = cuda_model_open(path, threads, ours, w2, sizeof w2);
        if (m->impl) { m->cuda = 1; note("Demucs v4 runs on the GPU", ""); return m; }
        note("the CUDA session failed, the CPU is used", w2);
    }
    m->impl = cpu_model_open(path, threads, ours, why, whyn);
    if (!m->impl) { free(m); return NULL; }
    return m;
}

void model_close(model *m) {
    if (!m) return;
    if (m->cuda) cuda_model_close(m->impl); else cpu_model_close(m->impl);
    free(m);
}

int model_run_inner(model *m, const float *mix, size_t L, int ours, float **x, float **xt, size_t *F, char *why, size_t whyn) {
    if (m->cuda) {
        char w2[400];
        if (cuda_model_run(m->impl, mix, L, ours, x, xt, F, w2, sizeof w2) == 0) return 0;
        /* a CUDA run that fails (or runs out of memory): the CPU takes the rest of the run */
        note("a run failed, the CPU is used for the rest", w2);
        m->cuda = 0;
        cuda_model_close(m->impl);
        m->impl = cpu_model_open(rbp_model_path, rbp_model_threads, ours, why, whyn);
        if (!m->impl) return 1;
    }
    return cpu_model_run(m->impl, mix, L, ours, x, xt, F, why, whyn);
}
