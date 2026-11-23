#include "harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <execinfo.h>

static char **lines; static int nlines, pos;
static char history[16][96]; static int hpos;

static void fail(const char *what, const char *got)
{
    fprintf(stderr, "\nTRACE MISMATCH at line %d: %s\n  expected: %s\n  got:      %s\n  recent:\n", pos + 1, what,
            pos < nlines ? lines[pos] : "<end of trace>", got);
    for (int i = 0; i < 16; i++) { const char *h = history[(hpos + i) % 16]; if (*h) fprintf(stderr, "    %s\n", h); }
    void *bt[24]; int n = backtrace(bt, 24);                 /* which SDK function asked for it */
    fprintf(stderr, "  call stack:\n");
    backtrace_symbols_fd(bt + 2, n > 2 ? n - 2 : 0, 2);
    exit(1);
}
static void remember(const char *s) { snprintf(history[hpos], sizeof history[0], "%s", s); hpos = (hpos + 1) % 16; }
static const char *next(void)
{
    while (pos < nlines && (lines[pos][0] == ' ' || lines[pos][0] == '\0' || (lines[pos][0] == '#' && lines[pos][1] == ' ' && isdigit((unsigned char)lines[pos][2]) == 0 && strncmp(lines[pos], "# tick", 6) && strncmp(lines[pos], "# build", 7) && strchr(lines[pos], 'w') != NULL && strstr(lines[pos], "writes"))))
        pos++;                                   /* skip call lines, blanks and the summary line */
    return pos < nlines ? lines[pos] : NULL;
}
void harness_load(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    char buf[2048]; int cap = 1024; lines = malloc(cap * sizeof *lines);
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\n")] = 0;
        if (nlines == cap) lines = realloc(lines, (cap *= 2) * sizeof *lines);
        lines[nlines++] = strdup(buf);
    }
    fclose(f);
}
void harness_mark(const char *text)
{
    char got[96]; snprintf(got, sizeof got, "# %s", text);
    const char *e = next();
    if (!e || strcmp(e, got)) fail("marker", got);
    remember(got); pos++;
}
static int xfer(char kind, uint8_t dev, uint16_t reg, uint8_t *data, uint16_t n)
{
    char got[96]; int k = snprintf(got, sizeof got, "%c dev 0x%02x page 0x%02x reg 0x%02x", kind, dev, reg >> 8, reg & 0xff);
    const char *e = next();
    if (!e || strncmp(e, got, (size_t)k) || e[k] != ' ') fail("transaction", got);
    /* recorded bytes */
    const char *p = e + k; uint8_t rec[512]; int m = 0;
    while (*p && m < 512) { unsigned v; if (sscanf(p, " %2x", &v) != 1) break; rec[m++] = (uint8_t)v; p += 3; }
    if (m != n) fail("length", got);
    if (kind == 'W') { for (int i = 0; i < n; i++) if (rec[i] != data[i]) { snprintf(got + k, sizeof got - k, " %02x (expected %02x)", data[i], rec[i]); fail("write value", got); } }
    else memcpy(data, rec, n);
    remember(e); pos++;
    return 0;
}
static int rd(uint8_t dev, uint16_t reg, uint8_t *d, uint16_t n) { return xfer('R', dev, reg, d, n); }
static int wr(uint8_t dev, uint16_t reg, uint8_t *d, uint16_t n) { return xfer('W', dev, reg, d, n); }
const gsv_platform harness_platform = { rd, wr };
void harness_done(void)
{
    const char *e = next();
    if (e && e[0] != '#') fail("end of run", "<nothing>");
    printf("ok: %d trace lines replayed\n", pos);
}
