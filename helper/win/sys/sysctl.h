/* sysctlbyname("hw.activecpu"): the processors Windows lets this process use */
#ifndef RBSP_WIN_SYSCTL_H
#define RBSP_WIN_SYSCTL_H
static int sysctlbyname(const char *name, void *out, size_t *n, void *in, size_t inn) {
    (void)in; (void)inn;
    if (strcmp(name, "hw.activecpu") || *n < sizeof(int)) return -1;
    *(int *)out = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    *n = sizeof(int);
    return 0;
}
#endif
