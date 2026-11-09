/* prepare.c asks Finder's "important usage" free space and falls back to statfs when it gets
 * nothing: here it always gets nothing. */
#ifndef RBSP_WIN_COREFOUNDATION_H
#define RBSP_WIN_COREFOUNDATION_H
#include <stdint.h>
typedef void *CFURLRef, *CFTypeRef;
typedef long CFIndex;
typedef unsigned char UInt8;
typedef unsigned long CFTypeID;
#define kCFURLVolumeAvailableCapacityForImportantUsageKey 0
#define kCFNumberSInt64Type 4
typedef void *CFNumberRef;
static CFURLRef CFURLCreateFromFileSystemRepresentation(void *a, const UInt8 *p, CFIndex n, int d) { (void)a; (void)p; (void)n; (void)d; return NULL; }
static int CFURLCopyResourcePropertyForKey(CFURLRef u, int k, CFTypeRef *v, void *e) { (void)u; (void)k; (void)v; (void)e; return 0; }
static CFTypeID CFGetTypeID(CFTypeRef v) { (void)v; return 0; }
static CFTypeID CFNumberGetTypeID(void) { return 1; }
static int CFNumberGetValue(CFNumberRef n, int t, void *out) { (void)n; (void)t; (void)out; return 0; }
static void CFRelease(CFTypeRef v) { (void)v; }
#endif
