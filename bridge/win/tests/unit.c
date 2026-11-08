/* bridge/win/tests unit checks of the Windows bridge's own code, built with its source #included
 * (as bridge/tests/fuzz.c does), run by run.sh:
 *
 *   unit config FILE    reads FILE as config.ini, prints what the bridge makes of it:
 *                       "enabled=E max_gb=G max_days=D location=thismac|chosen|malformed|unreadable
 *                       [dir=DIR]" (DIR with spaces as %20), as tests/config-fixtures-windows.txt says
 *   unit marker FILE    "valid" or "invalid": the marker check (size, then marker_ok) for the
 *                       id 0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F
 *   unit sha256         SHA-256 test vectors (rb_sha256)
 *   unit irfft          rb_irfft against a direct inverse DFT, N = 4096 (rebuild_spec's)
 *   unit pathok PATH    1 or 0: location_path_ok
 */
#include "../rbstems_bridge_win.c"

static int read_file(const char *path, char **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc((size_t)len + 1);
    *n = fread(*out, 1, (size_t)len, f);
    fclose(f);
    return 0;
}

static int unit_config(const char *path) {
    config c = {.enabled = 1, .rekordbox_model = 0, .near_hits = 0, .max_bytes = 20LL << 30, .max_days = 60};
    pathf(config_path, sizeof config_path, "%s", path);
    int unreadable = read_config(&c);
    if (unreadable) { printf("location=unreadable\n"); return 0; }
    printf("enabled=%d max_gb=%g max_days=%g location=%s", c.enabled, c.max_bytes / (double)(1LL << 30), c.max_days,
           c.location < 0 ? "malformed" : c.location ? "chosen" : "thismac");
    if (c.location > 0) {
        printf(" dir=");
        for (const char *p = c.dir; *p; p++) {
            if (*p == ' ') printf("%%20");
            else putchar(*p);
        }
    }
    printf("\n");
    return 0;
}

static int unit_marker(const char *path) {
    char *b = NULL;
    size_t n = 0;
    if (read_file(path, &b, &n)) { printf("invalid\n"); return 0; }
    memcpy(cache_id, "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F", 37);
    printf("%s\n", n <= MARKER_MAX && marker_ok(b, n, cache_id) ? "valid" : "invalid");
    free(b);
    return 0;
}

static int unit_sha256(void) {
    struct { const char *in; size_t reps; const char *want; } v[] = {
        {"", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
        rb_sha256 c;
        unsigned char d[32];
        char hx[65];
        rb_sha_init(&c);
        for (size_t r = 0; r < v[i].reps; r++) rb_sha_update(&c, v[i].in, strlen(v[i].in));
        rb_sha_final(d, &c);
        hex(d, 32, hx);
        int ok = !strcmp(hx, v[i].want);
        printf("%s sha256 vector %zu\n", ok ? "ok  " : "FAIL", i);
        bad |= !ok;
    }
    return bad;
}

static int unit_irfft(void) {
    enum { N = 4096, B = N / 2 };
    static float re[B], im[B], out[N];
    static double tw[2 * N];
    uint32_t s = 12345;
    for (int k = 0; k < B; k++) {
        s = s * 1664525u + 1013904223u; re[k] = (float)((s >> 8) / 8388608.0 - 1.0);
        s = s * 1664525u + 1013904223u; im[k] = (float)((s >> 8) / 8388608.0 - 1.0);
    }
    rb_irfft(re, im, 12, out, tw);
    double worst = 0, scale = 0;
    for (int n = 0; n < N; n++) {
        /* the plain inverse sum of the Hermitian spectrum: DC re[0], Nyquist im[0], bins 1..B-1 twice */
        double x = re[0] + im[0] * (n & 1 ? -1.0 : 1.0);
        for (int k = 1; k < B; k++) {
            double a = 2.0 * 3.14159265358979323846 * k * n / N;
            x += 2.0 * (re[k] * cos(a) - im[k] * sin(a));
        }
        double e = fabs(x - out[n]);
        if (e > worst) worst = e;
        if (fabs(x) > scale) scale = fabs(x);
    }
    int ok = worst <= 1e-6 * scale + 1e-4;
    printf("%s irfft against a direct DFT: worst %.3g of %.3g\n", ok ? "ok  " : "FAIL", worst, scale);
    return !ok;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "config")) return unit_config(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "marker")) return unit_marker(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "sha256")) return unit_sha256();
    if (argc == 2 && !strcmp(argv[1], "irfft")) return unit_irfft();
    if (argc == 3 && !strcmp(argv[1], "pathok")) { printf("%d\n", location_path_ok(argv[2])); return 0; }
    fprintf(stderr, "usage: see the top of unit.c\n");
    return 2;
}
