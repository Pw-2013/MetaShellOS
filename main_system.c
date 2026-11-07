/* ============================================================================
 *  MetaShell OS 2.0.0  -  a terminal-based "operating system" simulation
 *
 *  Build (Linux / macOS):
 *    gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -o metashell main_system.c
 *  Build (Windows, MinGW-w64):
 *    gcc -std=c11 -Wall -Wextra -O2 -o metashell.exe main_system.c
 *
 *  Run options:  --fast (skip animations)  --ascii (no Unicode box drawing)
 *                --selftest (run built-in checks)  --help
 *
 *  IMPORTANT: MetaShell is an application that simulates an OS. It has no
 *  kernel, scheduler, drivers or isolation. The account system is an
 *  educational simulation: passwords are stored as salted PBKDF2-HMAC-SHA256
 *  hashes (standard algorithm, own small implementation, no dependencies),
 *  but it is not a hardened authentication system. Do not use real
 *  credentials.
 * ========================================================================== */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#else
#define _CRT_RAND_S
#define WIN32_LEAN_AND_MEAN
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_CURL
#include <curl/curl.h>
#endif
#ifndef _WIN32
#include <sys/statvfs.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <direct.h>
#include <conio.h>
#define MS_ISATTY(fd) _isatty(fd)
#define MS_MKDIR(p)   _mkdir(p)
#define MS_GETCWD     _getcwd
#define MS_CHDIR      _chdir
#define MS_RMDIR      _rmdir
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/wait.h>
#define MS_ISATTY(fd) isatty(fd)
#define MS_MKDIR(p)   mkdir((p), 0755)
#define MS_GETCWD     getcwd
#define MS_CHDIR      chdir
#define MS_RMDIR      rmdir
#endif

#if defined(__GNUC__)
#define MS_FMT(a, b) __attribute__((format(printf, a, b)))
#define MS_TFMT(a)   __attribute__((format(strftime, a, 0)))
#else
#define MS_FMT(a, b)
#define MS_TFMT(a)
#endif

/* ================= CONSTANTS ================= */

#define MS_NAME        "MetaShell OS"
#define MS_VERSION     "2.1.0"
#define MS_PATH_MAX    1024
#define MS_USER_MAX    32          /* buffer size; names are at most 31 chars */
#define UI_INNER       62          /* columns between the two frame borders   */
#define DEFAULT_THEME  10          /* original "Reset Theme" value            */
#define PASS_MIN       4
#define PASS_MAX       128
#define PBKDF2_ITERS   100000u
#define TERM_LINE_MAX  256
#define HIST_MAX       100
#define TODO_TEXT_MAX  160
#define TODO_LIMIT     2000
#define MAX_VIEW_BYTES (2 * 1024 * 1024)
#define PAGE_LINES     20
#define FM_MAX_ROWS    16

typedef enum { SYS_NONE = 0, SYS_LOGOUT, SYS_RESTART, SYS_SHUTDOWN } SysAction;
typedef enum { ST_RESET, ST_ACCENT, ST_DIM, ST_BOLD, ST_GOOD, ST_WARN, ST_BAD, ST_PATH } Style;
typedef enum { IN_OK = 0, IN_EOF, IN_TOO_LONG, IN_BAD, IN_INVALID } InStatus;

typedef struct {
    char user[MS_USER_MAX];       /* logged-in user (session)            */
    char lastUser[MS_USER_MAX];   /* remembered for the login prompt     */
    int  theme;                   /* 1..15, Windows console colour index */
    char home[MS_PATH_MAX];       /* directory MetaShell was started in  */
    char usersPath[MS_PATH_MAX];
    char cfgPath[MS_PATH_MAX];
    char todoPath[MS_PATH_MAX];
    int  tty, vt, color, unicode, fast;
    char accent[16];
    Style noticeStyle;
    char notice[200];
    int  cfgCorrupt;
    int  eof;
#ifdef _WIN32
    WORD winDefaultAttr;
    UINT oldCP, oldOutCP;
    DWORD oldOutMode;
    int  modeSaved;
#endif
} OsState;

static OsState os;

typedef struct {
    const char *tl, *tr, *bl, *br, *h, *v, *ml, *mr;
    const char *dot, *diamond, *arrow, *full, *light, *ok, *fail, *bullet;
} Glyphs;

static const Glyphs G_UNI = { "\xE2\x95\xAD", "\xE2\x95\xAE", "\xE2\x95\xB0", "\xE2\x95\xAF",
    "\xE2\x94\x80", "\xE2\x94\x82", "\xE2\x94\x9C", "\xE2\x94\xA4",
    "\xE2\x97\x8F", "\xE2\x97\x86", "\xE2\x80\xBA", "\xE2\x96\x88", "\xE2\x96\x91",
    "\xE2\x9C\x93", "\xE2\x9C\x97", "\xE2\x80\xA2" };
static const Glyphs G_ASC = { "+", "+", "+", "+", "-", "|", "+", "+",
    "*", "*", ">", "#", "-", "+", "x", "-" };
static const Glyphs *gl = &G_ASC;

/* ================= SMALL UTILITIES ================= */

static void safeCopy(char *dst, size_t n, const char *src)
{
    size_t l;
    if (n == 0) return;
    l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = '\0';
    return s;
}

static int ciCompare(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static int ciEqual(const char *a, const char *b) { return ciCompare(a, b) == 0; }

static int fits(int r, size_t n) { return r >= 0 && (size_t)r < n; }

static void secureZero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static int hasControl(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && c != '\t') || c == 0x7f) return 1;
    }
    return 0;
}

static int visibleLen(const char *s)
{
    int n = 0;
    while (*s) {
        if (*s == '\033' && s[1] == '[') {
            s += 2;
            while (*s && !(*s >= '@' && *s <= '~')) s++;
            if (*s) s++;
            continue;
        }
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
        s++;
    }
    return n;
}

/* Write text truncated/padded to exactly `width` visible columns. */
static void fmtCell(char *dst, size_t dsz, const char *text, int width)
{
    int cps = visibleLen(text), keep = cps, seen = 0, ell = 0;
    size_t o = 0;
    const char *p = text;
    if (dsz == 0) return;
    if (cps > width) { keep = width - 2; ell = 1; }
    while (*p && seen < keep) {
        size_t l = 1;
        while (((unsigned char)p[l] & 0xC0) == 0x80) l++;
        if (o + l + 1 >= dsz) break;
        memcpy(dst + o, p, l);
        o += l; p += l; seen++;
    }
    if (ell) while (seen < width && o + 2 < dsz) { dst[o++] = '.'; seen++; }
    while (seen < width && o + 1 < dsz) { dst[o++] = ' '; seen++; }
    dst[o] = '\0';
}

/* Keep the END of a long string (for paths). */
static void fitTail(char *dst, size_t dsz, const char *text, int maxcols)
{
    int cps = visibleLen(text);
    if (cps <= maxcols) { safeCopy(dst, dsz, text); return; }
    {
        int skip = cps - (maxcols - 3);
        const char *p = text;
        while (*p && skip > 0) {
            p++;
            while (((unsigned char)*p & 0xC0) == 0x80) p++;
            skip--;
        }
        if (dsz > 4) { memcpy(dst, "...", 3); safeCopy(dst + 3, dsz - 3, p); }
        else safeCopy(dst, dsz, p);
    }
}

static void fmtSize(char *out, size_t n, long long bytes)
{
    static const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    if (bytes < 1024) { snprintf(out, n, "%lld B", bytes); return; }
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    snprintf(out, n, "%.1f %s", v, unit[u]);
}

static int localTm(time_t t, struct tm *out)
{
#ifdef _WIN32
    return localtime_s(out, &t) == 0 ? 0 : -1;
#else
    return localtime_r(&t, out) ? 0 : -1;
#endif
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static void fmtTime(char *out, size_t n, const char *fmt, time_t t) MS_TFMT(3);
static void fmtTime(char *out, size_t n, const char *fmt, time_t t)
{
    struct tm tmv;
    if (localTm(t, &tmv) != 0 || strftime(out, n, fmt, &tmv) == 0)
        safeCopy(out, n, "(unknown)");
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static void extOf(const char *name, char *ext, size_t n)
{
    const char *dot = strrchr(name, '.'), *s1 = strrchr(name, '/'), *s2 = strrchr(name, '\\');
    size_t i;
    ext[0] = '\0';
    if (!dot || (s1 && dot < s1) || (s2 && dot < s2) || dot == name) return;
    for (i = 0; dot[i] && i + 1 < n; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    ext[i] = '\0';
}

static int inList(const char *ext, const char *const *list)
{
    for (; *list; list++) if (strcmp(ext, *list) == 0) return 1;
    return 0;
}
static const char *const TEXT_EXT[]  = { ".txt", ".c", ".h", ".md", ".log", NULL };
static const char *const IMAGE_EXT[] = { ".png", ".jpg", ".jpeg", ".gif", ".bmp", NULL };

/* ================= PLATFORM / TERMINAL STATE ================= */

#ifndef _WIN32
static struct termios g_savedTermios;
static volatile sig_atomic_t g_termChanged = 0;
#endif

static void platformCleanup(void)
{
    if (os.color) { fputs("\033[0m", stdout); fflush(stdout); }
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (os.tty && !os.vt) SetConsoleTextAttribute(h, os.winDefaultAttr);
        if (os.modeSaved) SetConsoleMode(h, os.oldOutMode);
        if (os.oldOutCP) SetConsoleOutputCP(os.oldOutCP);
        if (os.oldCP) SetConsoleCP(os.oldCP);
    }
#endif
}

static void onSignal(int sig)
{
    (void)sig;
#ifndef _WIN32
    if (g_termChanged) tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTermios);
    if (os.color) { ssize_t r = write(STDOUT_FILENO, "\033[0m\n", 5); (void)r; }
    _exit(130);
#else
    platformCleanup();
    ExitProcess(130);
#endif
}

#ifndef _WIN32
static int envHasUtf8(const char *name)
{
    const char *v = getenv(name);
    char buf[64];
    size_t i;
    if (!v || !*v) return 0;
    for (i = 0; v[i] && i + 1 < sizeof buf; i++) buf[i] = (char)tolower((unsigned char)v[i]);
    buf[i] = '\0';
    return strstr(buf, "utf-8") || strstr(buf, "utf8");
}
#endif

static void platformInit(int forceAscii)
{
    os.tty = MS_ISATTY(0) && MS_ISATTY(1);
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        os.winDefaultAttr = 7;
        if (GetConsoleMode(h, &mode)) {
            os.oldOutMode = mode; os.modeSaved = 1;
            if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) os.vt = 1;
            if (GetConsoleScreenBufferInfo(h, &csbi)) os.winDefaultAttr = csbi.wAttributes;
        }
        os.oldOutCP = GetConsoleOutputCP();
        os.oldCP = GetConsoleCP();
        if (SetConsoleOutputCP(CP_UTF8)) { os.unicode = 1; SetConsoleCP(CP_UTF8); }
        if (!MS_ISATTY(1)) os.vt = 0;
    }
#else
    os.vt = os.tty && getenv("TERM") && strcmp(getenv("TERM"), "dumb") != 0;
    os.unicode = envHasUtf8("LC_ALL") || envHasUtf8("LC_CTYPE") || envHasUtf8("LANG");
#endif
    os.color = os.vt && !getenv("NO_COLOR");
    if (forceAscii) os.unicode = 0;
    gl = os.unicode ? &G_UNI : &G_ASC;
    signal(SIGINT, onSignal);
    atexit(platformCleanup);
}

static void delay(int ms)
{
    if (ms <= 0 || os.fast) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
    }
#endif
}

/* ---- Colour themes ---- */

static const char *const THEME_NAMES[16] = { "Black", "Blue", "Green", "Aqua", "Red", "Purple",
    "Yellow", "White", "Gray", "Light Blue", "Light Green", "Light Aqua", "Light Red",
    "Light Purple", "Light Yellow", "Bright White" };

static void themeAnsi(int t, char *out, size_t n)
{
    static const int map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };  /* Windows index -> ANSI */
    snprintf(out, n, "\033[%dm", ((t >> 3) & 1 ? 90 : 30) + map[t & 7]);
}

static void setTheme(void)
{
    themeAnsi(os.theme, os.accent, sizeof os.accent);
#ifdef _WIN32
    if (os.tty && !os.vt)
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)os.theme);
#endif
}

static const char *sty(Style s)
{
    if (!os.color) return "";
    switch (s) {
        case ST_ACCENT: return os.accent;
        case ST_DIM:    return "\033[2m";
        case ST_BOLD:   return "\033[1m";
        case ST_GOOD:   return "\033[92m";
        case ST_WARN:   return "\033[93m";
        case ST_BAD:    return "\033[91m";
        case ST_PATH:   return "\033[94m";
        default:        return "\033[0m";
    }
}

static void clear(void)
{
    if (!os.tty) return;
    if (os.vt) { fputs("\033[2J\033[H", stdout); return; }
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        DWORD cells, written;
        COORD home = { 0, 0 };
        if (GetConsoleScreenBufferInfo(h, &csbi)) {
            cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
            FillConsoleOutputCharacterA(h, ' ', cells, home, &written);
            FillConsoleOutputAttribute(h, csbi.wAttributes, cells, home, &written);
            SetConsoleCursorPosition(h, home);
        }
    }
#endif
}

static void cursorHome(void)
{
    if (os.vt) fputs("\033[H", stdout); else clear();
}

/* ---- Notices (status line shown under the next menu) ---- */

static void setNotice(Style st, const char *fmt, ...) MS_FMT(2, 3);
static void setNotice(Style st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(os.notice, sizeof os.notice, fmt, ap);
    va_end(ap);
    os.noticeStyle = st;
}

static void showNotice(void)
{
    if (!os.notice[0]) return;
    {
        const char *icon = os.noticeStyle == ST_GOOD ? gl->ok :
                           os.noticeStyle == ST_BAD ? gl->fail : "!";
        printf("  %s%s %s%s\n", sty(os.noticeStyle), icon, os.notice, sty(ST_RESET));
    }
    os.notice[0] = '\0';
}

/* ================= FRAME DRAWING ================= */

static void repeat(const char *s, int n) { while (n-- > 0) fputs(s, stdout); }

static void boxEdge(const char *l, const char *r)
{
    printf("%s%s", sty(ST_ACCENT), l);
    repeat(gl->h, UI_INNER);
    printf("%s%s\n", r, sty(ST_RESET));
}
static void boxTop(void)    { boxEdge(gl->tl, gl->tr); }
static void boxMid(void)    { boxEdge(gl->ml, gl->mr); }
static void boxBottom(void) { boxEdge(gl->bl, gl->br); }

static void boxRow(const char *text)
{
    int pad = UI_INNER - 2 - visibleLen(text);
    if (pad < 0) pad = 0;
    printf("%s%s%s %s%*s %s%s%s\n", sty(ST_ACCENT), gl->v, sty(ST_RESET), text, pad, "",
           sty(ST_ACCENT), gl->v, sty(ST_RESET));
}

static void boxRow2(const char *left, const char *right)
{
    int pad = UI_INNER - 2 - visibleLen(left) - visibleLen(right);
    if (pad < 1) pad = 1;
    printf("%s%s%s %s%*s%s %s%s%s\n", sty(ST_ACCENT), gl->v, sty(ST_RESET), left, pad, "", right,
           sty(ST_ACCENT), gl->v, sty(ST_RESET));
}

static void menuItem(char *out, size_t n, int num, const char *label, int width)
{
    char cell[96];
    fmtCell(cell, sizeof cell, label, width);
    snprintf(out, n, "%s%2d%s  %s", sty(ST_ACCENT), num, sty(ST_RESET), cell);
}

static void appHeaderEx(const char *title, int doClear)
{
    char l[160], r[96];
    if (doClear) clear(); else cursorHome();
    snprintf(l, sizeof l, "%s%s %s%s%s", sty(ST_ACCENT), gl->diamond, sty(ST_BOLD), title, sty(ST_RESET));
    snprintf(r, sizeof r, "%s%s%s", sty(ST_DIM), os.user, sty(ST_RESET));
    boxTop();
    boxRow2(l, r);
    boxBottom();
}
static void appHeader(const char *title) { appHeaderEx(title, 1); }

static void printLogo(void)
{
    printf("%s", sty(ST_ACCENT));
    printf(" __  __      _        ____  _          _ _ \n");
    printf("|  \\/  | ___| |_ __ _/ ___|| |__   ___| | |\n");
    printf("| |\\/| |/ _ \\ __/ _` \\___ \\| '_ \\ / _ \\ | |\n");
    printf("| |  | |  __/ || (_| |___) | | | |  __/ | |\n");
    printf("|_|  |_|\\___|\\__\\__,_|____/|_| |_|\\___|_|_|\n");
    printf("%s", sty(ST_RESET));
}

/* ================= INPUT SYSTEM ================= */

/* Reads one line safely. Strips the newline, drains over-long lines, rejects
 * control characters. Never leaves stray characters in the stdin buffer. */
static InStatus readInput(char *buf, size_t size)
{
    size_t len;
    int truncated = 0;
    fflush(stdout);
    if (size < 2 || !fgets(buf, (int)size, stdin)) {
        if (size > 0) buf[0] = '\0';
        os.eof = 1;
        return IN_EOF;
    }
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    } else if (!feof(stdin)) {
        int c = getchar();
        if (c != '\n' && c != EOF) {
            truncated = 1;
            while (c != '\n' && c != EOF) c = getchar();
        }
    }
    if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
    if (truncated) return IN_TOO_LONG;
    if (hasControl(buf)) return IN_BAD;
    return IN_OK;
}

static InStatus readLine(const char *prompt, char *buf, size_t size)
{
    if (prompt) printf("%s%s%s", sty(ST_ACCENT), prompt, sty(ST_RESET));
    return readInput(buf, size);
}

static void inputProblem(InStatus st, size_t limit)
{
    if (st == IN_TOO_LONG) setNotice(ST_BAD, "Input too long (limit %llu characters).", (unsigned long long)limit);
    else if (st == IN_BAD) setNotice(ST_BAD, "Input contains unsupported control characters.");
}

static InStatus readChoice(const char *prompt, int lo, int hi, int *out)
{
    char buf[32], *t, *end;
    long v;
    InStatus st = readLine(prompt, buf, sizeof buf);
    if (st == IN_EOF) return st;
    if (st != IN_OK) { inputProblem(st, sizeof buf - 1); return IN_INVALID; }
    t = trim(buf);
    if (!*t) { setNotice(ST_WARN, "Please enter a number from %d to %d.", lo, hi); return IN_INVALID; }
    errno = 0;
    v = strtol(t, &end, 10);
    if (*end || errno) { setNotice(ST_BAD, "\"%s\" is not a valid number.", t); return IN_INVALID; }
    if (v < lo || v > hi) { setNotice(ST_BAD, "Invalid choice. Pick a number from %d to %d.", lo, hi); return IN_INVALID; }
    *out = (int)v;
    return IN_OK;
}

/* Optional integer with default (empty input -> def). */
static InStatus readLongLong(const char *prompt, long long def, long long lo, long long hi, long long *out)
{
    char buf[48], *t, *end;
    long long v;
    InStatus st = readLine(prompt, buf, sizeof buf);
    if (st == IN_EOF) return st;
    if (st != IN_OK) { inputProblem(st, sizeof buf - 1); return IN_INVALID; }
    t = trim(buf);
    if (!*t) { *out = def; return IN_OK; }
    errno = 0;
    v = strtoll(t, &end, 10);
    if (*end || errno) { setNotice(ST_BAD, "\"%s\" is not a valid whole number.", t); return IN_INVALID; }
    if (v < lo || v > hi) { setNotice(ST_BAD, "Value must be between %lld and %lld.", lo, hi); return IN_INVALID; }
    *out = v;
    return IN_OK;
}

static int confirm(const char *question)
{
    char buf[32], *t;
    char prompt[160];
    snprintf(prompt, sizeof prompt, "  %s [y/N]: ", question);
    if (readLine(prompt, buf, sizeof buf) != IN_OK) return 0;
    t = trim(buf);
    return ciEqual(t, "y") || ciEqual(t, "yes");
}

static void pauseProgram(void)
{
    char b[256];
    printf("\n");
    readLine("  Press ENTER to continue...", b, sizeof b);
}

static InStatus readPassword(const char *prompt, char *buf, size_t size)
{
    InStatus st;
    int changed = 0;
    printf("%s%s%s", sty(ST_ACCENT), prompt, sty(ST_RESET));
    fflush(stdout);
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode) && SetConsoleMode(h, mode & ~(DWORD)ENABLE_ECHO_INPUT)) changed = 1;
        st = readInput(buf, size);
        if (changed) { SetConsoleMode(h, mode); printf("\n"); }
    }
#else
    {
        struct termios nt;
        if (MS_ISATTY(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_savedTermios) == 0) {
            nt = g_savedTermios;
            nt.c_lflag &= ~(tcflag_t)ECHO;
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &nt) == 0) { changed = 1; g_termChanged = 1; }
        }
        st = readInput(buf, size);
        if (changed) { tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTermios); g_termChanged = 0; printf("\n"); }
    }
#endif
    return st;
}

/* Wait up to `ms` for the user to press a key / ENTER. Returns 1 if input arrived. */
static int waitInput(int ms)
{
#ifdef _WIN32
    int waited;
    for (waited = 0; waited < ms; waited += 50) {
        if (_kbhit()) { while (_kbhit()) (void)_getch(); return 1; }
        Sleep(50);
    }
    return 0;
#else
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (suseconds_t)(ms % 1000) * 1000;
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        char junk[64];
        readInput(junk, sizeof junk);
        return 1;
    }
    return 0;
#endif
}

/* ================= RANDOMNESS ================= */

static int secureBytes(unsigned char *buf, size_t n)
{
#ifdef _WIN32
    size_t i = 0;
    while (i < n) {
        unsigned int v;
        size_t k, take = n - i < 4 ? n - i : 4;
        if (rand_s(&v) != 0) return -1;
        for (k = 0; k < take; k++) buf[i + k] = (unsigned char)(v >> (8 * k));
        i += take;
    }
    return 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (!f) return -1;
    got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
#endif
}

static uint64_t weakState = 0;
static uint64_t weakNext(void)
{
    if (!weakState) weakState = ((uint64_t)time(NULL) << 20) ^ (uint64_t)clock() ^ 0x9E3779B97F4A7C15ULL;
    weakState ^= weakState >> 12; weakState ^= weakState << 25; weakState ^= weakState >> 27;
    return weakState * 0x2545F4914F6CDD1DULL;
}

/* Uniform value in [0, bound). If needSecure, fails rather than falling back
 * to a non-cryptographic generator. */
static int randomBelow(uint64_t bound, int needSecure, uint64_t *out)
{
    uint64_t lim, v;
    unsigned char b[8];
    int i;
    if (bound == 0) return -1;
    lim = UINT64_MAX - (UINT64_MAX % bound);
    for (;;) {
        if (secureBytes(b, sizeof b) == 0) {
            v = 0;
            for (i = 0; i < 8; i++) v = (v << 8) | b[i];
        } else if (!needSecure) {
            v = weakNext();
        } else {
            return -1;
        }
        if (v < lim) { *out = v % bound; return 0; }
    }
}

/* ================= SHA-256 / PBKDF2 (standard algorithms) ================= */

typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t blen; } Sha256;

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256Block(Sha256 *c, const unsigned char *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15], 7) ^ ROR32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR32(w[i-2], 17) ^ ROR32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256Init(Sha256 *c)
{
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0; c->blen = 0;
}

static void sha256Update(Sha256 *c, const unsigned char *d, size_t n)
{
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->blen;
        if (take > n) take = n;
        memcpy(c->buf + c->blen, d, take);
        c->blen += take; d += take; n -= take;
        if (c->blen == 64) { sha256Block(c, c->buf); c->blen = 0; }
    }
}

static void sha256Final(Sha256 *c, unsigned char out[32])
{
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    int i;
    sha256Update(c, &pad, 1);
    while (c->blen != 56) sha256Update(c, &zero, 1);
    for (i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256Update(c, lenb, 8);
    for (i = 0; i < 8; i++) {
        out[4*i] = (unsigned char)(c->h[i] >> 24); out[4*i+1] = (unsigned char)(c->h[i] >> 16);
        out[4*i+2] = (unsigned char)(c->h[i] >> 8); out[4*i+3] = (unsigned char)c->h[i];
    }
}

static void pbkdf2Sha256(const unsigned char *pw, size_t pwlen, const unsigned char *salt,
                         size_t saltlen, uint32_t iters, unsigned char out[32])
{
    unsigned char key[64], ip[64], op[64], u[32], t[32], cnt[4] = { 0, 0, 0, 1 };
    Sha256 inner, outer, c;
    uint32_t i;
    size_t k;
    memset(key, 0, sizeof key);
    if (pwlen > 64) { sha256Init(&c); sha256Update(&c, pw, pwlen); sha256Final(&c, key); }
    else memcpy(key, pw, pwlen);
    for (k = 0; k < 64; k++) { ip[k] = key[k] ^ 0x36; op[k] = key[k] ^ 0x5c; }
    sha256Init(&inner); sha256Update(&inner, ip, 64);
    sha256Init(&outer); sha256Update(&outer, op, 64);
    c = inner; sha256Update(&c, salt, saltlen); sha256Update(&c, cnt, 4); sha256Final(&c, t);
    c = outer; sha256Update(&c, t, 32); sha256Final(&c, u);
    memcpy(out, u, 32);
    for (i = 1; i < iters; i++) {
        c = inner; sha256Update(&c, u, 32); sha256Final(&c, t);
        c = outer; sha256Update(&c, t, 32); sha256Final(&c, u);
        for (k = 0; k < 32; k++) out[k] ^= u[k];
    }
    secureZero(key, sizeof key); secureZero(ip, sizeof ip); secureZero(op, sizeof op);
    secureZero(u, sizeof u); secureZero(t, sizeof t);
}

static void toHex(const unsigned char *b, size_t n, char *out)
{
    static const char *hx = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) { out[2*i] = hx[b[i] >> 4]; out[2*i+1] = hx[b[i] & 15]; }
    out[2*n] = '\0';
}

static int fromHex(const char *s, unsigned char *out, size_t n)
{
    size_t i;
    if (strlen(s) != 2 * n) return -1;
    for (i = 0; i < 2 * n; i++) if (!isxdigit((unsigned char)s[i])) return -1;
    for (i = 0; i < n; i++) {
        char pair[3] = { s[2*i], s[2*i+1], 0 };
        out[i] = (unsigned char)strtoul(pair, NULL, 16);
    }
    return 0;
}

#define HASH_PREFIX "pbkdf2-sha256$"

static int hashPassword(const char *pw, char *out, size_t n)
{
    unsigned char salt[16], dk[32];
    char sh[33], dh[65];
    if (secureBytes(salt, sizeof salt) != 0) return -1;
    pbkdf2Sha256((const unsigned char *)pw, strlen(pw), salt, sizeof salt, PBKDF2_ITERS, dk);
    toHex(salt, sizeof salt, sh); toHex(dk, sizeof dk, dh);
    secureZero(dk, sizeof dk);
    return fits(snprintf(out, n, HASH_PREFIX "%u$%s$%s", PBKDF2_ITERS, sh, dh), n) ? 0 : -1;
}

static int parseHashField(const char *enc, uint32_t *iters, unsigned char salt[16], unsigned char dk[32])
{
    char tmp[200], *p, *s1, *s2;
    unsigned long it;
    if (strncmp(enc, HASH_PREFIX, strlen(HASH_PREFIX)) != 0) return -1;
    if (strlen(enc) >= sizeof tmp) return -1;
    safeCopy(tmp, sizeof tmp, enc + strlen(HASH_PREFIX));
    s1 = strchr(tmp, '$'); if (!s1) return -1;
    *s1++ = '\0';
    s2 = strchr(s1, '$'); if (!s2) return -1;
    *s2++ = '\0';
    it = strtoul(tmp, &p, 10);
    if (*p || it < 1000 || it > 5000000UL) return -1;
    if (fromHex(s1, salt, 16) != 0 || fromHex(s2, dk, 32) != 0) return -1;
    *iters = (uint32_t)it;
    return 0;
}

static int verifyHash(const char *pw, const char *enc)
{
    unsigned char salt[16], want[32], got[32];
    uint32_t it;
    unsigned char diff = 0;
    size_t i;
    if (parseHashField(enc, &it, salt, want) != 0) return 0;
    pbkdf2Sha256((const unsigned char *)pw, strlen(pw), salt, sizeof salt, it, got);
    for (i = 0; i < 32; i++) diff |= (unsigned char)(got[i] ^ want[i]);
    secureZero(got, sizeof got);
    return diff == 0;
}

static int ctEqualStr(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), i;
    unsigned char d = (unsigned char)(la != lb);
    for (i = 0; i < la && i < lb; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* ================= FILESYSTEM HELPERS ================= */

static int pathExists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int pathIsDir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int pathIsFile(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static int pathJoin(char *out, size_t n, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    int r;
    if (dl > 0 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) r = snprintf(out, n, "%s%s", dir, name);
    else r = snprintf(out, n, "%s/%s", dir, name);
    return fits(r, n) ? 0 : -1;
}

static int absPath(const char *p, char *out, size_t n)
{
#ifdef _WIN32
    return _fullpath(out, p, n) ? 0 : -1;
#else
    char tmp[4096];
    char cwd[MS_PATH_MAX];
    if (realpath(p, tmp)) return fits(snprintf(out, n, "%s", tmp), n) ? 0 : -1;
    if (p[0] == '/') return fits(snprintf(out, n, "%s", p), n) ? 0 : -1;
    if (!MS_GETCWD(cwd, sizeof cwd)) return -1;
    return fits(snprintf(out, n, "%s/%s", cwd, p), n) ? 0 : -1;
#endif
}

static int replaceFile(const char *tmp, const char *dst)
{
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(tmp, dst);
#endif
}

/* Copies src to a NEW file dst (refuses to overwrite). errno is preserved on failure. */
static int copyFileSafe(const char *src, const char *dst)
{
    FILE *in, *out;
    unsigned char buf[65536];
    size_t n;
    int e = 0;
    if (pathExists(dst)) { errno = EEXIST; return -1; }
    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { e = errno; fclose(in); errno = e; return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { e = errno ? errno : EIO; break; }
    }
    if (!e && ferror(in)) e = errno ? errno : EIO;
    fclose(in);
    if (fclose(out) != 0 && !e) e = errno ? errno : EIO;
    if (e) { remove(dst); errno = e; return -1; }
    return 0;
}

static int pathHasPrefix(const char *path, const char *prefix)
{
    size_t pl = strlen(prefix), i;
    for (i = 0; i < pl; i++) {
        char a = path[i], b = prefix[i];
#ifdef _WIN32
        a = (char)tolower((unsigned char)a); b = (char)tolower((unsigned char)b);
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
#endif
        if (!a || a != b) return 0;
    }
    return path[pl] == '\0' || path[pl] == '/' || path[pl] == '\\';
}

/* Files MetaShell depends on, and OS directories, must not be deleted/renamed. */
static int isProtectedPath(const char *path)
{
    char abs[MS_PATH_MAX], other[MS_PATH_MAX];
    const char *sys[] = {
#ifdef _WIN32
        "C:/Windows", "C:/Program Files", "C:/Program Files (x86)",
#else
        "/bin", "/boot", "/dev", "/etc", "/lib", "/lib64", "/proc", "/sbin", "/sys", "/usr", "/var", "/root",
#endif
        NULL };
    const char *own[3];
    int i;
    if (absPath(path, abs, sizeof abs) != 0) return 1;           /* fail safe */
    if (strlen(abs) <= 3) return 1;                              /* filesystem root */
    own[0] = os.usersPath; own[1] = os.cfgPath; own[2] = os.home;
    for (i = 0; i < 3; i++) {
        if (absPath(own[i], other, sizeof other) == 0) {
            size_t la = strlen(abs), lo = strlen(other);
            if (la == lo && pathHasPrefix(abs, other)) return 1;
        }
    }
    {
        char bak[MS_PATH_MAX];
        if (fits(snprintf(bak, sizeof bak, "%s.bak", os.cfgPath), sizeof bak) &&
            absPath(bak, other, sizeof other) == 0 && strlen(abs) == strlen(other) && pathHasPrefix(abs, other))
            return 1;
    }
    for (i = 0; sys[i]; i++) if (pathHasPrefix(abs, sys[i])) return 1;
    return 0;
}

/* Name validation for new files / folders / rename targets (single path component). */
static int validLeafName(const char *name, char *why, size_t whySz)
{
    size_t l = strlen(name), i;
    static const char *reserved[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "LPT1", "LPT2", "LPT3", NULL };
    if (l == 0) { safeCopy(why, whySz, "name is empty"); return 0; }
    if (l > 200) { safeCopy(why, whySz, "name is too long (max 200)"); return 0; }
    if (!strcmp(name, ".") || !strcmp(name, "..")) { safeCopy(why, whySz, "reserved name"); return 0; }
    for (i = 0; i < l; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            safeCopy(why, whySz, "name contains characters that are not allowed ( / \\ : * ? \" < > | )");
            return 0;
        }
    }
    if (name[l - 1] == ' ' || name[l - 1] == '.') { safeCopy(why, whySz, "name cannot end with a space or dot"); return 0; }
    for (i = 0; reserved[i]; i++) {
        char stem[16];
        size_t k;
        for (k = 0; name[k] && name[k] != '.' && k + 1 < sizeof stem; k++) stem[k] = (char)toupper((unsigned char)name[k]);
        stem[k] = '\0';
        if (!strcmp(stem, reserved[i])) { safeCopy(why, whySz, "reserved device name"); return 0; }
    }
    return 1;
}

static int validPathArg(const char *p)
{
    size_t l = strlen(p);
    return l > 0 && l < MS_PATH_MAX - 8 && !hasControl(p);
}

static void normSlashes(char *s) { for (; *s; s++) if (*s == '\\') *s = '/'; }

/* Prompt-style path: "~" for the MetaShell home folder, "~/sub" below it. */
static void displayPath(char *out, size_t n)
{
    char cwd[MS_PATH_MAX], home[MS_PATH_MAX];
    size_t hl;
    if (!MS_GETCWD(cwd, sizeof cwd)) { safeCopy(out, n, "?"); return; }
    safeCopy(home, sizeof home, os.home);
    normSlashes(cwd); normSlashes(home);
    hl = strlen(home);
    if (pathHasPrefix(cwd, home)) {
        if (cwd[hl] == '\0') safeCopy(out, n, "~");
        else snprintf(out, n, "~%s", cwd + hl);
    } else {
        safeCopy(out, n, cwd);
    }
}

/* ---- Directory scanning (shared by file manager and terminal) ---- */

typedef struct {
    char name[512];
    long long size;
    time_t mtime;
    int isDir, hidden, statOk;
} FsEntry;
typedef struct { FsEntry *items; size_t count, cap; } FsList;

static void fsFree(FsList *l) { free(l->items); l->items = NULL; l->count = l->cap = 0; }

static int fsCompare(const void *a, const void *b)
{
    const FsEntry *x = (const FsEntry *)a, *y = (const FsEntry *)b;
    if (x->isDir != y->isDir) return y->isDir - x->isDir;
    return ciCompare(x->name, y->name);
}

static int fsScan(const char *dir, int showHidden, FsList *out, int *err)
{
    DIR *d;
    struct dirent *de;
    out->items = NULL; out->count = out->cap = 0;
    d = opendir(dir);
    if (!d) { *err = errno; return -1; }
    while ((de = readdir(d)) != NULL) {
        FsEntry e;
        char full[MS_PATH_MAX];
        struct stat st;
        size_t i;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        memset(&e, 0, sizeof e);
        safeCopy(e.name, sizeof e.name, de->d_name);
        for (i = 0; e.name[i]; i++)
            if ((unsigned char)e.name[i] < 0x20 || e.name[i] == 0x7f) e.name[i] = '?';
        e.hidden = e.name[0] == '.';
        if (pathJoin(full, sizeof full, dir, de->d_name) == 0) {
#ifdef _WIN32
            DWORD at = GetFileAttributesA(full);
            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_HIDDEN)) e.hidden = 1;
#endif
            if (stat(full, &st) == 0) {
                e.statOk = 1; e.isDir = S_ISDIR(st.st_mode);
                e.size = (long long)st.st_size; e.mtime = st.st_mtime;
            }
        }
        if (e.hidden && !showHidden) continue;
        if (out->count == out->cap) {
            size_t nc = out->cap ? out->cap * 2 : 64;
            FsEntry *ni = (FsEntry *)realloc(out->items, nc * sizeof *ni);
            if (!ni) { closedir(d); fsFree(out); *err = ENOMEM; return -1; }
            out->items = ni; out->cap = nc;
        }
        out->items[out->count++] = e;
    }
    closedir(d);
    if (out->count > 1) qsort(out->items, out->count, sizeof *out->items, fsCompare);
    return 0;
}

static const char *fileTypeLabel(const FsEntry *e)
{
    char ext[16];
    if (!e->statOk) return "no access";
    if (e->isDir) return "folder";
    extOf(e->name, ext, sizeof ext);
    if (inList(ext, TEXT_EXT)) return "text";
    if (inList(ext, IMAGE_EXT)) return "image";
    if (!strcmp(ext, ".exe")) return "program";
    return "file";
}

/* Safely print a text file (control characters replaced). Returns 0 / -1. */
static int viewTextFile(const char *path, int paged)
{
    FILE *f = fopen(path, "rb");
    unsigned char buf[4096];
    size_t n, i;
    long long total = 0;
    int lines = 0, stop = 0, lastNl = 1, any = 0;
    if (!f) { printf("  %scannot open '%s': %s%s\n", sty(ST_BAD), path, strerror(errno), sty(ST_RESET)); return -1; }
    while (!stop && (n = fread(buf, 1, sizeof buf, f)) > 0) {
        for (i = 0; i < n && !stop; i++) {
            unsigned char c = buf[i];
            if (c == 0) { printf("\n  %s(binary data - not displayed)%s\n", sty(ST_WARN), sty(ST_RESET)); stop = 1; lastNl = 1; break; }
            any = 1;
            if (c == '\n' || c == '\t') putchar(c);
            else if (c == '\r') continue;
            else if (c < 0x20 || c == 0x7f) putchar('.');
            else putchar(c);
            lastNl = (c == '\n');
            total++;
            if (total >= MAX_VIEW_BYTES) {
                printf("\n  %s(output truncated at %d MB)%s\n", sty(ST_WARN), MAX_VIEW_BYTES / (1024 * 1024), sty(ST_RESET));
                stop = 1; lastNl = 1; break;
            }
            if (c == '\n' && paged && os.tty && ++lines >= PAGE_LINES) {
                char ans[16];
                lines = 0;
                if (readLine("  -- more -- ENTER: next page, q: stop ", ans, sizeof ans) != IN_OK || ciEqual(trim(ans), "q")) stop = 1;
            }
        }
    }
    if (ferror(f)) printf("\n  %sread error: %s%s\n", sty(ST_BAD), strerror(errno), sty(ST_RESET));
    fclose(f);
    if (!any) printf("  %s(empty file)%s\n", sty(ST_DIM), sty(ST_RESET));
    else if (!lastNl) putchar('\n');
    return 0;
}

/* ================= FILE OPENING (shared by File Manager and `open`) ================= */

static int openFile(const char *name, int fromTerminal)
{
    char ext[16];
    int rc = 0;
    if (!validPathArg(name)) { printf("  %sInvalid file name.%s\n", sty(ST_BAD), sty(ST_RESET)); rc = -1; goto done; }
    if (!pathExists(name)) { printf("  %s'%s' does not exist.%s\n", sty(ST_BAD), name, sty(ST_RESET)); rc = -1; goto done; }
    if (pathIsDir(name)) { printf("  %s'%s' is a folder. Use Open Folder / cd.%s\n", sty(ST_WARN), name, sty(ST_RESET)); rc = -1; goto done; }
    extOf(name, ext, sizeof ext);
    if (inList(ext, TEXT_EXT)) {
        if (!fromTerminal) {
            char t[96];
            snprintf(t, sizeof t, "Viewing: %s", name);
            appHeader(t);
            printf("\n");
        }
        rc = viewTextFile(name, !fromTerminal);
    } else if (inList(ext, IMAGE_EXT)) {
        printf("  %sImage preview is not available in the terminal UI; no external viewer will be opened.%s\n", sty(ST_WARN), sty(ST_RESET));
        rc = -1;
    } else if (!strcmp(ext, ".exe")) {
        printf("  %sExecutable launching is disabled: MetaShell only runs built-in apps.%s\n", sty(ST_WARN), sty(ST_RESET));
        rc = -1;
    } else {
        printf("  %sUnsupported file type. Supported: .txt .c .h .md .log, images%s\n", sty(ST_WARN), sty(ST_RESET));
        rc = -1;
    }
done:
    if (!fromTerminal) pauseProgram();
    return rc;
}

/* ================= CONFIG ================= */

static int dbNameOk(const char *s)
{
    size_t l = strlen(s), i;
    if (l == 0 || l >= MS_USER_MAX) return 0;
    for (i = 0; i < l; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int validUsernameStrict(const char *s, char *why, size_t whySz)
{
    size_t l = strlen(s), i;
    if (l == 0) { safeCopy(why, whySz, "Username cannot be empty."); return 0; }
    if (l < 3 || l > 24) { safeCopy(why, whySz, "Username must be 3-24 characters long."); return 0; }
    for (i = 0; i < l; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) {
            safeCopy(why, whySz, "Username may only use letters, digits, '_', '-' and '.'.");
            return 0;
        }
    }
    return 1;
}

static void configLoad(void)
{
    FILE *f = fopen(os.cfgPath, "r");
    char line[512];
    int bad = 0;
    if (!f) {
        if (errno != ENOENT) setNotice(ST_WARN, "Cannot read settings (%s). Using defaults.", strerror(errno));
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *t, *eq, *key, *val, *end;
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] != '\n' && !feof(f)) {            /* over-long line */
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF) { }
            bad = 1; continue;
        }
        t = trim(line);
        if (!*t || *t == '#') continue;
        eq = strchr(t, '=');
        if (!eq) { bad = 1; continue; }
        *eq = '\0';
        key = trim(t); val = trim(eq + 1);
        if (!strcmp(key, "theme")) {
            long v = strtol(val, &end, 10);
            if (*val && !*end && v >= 1 && v <= 15) os.theme = (int)v; else bad = 1;
        } else if (!strcmp(key, "username")) {
            if (dbNameOk(val)) safeCopy(os.lastUser, sizeof os.lastUser, val); else bad = 1;
        }
    }
    if (ferror(f)) bad = 1;
    fclose(f);
    if (bad) {
        os.cfgCorrupt = 1;
        setNotice(ST_WARN, "Settings file is damaged; defaults used for bad values. A backup is made before it is rewritten.");
    }
}

static int configSave(void)
{
    char tmp[MS_PATH_MAX + 8], bak[MS_PATH_MAX + 8];
    FILE *f;
    if (os.cfgCorrupt) {
        snprintf(bak, sizeof bak, "%s.bak", os.cfgPath);
        remove(bak);
        if (pathExists(os.cfgPath) && copyFileSafe(os.cfgPath, bak) != 0) return -1;   /* never overwrite without a backup */
        setNotice(ST_WARN, "Damaged settings were backed up to metashell.cfg.bak");
        os.cfgCorrupt = 0;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", os.cfgPath);
    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# MetaShell OS settings\nusername=%s\ntheme=%d\n", os.lastUser, os.theme);
    if (ferror(f)) { fclose(f); remove(tmp); return -1; }
    if (fclose(f) != 0) { remove(tmp); return -1; }
    if (replaceFile(tmp, os.cfgPath) != 0) { remove(tmp); return -1; }
    return 0;
}

/* ================= ACCOUNTS ================= */

typedef enum { U_INVALID = 0, U_LEGACY, U_HASHED } UserKind;
typedef struct {
    char name[MS_USER_MAX];
    char secret[200];            /* hash field, or legacy plaintext password */
    char raw[512];               /* original text of unparseable lines       */
    UserKind kind;
} UserRec;
typedef struct { UserRec *items; size_t count, cap; } UserDb;

static void dbFree(UserDb *db) { free(db->items); db->items = NULL; db->count = db->cap = 0; }

static UserRec *dbPush(UserDb *db)
{
    if (db->count == db->cap) {
        size_t nc = db->cap ? db->cap * 2 : 16;
        UserRec *n = (UserRec *)realloc(db->items, nc * sizeof *n);
        if (!n) return NULL;
        db->items = n; db->cap = nc;
    }
    memset(&db->items[db->count], 0, sizeof(UserRec));
    return &db->items[db->count++];
}

/* Returns 0 ok, 1 file missing (empty db), -1 error. */
static int dbLoad(UserDb *db)
{
    FILE *f;
    char line[512];
    db->items = NULL; db->count = db->cap = 0;
    f = fopen(os.usersPath, "r");
    if (!f) return errno == ENOENT ? 1 : -1;
    while (fgets(line, sizeof line, f)) {
        UserRec *r;
        char *t, *a, *b, *rest;
        size_t l = strlen(line);
        int toolong = (l > 0 && line[l - 1] != '\n' && !feof(f));
        if (toolong) { int c; while ((c = fgetc(f)) != '\n' && c != EOF) { } }
        t = trim(line);
        if (!*t) continue;
        r = dbPush(db);
        if (!r) { fclose(f); dbFree(db); errno = ENOMEM; return -1; }
        safeCopy(r->raw, sizeof r->raw, t);
        r->kind = U_INVALID;
        if (toolong) continue;
        a = t;
        b = a; while (*b && *b != ' ' && *b != '\t') b++;
        if (!*b) continue;
        *b++ = '\0';
        while (*b == ' ' || *b == '\t') b++;
        rest = b; while (*rest && *rest != ' ' && *rest != '\t') rest++;
        if (*rest) { *rest++ = '\0'; while (*rest == ' ' || *rest == '\t') rest++; if (*rest) continue; }
        if (!dbNameOk(a) || !*b || strlen(b) >= sizeof r->secret) continue;
        safeCopy(r->name, sizeof r->name, a);
        safeCopy(r->secret, sizeof r->secret, b);
        if (!strncmp(b, HASH_PREFIX, strlen(HASH_PREFIX))) {
            unsigned char s[16], d[32]; uint32_t it;
            r->kind = parseHashField(b, &it, s, d) == 0 ? U_HASHED : U_INVALID;
        } else {
            r->kind = U_LEGACY;
        }
    }
    {
        int bad = ferror(f);
        fclose(f);
        if (bad) { dbFree(db); return -1; }
    }
    return 0;
}

static int dbSave(const UserDb *db)
{
    char tmp[MS_PATH_MAX + 8];
    FILE *f;
    size_t i;
    snprintf(tmp, sizeof tmp, "%s.tmp", os.usersPath);
    f = fopen(tmp, "w");
    if (!f) return -1;
    for (i = 0; i < db->count; i++) {
        const UserRec *r = &db->items[i];
        if (r->kind == U_INVALID) fprintf(f, "%s\n", r->raw);
        else fprintf(f, "%s %s\n", r->name, r->secret);
    }
    if (ferror(f)) { fclose(f); remove(tmp); return -1; }
    if (fclose(f) != 0) { remove(tmp); return -1; }
    if (replaceFile(tmp, os.usersPath) != 0) { remove(tmp); return -1; }
    return 0;
}

static UserRec *dbFind(UserDb *db, const char *name)
{
    size_t i;
    for (i = 0; i < db->count; i++)
        if (db->items[i].kind != U_INVALID && ciEqual(db->items[i].name, name)) return &db->items[i];
    return NULL;
}

/* ---- Login screen / account flows ---- */

static void createAccount(void)
{
    char user[64], p1[PASS_MAX + 2], p2[PASS_MAX + 2], why[128], enc[200], *u;
    UserDb db;
    UserRec *r;
    int lr;
    InStatus st;

    appHeader("Create Account");
    printf("\n  %sUsername: 3-24 chars (letters, digits, _ - .)   Password: %d-%d chars%s\n",
           sty(ST_DIM), PASS_MIN, PASS_MAX, sty(ST_RESET));
    printf("  %sEducational simulation - do not use real credentials.%s\n\n", sty(ST_DIM), sty(ST_RESET));
    st = readLine("  Username: ", user, sizeof user);
    if (st == IN_EOF) return;
    if (st != IN_OK) { inputProblem(st, sizeof user - 1); return; }
    u = trim(user);
    if (!validUsernameStrict(u, why, sizeof why)) { setNotice(ST_BAD, "%s", why); return; }

    lr = dbLoad(&db);
    if (lr < 0) { setNotice(ST_BAD, "Cannot read accounts file: %s", strerror(errno)); return; }
    if (dbFind(&db, u)) { dbFree(&db); setNotice(ST_BAD, "The username '%s' is already taken.", u); return; }

    st = readPassword("  Password: ", p1, sizeof p1);
    if (st == IN_EOF) { dbFree(&db); return; }
    if (st != IN_OK) { dbFree(&db); inputProblem(st, sizeof p1 - 1); return; }
    if (p1[0] == '\0') { dbFree(&db); setNotice(ST_BAD, "Password cannot be empty."); return; }
    if (strlen(p1) < PASS_MIN) { dbFree(&db); setNotice(ST_BAD, "Password must be at least %d characters.", PASS_MIN); secureZero(p1, sizeof p1); return; }
    st = readPassword("  Confirm password: ", p2, sizeof p2);
    if (st != IN_OK || strcmp(p1, p2) != 0) {
        dbFree(&db); secureZero(p1, sizeof p1); secureZero(p2, sizeof p2);
        if (st == IN_EOF) return;
        setNotice(ST_BAD, "Passwords did not match. Account not created.");
        return;
    }
    if (hashPassword(p1, enc, sizeof enc) != 0) {
        dbFree(&db); secureZero(p1, sizeof p1); secureZero(p2, sizeof p2);
        setNotice(ST_BAD, "No secure random source available; cannot create a salted hash.");
        return;
    }
    secureZero(p1, sizeof p1); secureZero(p2, sizeof p2);
    r = dbPush(&db);
    if (!r) { dbFree(&db); setNotice(ST_BAD, "Out of memory."); return; }
    safeCopy(r->name, sizeof r->name, u);
    safeCopy(r->secret, sizeof r->secret, enc);
    r->kind = U_HASHED;
    if (dbSave(&db) != 0) setNotice(ST_BAD, "Could not write accounts file: %s", strerror(errno));
    else setNotice(ST_GOOD, "Account '%s' created. You can now log in.", u);
    dbFree(&db);
}

/* 1 = logged in, 0 = failed, -1 = EOF */
static int login(void)
{
    char user[64], pass[PASS_MAX + 2], prompt[96], *u;
    UserDb db;
    UserRec *r;
    int lr, ok = 0;
    InStatus st;

    appHeader("Login");
    printf("\n");
    if (os.lastUser[0]) snprintf(prompt, sizeof prompt, "  Username [%s]: ", os.lastUser);
    else snprintf(prompt, sizeof prompt, "  Username: ");
    st = readLine(prompt, user, sizeof user);
    if (st == IN_EOF) return -1;
    if (st != IN_OK) { inputProblem(st, sizeof user - 1); return 0; }
    u = trim(user);
    if (!*u && os.lastUser[0]) u = os.lastUser;
    if (!*u) { setNotice(ST_BAD, "Username cannot be empty."); return 0; }
    st = readPassword("  Password: ", pass, sizeof pass);
    if (st == IN_EOF) return -1;
    if (st != IN_OK) { inputProblem(st, sizeof pass - 1); return 0; }
    if (!pass[0]) { setNotice(ST_BAD, "Password cannot be empty."); return 0; }

    lr = dbLoad(&db);
    if (lr == 1) { secureZero(pass, sizeof pass); setNotice(ST_WARN, "No accounts exist yet. Choose 'Create Account' first."); return 0; }
    if (lr < 0) { secureZero(pass, sizeof pass); setNotice(ST_BAD, "Cannot read accounts file: %s", strerror(errno)); return 0; }
    r = dbFind(&db, u);
    if (r && r->kind == U_HASHED) ok = verifyHash(pass, r->secret);
    else if (r && r->kind == U_LEGACY) {
        ok = ctEqualStr(pass, r->secret);
        if (ok) {                                    /* upgrade plaintext record to a hash */
            char enc[200];
            if (hashPassword(pass, enc, sizeof enc) == 0) {
                safeCopy(r->secret, sizeof r->secret, enc);
                r->kind = U_HASHED;
                if (dbSave(&db) == 0) setNotice(ST_WARN, "Your old plaintext password was upgraded to a salted hash.");
            }
        }
    }
    secureZero(pass, sizeof pass);
    if (ok) {
        safeCopy(os.user, sizeof os.user, r->name);
        safeCopy(os.lastUser, sizeof os.lastUser, r->name);
        dbFree(&db);
        if (configSave() != 0) setNotice(ST_WARN, "Logged in, but settings could not be saved: %s", strerror(errno));
        return 1;
    }
    dbFree(&db);
    delay(600);
    setNotice(ST_BAD, "Invalid username or password.");
    return 0;
}

static int userRename(const char *oldName, const char *newName, char *err, size_t errSz)
{
    UserDb db;
    UserRec *me, *other;
    int lr = dbLoad(&db);
    if (lr != 0) { snprintf(err, errSz, "cannot read accounts file%s%s", lr < 0 ? ": " : "", lr < 0 ? strerror(errno) : ""); return -1; }
    me = dbFind(&db, oldName);
    other = dbFind(&db, newName);
    if (!me) { dbFree(&db); safeCopy(err, errSz, "your account record was not found"); return -1; }
    if (other && other != me) { dbFree(&db); safeCopy(err, errSz, "that username is already taken"); return -1; }
    safeCopy(me->name, sizeof me->name, newName);
    if (dbSave(&db) != 0) { snprintf(err, errSz, "cannot write accounts file: %s", strerror(errno)); dbFree(&db); return -1; }
    dbFree(&db);
    return 0;
}

/* ================= CALCULATOR ================= */

static int parseNumber(const char **pp, double *out, char *err, size_t errSz)
{
    const char *s = *pp;
    char buf[64];
    size_t len = 0;
    int digits = 0, dots = 0;
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+' || *s == '-') buf[len++] = *s++;
    while (isdigit((unsigned char)*s) || *s == '.') {
        if (*s == '.') dots++; else digits++;
        if (len >= sizeof buf - 1) { safeCopy(err, errSz, "number is too long"); return -1; }
        buf[len++] = *s++;
    }
    if (!digits || dots > 1) { safeCopy(err, errSz, "expected a number (e.g. 5, -2.5)"); return -1; }
    buf[len] = '\0';
    errno = 0;
    *out = strtod(buf, &end);
    if (*end || errno == ERANGE) { safeCopy(err, errSz, "number out of range"); return -1; }
    *pp = s;
    return 0;
}

/* Evaluates "<number> <op> <number>" with op in + - * / */
static int evalExpression(const char *expr, double *result, char *err, size_t errSz)
{
    const char *p = expr;
    double a, b;
    char op;
    if (parseNumber(&p, &a, err, errSz) != 0) return -1;
    while (*p == ' ' || *p == '\t') p++;
    op = *p;
    if (!op) { safeCopy(err, errSz, "missing operator and second number"); return -1; }
    if (op != '+' && op != '-' && op != '*' && op != '/') {
        snprintf(err, errSz, "invalid operator '%c' (use + - * /)", isprint((unsigned char)op) ? op : '?');
        return -1;
    }
    p++;
    if (parseNumber(&p, &b, err, errSz) != 0) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) { safeCopy(err, errSz, "only one operation is supported: <number> <op> <number>"); return -1; }
    switch (op) {
        case '+': *result = a + b; break;
        case '-': *result = a - b; break;
        case '*': *result = a * b; break;
        default:
            if (b == 0.0) { safeCopy(err, errSz, "division by zero"); return -1; }
            *result = a / b;
    }
    if (!(*result == *result) || *result > 1.7e308 || *result < -1.7e308) { safeCopy(err, errSz, "result out of range"); return -1; }
    return 0;
}

static void calculator(void)
{
    char line[128], err[96], *t;
    double r;
    InStatus st;
    appHeader("Calculator");
    printf("\n  Format:   <number> <operator> <number>      operators: + - * /\n");
    printf("  Examples: 5 + 2   10 / 4   -5 + 10   2.5 * 4\n");
    printf("  %sType q to return to the desktop.%s\n\n", sty(ST_DIM), sty(ST_RESET));
    for (;;) {
        st = readLine("  calc > ", line, sizeof line);
        if (st == IN_EOF) return;
        if (st != IN_OK) { printf("  %s%s%s\n", sty(ST_BAD), st == IN_TOO_LONG ? "Input too long." : "Unsupported characters.", sty(ST_RESET)); continue; }
        t = trim(line);
        if (!*t) continue;
        if (ciEqual(t, "q") || ciEqual(t, "quit") || ciEqual(t, "exit") || ciEqual(t, "back")) return;
        if (evalExpression(t, &r, err, sizeof err) != 0) printf("  %sError: %s%s\n", sty(ST_BAD), err, sty(ST_RESET));
        else printf("  %s= %.10g%s\n", sty(ST_GOOD), r, sty(ST_RESET));
    }
}

/* ================= CLOCK ================= */

static void bigTime(const char *ts)
{
    static const char *D[11][5] = {
        { "###", "# #", "# #", "# #", "###" }, { "  #", "  #", "  #", "  #", "  #" },
        { "###", "  #", "###", "#  ", "###" }, { "###", "  #", "###", "  #", "###" },
        { "# #", "# #", "###", "  #", "  #" }, { "###", "#  ", "###", "  #", "###" },
        { "###", "#  ", "###", "# #", "###" }, { "###", "  #", "  #", "  #", "  #" },
        { "###", "# #", "###", "# #", "###" }, { "###", "# #", "###", "  #", "###" },
        { "   ", " # ", "   ", " # ", "   " } };
    int row, k, c;
    for (row = 0; row < 5; row++) {
        printf("    %s", sty(ST_ACCENT));
        for (k = 0; ts[k]; k++) {
            int idx = ts[k] == ':' ? 10 : (ts[k] - '0');
            if (idx < 0 || idx > 10) continue;
            for (c = 0; c < 3; c++) {
                if (D[idx][row][c] == '#') { fputs(gl->full, stdout); fputs(gl->full, stdout); }
                else fputs("  ", stdout);
            }
            fputc(' ', stdout);
        }
        printf("%s\n", sty(ST_RESET));
    }
}

static void clockApp(void)
{
    char d[64], t[32], z[16];
    int first = 1;
    for (;;) {
        time_t now = time(NULL);
        fmtTime(d, sizeof d, "%A, %d %B %Y", now);
        fmtTime(t, sizeof t, "%H:%M:%S", now);
        fmtTime(z, sizeof z, "%Z", now);
        appHeaderEx("Clock", first);
        first = 0;
        printf("\n");
        bigTime(t);
        printf("\n    %s%s%s   %s%s%s\n", sty(ST_BOLD), d, sty(ST_RESET), sty(ST_DIM), z, sty(ST_RESET));
        if (!os.tty) { pauseProgram(); return; }
        printf("\n  %sPress ENTER to return to the desktop.%s\033[J\n", sty(ST_DIM), sty(ST_RESET));
        if (waitInput(1000) || os.eof) return;
    }
}

/* ================= RANDOM NUMBER ================= */

static void randomNumber(void)
{
    long long lo = 0, hi = 99, v;
    char ans[16];
    InStatus st;
    for (;;) {
        appHeader("Random Number");
        showNotice();
        printf("\n  %sRange defaults to 0-99. Not cryptographically secure for this use.%s\n\n", sty(ST_DIM), sty(ST_RESET));
        st = readLongLong("  Minimum [0]: ", 0, -1000000000000000LL, 1000000000000000LL, &lo);
        if (st == IN_EOF) return;
        if (st != IN_OK) continue;
        st = readLongLong("  Maximum [99]: ", 99, -1000000000000000LL, 1000000000000000LL, &hi);
        if (st == IN_EOF) return;
        if (st != IN_OK) continue;
        if (lo > hi) { setNotice(ST_BAD, "Minimum (%lld) cannot be greater than maximum (%lld).", lo, hi); continue; }
        for (;;) {
            uint64_t r;
            if (randomBelow((uint64_t)(hi - lo) + 1u, 0, &r) != 0) { setNotice(ST_BAD, "Random generator failed."); break; }
            v = lo + (long long)r;
            printf("\n  %s%s Random number (%lld to %lld): %s%lld%s\n\n", sty(ST_ACCENT), gl->diamond, lo, hi,
                   sty(ST_BOLD), v, sty(ST_RESET));
            st = readLine("  ENTER: roll again   r: new range   q: back > ", ans, sizeof ans);
            if (st == IN_EOF) return;
            if (st == IN_OK) {
                char *a = trim(ans);
                if (ciEqual(a, "q")) return;
                if (ciEqual(a, "r")) break;
            }
        }
    }
}

/* ================= TODO ================= */

typedef struct { char text[TODO_TEXT_MAX]; int done; } TodoItem;
typedef struct { TodoItem *items; int count, cap; } TodoList;

static void todoFree(TodoList *l) { free(l->items); l->items = NULL; l->count = l->cap = 0; }

static TodoItem *todoPush(TodoList *l)
{
    if (l->count >= TODO_LIMIT) return NULL;
    if (l->count == l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        TodoItem *n = (TodoItem *)realloc(l->items, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        l->items = n; l->cap = nc;
    }
    memset(&l->items[l->count], 0, sizeof(TodoItem));
    return &l->items[l->count++];
}

/* 0 ok (missing file = empty list), -1 error (errno set; -2 = too many tasks) */
static int todoLoad(TodoList *l)
{
    FILE *f;
    char line[512];
    l->items = NULL; l->count = l->cap = 0;
    f = fopen(os.todoPath, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    while (fgets(line, sizeof line, f)) {
        char *t = trim(line), *body;
        TodoItem *it;
        size_t i;
        int done = 0;
        if (!*t) continue;
        if (!strncmp(t, "[x] ", 4) || !strncmp(t, "[X] ", 4)) { done = 1; body = t + 4; }
        else if (!strncmp(t, "[ ] ", 4)) body = t + 4;
        else body = t;                                 /* legacy plain-text task */
        body = trim(body);
        if (!*body) continue;
        it = todoPush(l);
        if (!it) { int e = l->count >= TODO_LIMIT ? -2 : ENOMEM; fclose(f); todoFree(l); errno = e == -2 ? EFBIG : e; return -1; }
        safeCopy(it->text, sizeof it->text, body);
        for (i = 0; it->text[i]; i++) if ((unsigned char)it->text[i] < 0x20) it->text[i] = ' ';
        it->done = done;
    }
    { int bad = ferror(f); fclose(f); if (bad) { todoFree(l); return -1; } }
    return 0;
}

static int todoSave(const TodoList *l)
{
    char tmp[MS_PATH_MAX + 8];
    FILE *f;
    int i;
    snprintf(tmp, sizeof tmp, "%s.tmp", os.todoPath);
    f = fopen(tmp, "w");
    if (!f) return -1;
    for (i = 0; i < l->count; i++) fprintf(f, "[%c] %s\n", l->items[i].done ? 'x' : ' ', l->items[i].text);
    if (ferror(f)) { fclose(f); remove(tmp); return -1; }
    if (fclose(f) != 0) { remove(tmp); return -1; }
    if (replaceFile(tmp, os.todoPath) != 0) { remove(tmp); return -1; }
    return 0;
}

/* returns 0 ok, else a message in err */
static int todoAdd(TodoList *l, const char *raw, char *err, size_t errSz)
{
    char buf[512], *t;
    TodoItem *it;
    size_t i;
    safeCopy(buf, sizeof buf, raw);
    t = trim(buf);
    if (!*t) { safeCopy(err, errSz, "task cannot be empty"); return -1; }
    if (strlen(t) >= TODO_TEXT_MAX) { snprintf(err, errSz, "task too long (max %d characters)", TODO_TEXT_MAX - 1); return -1; }
    it = todoPush(l);
    if (!it) { safeCopy(err, errSz, "task list is full or out of memory"); return -1; }
    safeCopy(it->text, sizeof it->text, t);
    for (i = 0; it->text[i]; i++) if (it->text[i] == '\t') it->text[i] = ' ';
    return 0;
}

static void todoPrint(const TodoList *l, int limit)
{
    int i, shown = 0;
    if (l->count == 0) { printf("  %sNo tasks yet.%s\n", sty(ST_DIM), sty(ST_RESET)); return; }
    for (i = 0; i < l->count; i++) {
        if (limit > 0 && shown >= limit) { printf("  %s... and %d more (use View All)%s\n", sty(ST_DIM), l->count - i, sty(ST_RESET)); break; }
        if (l->items[i].done)
            printf("  %s%3d  [%s] %s%s\n", sty(ST_DIM), i + 1, gl->ok, l->items[i].text, sty(ST_RESET));
        else
            printf("  %s%3d%s  [ ] %s\n", sty(ST_ACCENT), i + 1, sty(ST_RESET), l->items[i].text);
        shown++;
    }
}

static int parseIndex(const char *s, int max, int *out)
{
    char *end;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (!*s || *end || errno || v < 1 || v > max) return -1;
    *out = (int)v;
    return 0;
}

static void todo(void)
{
    TodoList l;
    int rc;
    rc = todoLoad(&l);
    if (rc != 0) {
        setNotice(ST_BAD, "Cannot load todo.txt: %s", strerror(errno));
        appHeader("Todo List");
        printf("\n"); showNotice();
        pauseProgram();
        return;
    }
    for (;;) {
        char l1[96], r1[96], line[256], err[128];
        int choice, idx;
        InStatus st;
        int pending = 0, i;
        for (i = 0; i < l.count; i++) if (!l.items[i].done) pending++;
        appHeader("Todo List");
        printf("\n  %s%d task(s), %d pending%s\n\n", sty(ST_DIM), l.count, pending, sty(ST_RESET));
        todoPrint(&l, 10);
        printf("\n");
        boxTop();
        menuItem(l1, sizeof l1, 1, "Add Task", 22);      menuItem(r1, sizeof r1, 3, "Mark Complete", 22); snprintf(line, sizeof line, "%s    %s", l1, r1); boxRow(line);
        menuItem(l1, sizeof l1, 2, "View All Tasks", 22); menuItem(r1, sizeof r1, 4, "Delete Task", 22);  snprintf(line, sizeof line, "%s    %s", l1, r1); boxRow(line);
        menuItem(l1, sizeof l1, 5, "Back", 22);           boxRow(l1);
        boxBottom();
        showNotice();
        st = readChoice("  Select > ", 1, 5, &choice);
        if (st == IN_EOF) break;
        if (st != IN_OK) continue;
        if (choice == 5) break;
        if (choice == 1) {
            st = readLine("  New task: ", line, sizeof line);
            if (st == IN_EOF) break;
            if (st != IN_OK) { inputProblem(st, sizeof line - 1); continue; }
            if (todoAdd(&l, line, err, sizeof err) != 0) { setNotice(ST_BAD, "Not added: %s.", err); continue; }
            if (todoSave(&l) != 0) setNotice(ST_BAD, "Task added but saving failed: %s", strerror(errno));
            else setNotice(ST_GOOD, "Task saved to todo.txt.");
        } else if (choice == 2) {
            appHeader("Todo List - All Tasks");
            printf("\n");
            todoPrint(&l, 0);
            pauseProgram();
        } else {
            if (l.count == 0) { setNotice(ST_WARN, "There are no tasks."); continue; }
            st = readLine(choice == 3 ? "  Task number to complete: " : "  Task number to delete: ", line, sizeof line);
            if (st == IN_EOF) break;
            if (st != IN_OK) { inputProblem(st, sizeof line - 1); continue; }
            if (parseIndex(trim(line), l.count, &idx) != 0) { setNotice(ST_BAD, "Invalid task number (1-%d).", l.count); continue; }
            if (choice == 3) l.items[idx - 1].done = 1;
            else {
                if (!confirm("Delete this task?")) { setNotice(ST_WARN, "Cancelled."); continue; }
                memmove(&l.items[idx - 1], &l.items[idx], (size_t)(l.count - idx) * sizeof(TodoItem));
                l.count--;
            }
            if (todoSave(&l) != 0) setNotice(ST_BAD, "Saving failed: %s", strerror(errno));
            else setNotice(ST_GOOD, choice == 3 ? "Task marked complete." : "Task deleted.");
        }
    }
    todoFree(&l);
}

/* ================= PASSWORD GENERATOR ================= */

static void passwordGen(void)
{
    static const char LOW[] = "abcdefghijklmnopqrstuvwxyz", UP[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char DIG[] = "0123456789", SYM[] = "!@#$%^&*()-_=+[]{};:,.?";
    long long len = 12;
    int symbols = 0, askSettings = 1;
    InStatus st;
    for (;;) {
        char pw[PASS_MAX + 2], pool[128], ans[32];
        const char *sets[4];
        int nsets = 0, i;
        size_t poolLen, n;
        appHeader("Password Generator");
        showNotice();
        printf("\n  %sUses the operating system's secure random source (no rand()).%s\n\n", sty(ST_DIM), sty(ST_RESET));
        if (askSettings) {
            long long v;
            st = readLongLong("  Length (4-128) [12]: ", 12, 4, PASS_MAX, &v);
            if (st == IN_EOF) return;
            if (st != IN_OK) continue;
            len = v;
            st = readLine("  Include symbols? [y/N]: ", ans, sizeof ans);
            if (st == IN_EOF) return;
            if (st != IN_OK) { inputProblem(st, sizeof ans - 1); continue; }
            symbols = ciEqual(trim(ans), "y") || ciEqual(trim(ans), "yes");
            askSettings = 0;
        }
        sets[nsets++] = LOW; sets[nsets++] = UP; sets[nsets++] = DIG;
        if (symbols) sets[nsets++] = SYM;
        pool[0] = '\0';
        for (i = 0; i < nsets; i++) strncat(pool, sets[i], sizeof pool - strlen(pool) - 1);
        poolLen = strlen(pool);
        n = (size_t)len;
        for (i = 0; i < nsets; i++) {                   /* one guaranteed char from each class */
            uint64_t r;
            if (randomBelow(strlen(sets[i]), 1, &r) != 0) goto nosecure;
            pw[i] = sets[i][r];
        }
        for (i = nsets; (size_t)i < n; i++) {
            uint64_t r;
            if (randomBelow(poolLen, 1, &r) != 0) goto nosecure;
            pw[i] = pool[r];
        }
        for (i = (int)n - 1; i > 0; i--) {              /* Fisher-Yates shuffle */
            uint64_t r; char tmp;
            if (randomBelow((uint64_t)i + 1, 1, &r) != 0) goto nosecure;
            tmp = pw[i]; pw[i] = pw[r]; pw[r] = tmp;
        }
        pw[n] = '\0';
        printf("  %s%s Password:%s %s%s%s\n\n", sty(ST_ACCENT), gl->diamond, sty(ST_RESET), sty(ST_BOLD), pw, sty(ST_RESET));
        secureZero(pw, sizeof pw);
        st = readLine("  ENTER: another   c: change settings   q: back > ", ans, sizeof ans);
        if (st == IN_EOF) return;
        if (st == IN_OK) {
            char *a = trim(ans);
            if (ciEqual(a, "q")) return;
            if (ciEqual(a, "c")) askSettings = 1;
        }
        continue;
nosecure:
        secureZero(pw, sizeof pw);
        setNotice(ST_BAD, "No secure random source is available on this system; password not generated.");
        pauseProgram();
        return;
    }
}

/* ================= QUOTES ================= */

static void quote(void)
{
    static const char *const quotes[] = {
        "Stay hungry stay foolish",
        "Knowledge is power",
        "Code is poetry",
        "Dream big",
        "The deeds of the light are goodness, righteousness and faithfulness",
        "Our homes travel with us. They are wherever we feel loved and accepted.",
        "To be or not to be, that is the question.",
        "Happiness depends upon ourselves.",
        "What doesn't kill us makes us stronger."
    };
    const uint64_t count = sizeof quotes / sizeof quotes[0];
    uint64_t last = count;
    char ans[16];
    for (;;) {
        uint64_t idx = 0;
        InStatus st;
        do {
            if (randomBelow(count, 0, &idx) != 0) idx = 0;
        } while (count > 1 && idx == last);
        last = idx;
        appHeader("Quote Generator");
        printf("\n  %s\"%s\"%s\n\n", sty(ST_BOLD), quotes[idx], sty(ST_RESET));
        st = readLine("  ENTER: another quote   q: back > ", ans, sizeof ans);
        if (st == IN_EOF) return;
        if (st == IN_OK && ciEqual(trim(ans), "q")) return;
    }
}

/* ================= SYSTEM CONTROL ================= */

static void boot(void)
{
    int i;
    clear();
    printLogo();
    printf("\n%s%s META BOOT SEQUENCE %s%s\n", sty(ST_ACCENT), gl->diamond, gl->diamond, sty(ST_RESET));
    printf("  Initialising secure session handshake...\n"); delay(180);
    printf("  Scanning local storage "); fflush(stdout);
    printf("%s[", sty(ST_ACCENT)); for (i = 0; i < 24; i++) { putchar(gl->full[0]); fflush(stdout); delay(25); } printf("]%s\n", sty(ST_RESET));
    printf("  Mounting user space..."); fflush(stdout); delay(180); printf("  %sOK%s\n", sty(ST_GOOD), sty(ST_RESET));
    printf("  Loading application registry..."); fflush(stdout); delay(180); printf("  %sOK%s\n", sty(ST_GOOD), sty(ST_RESET));
    printf("  Starting terminal services..."); fflush(stdout); delay(180); printf("  %sOK%s\n", sty(ST_GOOD), sty(ST_RESET));
    printf("  Checking persistent notifications..."); fflush(stdout); delay(180); printf("  %sOK%s\n", sty(ST_GOOD), sty(ST_RESET));
    printf("\n%s%s Welcome back. Your workspace is ready.%s\n", sty(ST_BOLD), gl->ok, sty(ST_RESET));
    delay(800);
}

/* Returns 1 if the user confirmed (the main loop then reboots). */
static int restart(void)
{
    if (!confirm("Restart MetaShell OS?")) { setNotice(ST_WARN, "Restart cancelled."); return 0; }
    clear();
    printf("\n  Restarting MetaShell OS...\n");
    delay(1200);
    return 1;
}

static void logoff(void)
{
    clear();
    printf("\n  Logging off %s...\n", os.user);
    delay(800);
}

/* Exits MetaShell only - never powers off the host computer. */
static int shutdownSystem(void)
{
    if (!confirm("Shut down MetaShell OS?")) { setNotice(ST_WARN, "Shutdown cancelled."); return 0; }
    clear();
    printf("\n  Shutting down MetaShell OS...\n");
    delay(600);
    return 1;
}

/* ================= FILE MANAGER ================= */

/* Resolve user text (a list number or a name) to a name. */
static InStatus askTarget(const char *prompt, const FsList *list, char *out, size_t outSz)
{
    char buf[300], *t;
    InStatus st = readLine(prompt, buf, sizeof buf);
    size_t l;
    if (st == IN_EOF) return st;
    if (st != IN_OK) { inputProblem(st, sizeof buf - 1); return IN_INVALID; }
    t = trim(buf);
    l = strlen(t);
    if (l >= 2 && ((t[0] == '"' && t[l - 1] == '"') || (t[0] == '\'' && t[l - 1] == '\''))) { t[l - 1] = '\0'; t++; }
    if (!*t) { setNotice(ST_WARN, "Nothing entered."); return IN_INVALID; }
    {
        char *end; long n = strtol(t, &end, 10);
        if (!*end && n >= 1 && (size_t)n <= list->count) { safeCopy(out, outSz, list->items[n - 1].name); return IN_OK; }
    }
    if (strlen(t) >= outSz || !validPathArg(t)) { setNotice(ST_BAD, "Invalid name."); return IN_INVALID; }
    safeCopy(out, outSz, t);
    return IN_OK;
}

static int changeDir(const char *path, char *err, size_t errSz)
{
    if (!pathExists(path)) { snprintf(err, errSz, "'%s' does not exist", path); return -1; }
    if (!pathIsDir(path)) { snprintf(err, errSz, "'%s' is not a folder", path); return -1; }
    if (MS_CHDIR(path) != 0) { snprintf(err, errSz, "cannot enter '%s': %s", path, strerror(errno)); return -1; }
    return 0;
}

static void fileManager(void)
{
    int showHidden = 0;
    for (;;) {
        FsList list;
        int scanErr = 0, rc, choice;
        size_t i, shown = 0, nDirs = 0;
        char path[300], tail[96], line[1024], l1[96], r1[96], name[512], err[256], why[160];
        InStatus st;

        rc = fsScan(".", showHidden, &list, &scanErr);
        appHeader("File Manager");
        displayPath(path, sizeof path);
        fitTail(tail, sizeof tail, path, 54);
        boxTop();
        snprintf(line, sizeof line, "%s%s%s%s", sty(ST_PATH), tail, sty(ST_RESET), "");
        boxRow(line);
        boxMid();
        snprintf(line, sizeof line, "%s %3s  %-28s  %9s  %-8s%s", sty(ST_DIM), "#", "Name", "Size", "Type", sty(ST_RESET));
        boxRow(line);
        boxMid();
        if (rc != 0) {
            snprintf(line, sizeof line, "%sCannot read folder: %s%s", sty(ST_BAD), strerror(scanErr), sty(ST_RESET));
            boxRow(line);
        } else if (list.count == 0) {
            snprintf(line, sizeof line, "%s(empty folder)%s", sty(ST_DIM), sty(ST_RESET));
            boxRow(line);
        } else {
            for (i = 0; i < list.count; i++) {
                const FsEntry *e = &list.items[i];
                char nm[600], cell[256], sz[32];
                if (e->isDir) nDirs++;
                if (shown >= FM_MAX_ROWS) continue;
                snprintf(nm, sizeof nm, "%s%s", e->name, e->isDir ? "/" : "");
                fmtCell(cell, sizeof cell, nm, 28);
                if (e->isDir) safeCopy(sz, sizeof sz, "-");
                else if (!e->statOk) safeCopy(sz, sizeof sz, "?");
                else fmtSize(sz, sizeof sz, e->size);
                snprintf(line, sizeof line, "%s%3llu%s  %s%s%s  %9s  %-8s", sty(ST_DIM), (unsigned long long)(i + 1), sty(ST_RESET),
                         e->isDir ? sty(ST_ACCENT) : (e->hidden ? sty(ST_DIM) : ""), cell, sty(ST_RESET),
                         sz, fileTypeLabel(e));
                boxRow(line);
                shown++;
            }
            if (list.count > shown) {
                snprintf(line, sizeof line, "%s... %llu more not shown (open them by name, or use Terminal 'ls')%s",
                         sty(ST_DIM), (unsigned long long)(list.count - shown), sty(ST_RESET));
                boxRow(line);
            }
        }
        boxMid();
        snprintf(line, sizeof line, "%s%llu items, %llu folders, %llu files  |  hidden files: %s%s", sty(ST_DIM),
                 (unsigned long long)list.count, (unsigned long long)nDirs, (unsigned long long)(list.count - nDirs), showHidden ? "shown" : "hidden", sty(ST_RESET));
        boxRow(line);
        boxBottom();
        printf("  %sDO NOT RUN (TASKKILL.bat) unless your computer is infected with a virus or temporary problem with OS%s\n", sty(ST_DIM), sty(ST_RESET));
        {
            static const char *left[6]  = { "Open File", "Refresh", "Back", "Open Folder", "Parent Folder", "Home Folder" };
            static const char *right[6] = { "New File", "New Folder", "Rename", "Copy", "Delete", "Toggle Hidden" };
            boxTop();
            for (i = 0; i < 6; i++) {
                menuItem(l1, sizeof l1, (int)i + 1, left[i], 20);
                menuItem(r1, sizeof r1, (int)i + 7, right[i], 20);
                snprintf(line, sizeof line, "%s    %s", l1, r1);
                boxRow(line);
            }
            boxBottom();
        }
        showNotice();
        st = readChoice("  Select > ", 1, 12, &choice);
        if (st == IN_EOF) { fsFree(&list); return; }
        if (st != IN_OK) { fsFree(&list); continue; }

        switch (choice) {
        case 1:
            st = askTarget("  File name or #: ", &list, name, sizeof name);
            if (st == IN_EOF) { fsFree(&list); return; }
            if (st == IN_OK) {
                if (pathIsDir(name)) {
                    if (changeDir(name, err, sizeof err) != 0) setNotice(ST_BAD, "%s", err);
                } else openFile(name, 0);
            }
            break;
        case 2: setNotice(ST_GOOD, "Refreshed."); break;
        case 3: fsFree(&list); return;
        case 4:
            st = askTarget("  Folder name or #: ", &list, name, sizeof name);
            if (st == IN_EOF) { fsFree(&list); return; }
            if (st == IN_OK && changeDir(name, err, sizeof err) != 0) setNotice(ST_BAD, "%s", err);
            break;
        case 5:
            if (changeDir("..", err, sizeof err) != 0) setNotice(ST_BAD, "%s", err);
            break;
        case 6:
            if (changeDir(os.home, err, sizeof err) != 0) setNotice(ST_BAD, "%s", err);
            else setNotice(ST_GOOD, "Returned to the home folder.");
            break;
        case 7: case 8: {
            char nn[300], *t;
            st = readLine(choice == 7 ? "  New file name: " : "  New folder name: ", nn, sizeof nn);
            if (st == IN_EOF) { fsFree(&list); return; }
            if (st != IN_OK) { inputProblem(st, sizeof nn - 1); break; }
            t = trim(nn);
            if (!validLeafName(t, why, sizeof why)) { setNotice(ST_BAD, "Cannot create: %s.", why); break; }
            if (pathExists(t)) { setNotice(ST_BAD, "'%s' already exists.", t); break; }
            if (choice == 7) {
                FILE *f = fopen(t, "wb");
                if (!f) setNotice(ST_BAD, "Cannot create '%s': %s", t, strerror(errno));
                else if (fclose(f) != 0) setNotice(ST_BAD, "Error closing '%s': %s", t, strerror(errno));
                else setNotice(ST_GOOD, "Created file '%s'.", t);
            } else {
                if (MS_MKDIR(t) != 0) setNotice(ST_BAD, "Cannot create folder '%s': %s", t, strerror(errno));
                else setNotice(ST_GOOD, "Created folder '%s'.", t);
            }
            break;
        }
        case 9: {
            char nn[300], *t;
            st = askTarget("  Rename (name or #): ", &list, name, sizeof name);
            if (st == IN_EOF) { fsFree(&list); return; }
            if (st != IN_OK) break;
            if (!pathExists(name)) { setNotice(ST_BAD, "'%s' does not exist.", name); break; }
            if (isProtectedPath(name)) { setNotice(ST_BAD, "'%s' is a protected system file and cannot be renamed.", name); break; }
            st = readLine("  New name: ", nn, sizeof nn);
            if (st == IN_EOF) { fsFree(&list); return; }
            if (st != IN_OK) { inputProblem(st, sizeof nn - 1); break; }
            t = trim(nn);
            if (!validLeafName(t, why, sizeof why)) { setNotice(ST_BAD, "Cannot rename: %s.", why); break; }
            if (pathExists(t)) { setNotice(ST_BAD, "'%s' already exists; nothing was overwritten.", t); break; }
            if (rename(name, t) != 0) setNotice(ST_BAD, "Rename failed: %s", strerror(errno));
            else setNotice(ST_GOOD, "Renamed '%s' to '%s'.", name, t);
            break;
        }
        case 10: {
            char dst[300], *t, target[MS_PATH_MAX];
            st = askTarget("  Copy (name or #): ", &list, name, sizeof name);
            if (st == IN_EOF) { fsFree(&list); return; }
            if (st != IN_OK) break;
            if (!pathIsFile(name)) { setNotice(ST_BAD, "'%s' is not a regular file.", name); break; }
            st = readLine("  Destination (new name or folder): ", dst, sizeof dst);
            if (st == IN_EOF) { fsFree(&list); return; }
            if (st != IN_OK) { inputProblem(st, sizeof dst - 1); break; }
            t = trim(dst);
            if (!validPathArg(t)) { setNotice(ST_BAD, "Invalid destination."); break; }
            if (pathIsDir(t)) {
                if (pathJoin(target, sizeof target, t, name) != 0) { setNotice(ST_BAD, "Destination path too long."); break; }
            } else safeCopy(target, sizeof target, t);
            if (pathExists(target)) { setNotice(ST_BAD, "'%s' already exists; nothing was overwritten.", target); break; }
            if (copyFileSafe(name, target) != 0) setNotice(ST_BAD, "Copy failed: %s", strerror(errno));
            else setNotice(ST_GOOD, "Copied '%s' to '%s'.", name, target);
            break;
        }
        case 11: {
            st = askTarget("  Delete (name or #): ", &list, name, sizeof name);
            if (st == IN_EOF) { fsFree(&list); return; }
            if (st != IN_OK) break;
            if (!pathExists(name)) { setNotice(ST_BAD, "'%s' does not exist.", name); break; }
            if (isProtectedPath(name)) { setNotice(ST_BAD, "'%s' is a protected system file and cannot be deleted.", name); break; }
            snprintf(line, sizeof line, "Permanently delete '%.200s'?", name);
            if (!confirm(line)) { setNotice(ST_WARN, "Delete cancelled."); break; }
            if (pathIsDir(name)) {
                if (MS_RMDIR(name) != 0) setNotice(ST_BAD, "Cannot delete folder (only empty folders can be removed): %s", strerror(errno));
                else setNotice(ST_GOOD, "Deleted folder '%s'.", name);
            } else {
                if (remove(name) != 0) setNotice(ST_BAD, "Delete failed: %s", strerror(errno));
                else setNotice(ST_GOOD, "Deleted '%s'.", name);
            }
            break;
        }
        case 12:
            showHidden = !showHidden;
            setNotice(ST_GOOD, "Hidden files are now %s.", showHidden ? "shown" : "hidden");
            break;
        default: break;
        }
        fsFree(&list);
    }
}

/* ================= SETTINGS ================= */

static void settings(void)
{
    for (;;) {
        char l1[96];
        int choice, i;
        InStatus st;
        appHeader("Settings");
        printf("\n  Username : %s%s%s\n", sty(ST_BOLD), os.user, sty(ST_RESET));
        printf("  Theme    : %s%d - %s%s\n\n", sty(ST_ACCENT), os.theme, THEME_NAMES[os.theme], sty(ST_RESET));
        boxTop();
        { static const char *items[4] = { "Change Username", "Change Theme Color", "Reset Theme", "Back" };
          for (i = 0; i < 4; i++) { menuItem(l1, sizeof l1, i + 1, items[i], 24); boxRow(l1); } }
        boxBottom();
        showNotice();
        st = readChoice("  Select > ", 1, 4, &choice);
        if (st == IN_EOF) return;
        if (st != IN_OK) continue;
        if (choice == 4) return;
        if (choice == 1) {
            char nn[64], why[160], *t;
            st = readLine("  New username: ", nn, sizeof nn);
            if (st == IN_EOF) return;
            if (st != IN_OK) { inputProblem(st, sizeof nn - 1); continue; }
            t = trim(nn);
            if (!validUsernameStrict(t, why, sizeof why)) { setNotice(ST_BAD, "%s", why); continue; }
            if (!strcmp(t, os.user)) { setNotice(ST_WARN, "That is already your username."); continue; }
            if (userRename(os.user, t, why, sizeof why) != 0) { setNotice(ST_BAD, "Username not changed: %s.", why); continue; }
            safeCopy(os.user, sizeof os.user, t);
            safeCopy(os.lastUser, sizeof os.lastUser, t);
            if (configSave() != 0) setNotice(ST_WARN, "Username changed, but settings could not be saved: %s", strerror(errno));
            else setNotice(ST_GOOD, "Username changed to '%s'.", t);
        } else if (choice == 2) {
            long long v;
            printf("\n  ");
            for (i = 1; i <= 15; i++) {
                char a[16];
                themeAnsi(i, a, sizeof a);
                printf("%s%2d%s ", os.color ? a : "", i, sty(ST_RESET));
            }
            printf("\n  %s(1-15, Windows console colour numbers; 0 = black would hide the text)%s\n", sty(ST_DIM), sty(ST_RESET));
            st = readLongLong("  Theme number: ", -1, 0, 15, &v);
            if (st == IN_EOF) return;
            if (st != IN_OK) continue;
            if (v < 1) { setNotice(ST_BAD, "Please enter a theme between 1 and 15."); continue; }
            os.theme = (int)v;
            setTheme();
            if (configSave() != 0) setNotice(ST_WARN, "Theme applied, but settings could not be saved: %s", strerror(errno));
            else setNotice(ST_GOOD, "Theme set to %d (%s).", os.theme, THEME_NAMES[os.theme]);
        } else {
            os.theme = DEFAULT_THEME;
            setTheme();
            if (configSave() != 0) setNotice(ST_WARN, "Theme reset, but settings could not be saved: %s", strerror(errno));
            else setNotice(ST_GOOD, "Theme reset to default (%s).", THEME_NAMES[os.theme]);
        }
    }
}

/* ================= TERMINAL ================= */

typedef char HistLine[TERM_LINE_MAX];
typedef struct {
    HistLine *hist;
    int count, base, quit;
    SysAction action;
} TermCtx;
typedef int (*CmdFn)(TermCtx *, int, char **);
typedef struct { const char *name, *usage, *help; CmdFn fn; } Command;

static void termErr(const char *cmd, const char *fmt, ...) MS_FMT(2, 3);
static void termErr(const char *cmd, const char *fmt, ...)
{
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("%s%s: %s%s\n", sty(ST_BAD), cmd, msg, sty(ST_RESET));
}

static const char *platformName(void)
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Unknown";
#endif
}
static const char *archName(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

/* Splits `line` in place into arguments; supports "double" and 'single' quotes. */
static int tokenize(char *line, char **argv, int maxArgs, int *argc, char *err, size_t errSz)
{
    char *r = line, *w = line;
    *argc = 0;
    while (*r) {
        char q = 0;
        while (*r == ' ' || *r == '\t') r++;
        if (!*r) break;
        if (*argc >= maxArgs) { snprintf(err, errSz, "too many arguments (max %d)", maxArgs); return -1; }
        argv[(*argc)++] = w;
        while (*r && (q || (*r != ' ' && *r != '\t'))) {
            if (q) { if (*r == q) { q = 0; r++; } else *w++ = *r++; }
            else if (*r == '"' || *r == '\'') q = *r++;
            else *w++ = *r++;
        }
        if (q) { safeCopy(err, errSz, "unterminated quote"); return -1; }
        if (*r) r++;
        *w++ = '\0';
    }
    return 0;
}

static void joinArgs(char *out, size_t n, int argc, char **argv, int from)
{
    int i;
    out[0] = '\0';
    for (i = from; i < argc; i++) {
        if (i > from) strncat(out, " ", n - strlen(out) - 1);
        strncat(out, argv[i], n - strlen(out) - 1);
    }
}

static void terminalHeader(void)
{
    appHeader("MetaShell Terminal");
    printf("  %sType 'help' for commands, 'exit' to return to the desktop.%s\n\n", sty(ST_DIM), sty(ST_RESET));
}

static void printPrompt(void)
{
    char p[300];
    displayPath(p, sizeof p);
    printf("%s%s@MetaShell%s:%s%s%s$ ", sty(ST_ACCENT), os.user, sty(ST_RESET), sty(ST_PATH), p, sty(ST_RESET));
}

/* ---- terminal commands ---- */

#define UNUSED3 (void)t; (void)argc; (void)argv

static int cmd_help(TermCtx *t, int argc, char **argv);

static int cmd_clear(TermCtx *t, int argc, char **argv) { UNUSED3; clear(); terminalHeader(); return 0; }
static int cmd_exit(TermCtx *t, int argc, char **argv) { (void)argc; (void)argv; t->quit = 1; return 0; }
static int cmd_whoami(TermCtx *t, int argc, char **argv) { UNUSED3; printf("%s\n", os.user); return 0; }

static int cmd_pwd(TermCtx *t, int argc, char **argv)
{
    char cwd[MS_PATH_MAX];
    UNUSED3;
    if (!MS_GETCWD(cwd, sizeof cwd)) { termErr("pwd", "cannot determine current directory"); return 1; }
    printf("%s\n", cwd);
    return 0;
}

static int cmd_date(TermCtx *t, int argc, char **argv)
{
    char b[64];
    UNUSED3;
    fmtTime(b, sizeof b, "%A, %d %B %Y", time(NULL));
    printf("%s\n", b);
    return 0;
}

static int cmd_time(TermCtx *t, int argc, char **argv)
{
    char b[64];
    UNUSED3;
    fmtTime(b, sizeof b, "%H:%M:%S", time(NULL));
    printf("%s\n", b);
    return 0;
}

static int cmd_ls(TermCtx *t, int argc, char **argv)
{
    int all = 0, lng = 0, i, err = 0, cols, c;
    const char *dir = ".";
    int isDirAlias = argv[0] && ciEqual(argv[0], "dir");
    FsList list;
    size_t k, maxw = 0;
    (void)t;
    if (isDirAlias) lng = 1;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            const char *f;
            for (f = argv[i] + 1; *f; f++) {
                if (*f == 'a') all = 1; else if (*f == 'l') lng = 1;
                else { termErr(argv[0], "unknown option '-%c' (supported: -a -l)", *f); return 1; }
            }
        } else if (dir[0] == '.' && dir[1] == '\0') dir = argv[i];
        else { termErr(argv[0], "too many paths (use quotes for names with spaces)"); return 1; }
    }
    if (!pathExists(dir)) { termErr(argv[0], "cannot access '%s': No such file or directory", dir); return 1; }
    if (!pathIsDir(dir)) { printf("%s\n", dir); return 0; }
    if (fsScan(dir, all, &list, &err) != 0) { termErr(argv[0], "cannot open '%s': %s", dir, strerror(err)); return 1; }
    if (list.count == 0) { printf("%s(empty)%s\n", sty(ST_DIM), sty(ST_RESET)); fsFree(&list); return 0; }
    if (lng) {
        for (k = 0; k < list.count; k++) {
            const FsEntry *e = &list.items[k];
            char tm[32] = "                ", sz[32];
            if (e->statOk) fmtTime(tm, sizeof tm, "%Y-%m-%d %H:%M", e->mtime);
            if (e->isDir) safeCopy(sz, sizeof sz, "<DIR>");
            else if (!e->statOk) safeCopy(sz, sizeof sz, "?");
            else fmtSize(sz, sizeof sz, e->size);
            printf("%s  %10s  %s%s%s%s\n", tm, sz, e->isDir ? sty(ST_ACCENT) : (e->hidden ? sty(ST_DIM) : ""),
                   e->name, e->isDir ? "/" : "", sty(ST_RESET));
        }
    } else {
        for (k = 0; k < list.count; k++) {
            size_t w = (size_t)visibleLen(list.items[k].name) + (list.items[k].isDir ? 1u : 0u);
            if (w > maxw) maxw = w;
        }
        cols = (int)(78 / (maxw + 2));
        if (cols < 1) cols = 1;
        for (k = 0; k < list.count; k++) {
            const FsEntry *e = &list.items[k];
            int w = visibleLen(e->name) + (e->isDir ? 1 : 0);
            printf("%s%s%s%s", e->isDir ? sty(ST_ACCENT) : (e->hidden ? sty(ST_DIM) : ""), e->name, e->isDir ? "/" : "", sty(ST_RESET));
            c = (int)(k % (size_t)cols);
            if (c == cols - 1 || k + 1 == list.count) printf("\n");
            else printf("%*s", (int)maxw + 2 - w, "");
        }
    }
    fsFree(&list);
    return 0;
}

static int cmd_cd(TermCtx *t, int argc, char **argv)
{
    char err[300];
    const char *target;
    (void)t;
    if (argc > 2) { termErr("cd", "too many arguments (put names with spaces in quotes)"); return 1; }
    target = argc == 1 || !strcmp(argv[1], "~") ? os.home : argv[1];
    if (!validPathArg(target)) { termErr("cd", "invalid path"); return 1; }
    if (changeDir(target, err, sizeof err) != 0) { termErr("cd", "%s", err); return 1; }
    return 0;
}

static int cmd_mkdir(TermCtx *t, int argc, char **argv)
{
    int i, rc = 0;
    (void)t;
    if (argc < 2) { termErr("mkdir", "missing folder name (usage: mkdir <name>)"); return 1; }
    for (i = 1; i < argc; i++) {
        if (!validPathArg(argv[i])) { termErr("mkdir", "invalid name"); rc = 1; continue; }
        if (pathExists(argv[i])) { termErr("mkdir", "cannot create '%s': already exists", argv[i]); rc = 1; continue; }
        if (MS_MKDIR(argv[i]) != 0) { termErr("mkdir", "cannot create '%s': %s", argv[i], strerror(errno)); rc = 1; continue; }
        printf("created folder '%s'\n", argv[i]);
    }
    return rc;
}

static int cmd_touch(TermCtx *t, int argc, char **argv)
{
    int i, rc = 0;
    (void)t;
    if (argc < 2) { termErr("touch", "missing file name (usage: touch <name>)"); return 1; }
    for (i = 1; i < argc; i++) {
        FILE *f;
        if (!validPathArg(argv[i])) { termErr("touch", "invalid name"); rc = 1; continue; }
        if (pathExists(argv[i])) { termErr("touch", "'%s' already exists; not overwritten", argv[i]); rc = 1; continue; }
        f = fopen(argv[i], "wb");
        if (!f) { termErr("touch", "cannot create '%s': %s", argv[i], strerror(errno)); rc = 1; continue; }
        if (fclose(f) != 0) { termErr("touch", "error closing '%s': %s", argv[i], strerror(errno)); rc = 1; continue; }
        printf("created file '%s'\n", argv[i]);
    }
    return rc;
}

static int cmd_cat(TermCtx *t, int argc, char **argv)
{
    int i, rc = 0;
    (void)t;
    if (argc < 2) { termErr("cat", "missing file name (usage: cat <file>)"); return 1; }
    for (i = 1; i < argc; i++) {
        if (!validPathArg(argv[i])) { termErr("cat", "invalid name"); rc = 1; continue; }
        if (!pathExists(argv[i])) { termErr("cat", "%s: No such file or directory", argv[i]); rc = 1; continue; }
        if (pathIsDir(argv[i])) { termErr("cat", "%s: Is a directory", argv[i]); rc = 1; continue; }
        if (viewTextFile(argv[i], 0) != 0) rc = 1;
    }
    return rc;
}

static int cmd_echo(TermCtx *t, int argc, char **argv)
{
    char out[TERM_LINE_MAX];
    (void)t;
    joinArgs(out, sizeof out, argc, argv, 1);
    printf("%s\n", out);
    return 0;
}

static int cmd_history(TermCtx *t, int argc, char **argv)
{
    int i;
    (void)argc; (void)argv;
    for (i = 0; i < t->count; i++) printf("%s%5d%s  %s\n", sty(ST_DIM), t->base + i + 1, sty(ST_RESET), t->hist[i]);
    return 0;
}

static int cmd_version(TermCtx *t, int argc, char **argv) { UNUSED3; printf("%s v%s\n", MS_NAME, MS_VERSION); return 0; }

static int cmd_uname(TermCtx *t, int argc, char **argv)
{
    (void)t;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "-a") != 0)) { termErr("uname", "usage: uname [-a]"); return 1; }
    printf("%s %s %s %s", MS_NAME, MS_VERSION, platformName(), archName());
    if (argc == 2) printf(" (built %s %s)", __DATE__, __TIME__);
    printf("\n");
    return 0;
}

static int cmd_free(TermCtx *t, int argc, char **argv)
{
    char a[32], b[32], c[32];
    unsigned long long total = 0, avail = 0;
    int have = 0;
    UNUSED3;
#ifdef _WIN32
    { MEMORYSTATUSEX m; m.dwLength = sizeof m;
      if (GlobalMemoryStatusEx(&m)) { total = m.ullTotalPhys; avail = m.ullAvailPhys; have = 1; } }
#elif defined(__linux__)
    { FILE *f = fopen("/proc/meminfo", "r"); char line[160]; unsigned long long v; char key[64];
      if (f) {
          int g = 0;
          while (fgets(line, sizeof line, f)) {
              if (sscanf(line, "%63[^:]: %llu kB", key, &v) == 2) {
                  if (!strcmp(key, "MemTotal")) { total = v * 1024ULL; g |= 1; }
                  else if (!strcmp(key, "MemAvailable")) { avail = v * 1024ULL; g |= 2; }
              }
          }
          fclose(f);
          have = (g == 3);
      } }
#endif
    if (!have) { printf("Memory information is not available on this platform; no values are shown.\n"); return 1; }
    fmtSize(a, sizeof a, (long long)total); fmtSize(b, sizeof b, (long long)avail); fmtSize(c, sizeof c, (long long)(total - avail));
    printf("%-6s %12s %12s %12s\n", "", "total", "available", "used");
    printf("%-6s %12s %12s %12s\n", "Mem:", a, b, c);
    printf("%s(host computer memory; MetaShell has no memory manager of its own)%s\n", sty(ST_DIM), sty(ST_RESET));
    return 0;
}

static int cmd_calc(TermCtx *t, int argc, char **argv)
{
    char expr[TERM_LINE_MAX], err[96];
    double r;
    (void)t;
    if (argc < 2) { termErr("calc", "missing expression (usage: calc 5 + 5)"); return 1; }
    joinArgs(expr, sizeof expr, argc, argv, 1);
    if (evalExpression(expr, &r, err, sizeof err) != 0) { termErr("calc", "%s", err); return 1; }
    printf("%.10g\n", r);
    return 0;
}

static int cmd_info(TermCtx *t, int argc, char **argv)
{
    char p[300];
    UNUSED3;
    displayPath(p, sizeof p);
    printf("  %-14s %s\n", "OS:", MS_NAME);
    printf("  %-14s %s\n", "Version:", MS_VERSION);
    printf("  %-14s %s\n", "User:", os.user);
    printf("  %-14s %s\n", "Directory:", p);
    printf("  %-14s %s (%s)\n", "Platform:", platformName(), archName());
    printf("  %-14s %d - %s\n", "Theme:", os.theme, THEME_NAMES[os.theme]);
    printf("  %-14s File Manager, Calculator, Clock, Random Number, Todo List,\n", "Applications:");
    printf("  %-14s Password Generator, Quote Generator, Settings, Terminal,\n", "");
    printf("  %-14s System Apps (15 in-OS tools + browser)\n", "");
    return 0;
}

static int cmd_restart(TermCtx *t, int argc, char **argv)
{
    UNUSED3;
    if (restart()) { t->action = SYS_RESTART; t->quit = 1; }
    else showNotice();
    return 0;
}

static int cmd_shutdown(TermCtx *t, int argc, char **argv)
{
    UNUSED3;
    if (shutdownSystem()) { t->action = SYS_SHUTDOWN; t->quit = 1; }
    else showNotice();
    return 0;
}

static int cmd_logout(TermCtx *t, int argc, char **argv)
{
    UNUSED3;
    logoff();
    t->action = SYS_LOGOUT; t->quit = 1;
    return 0;
}

static int cmd_open(TermCtx *t, int argc, char **argv)
{
    (void)t;
    if (argc != 2) { termErr("open", "usage: open <file>   (quote names with spaces)"); return 1; }
    return openFile(argv[1], 1) == 0 ? 0 : 1;
}

static int cmd_todo(TermCtx *t, int argc, char **argv)
{
    TodoList l;
    char err[128], text[TERM_LINE_MAX];
    int idx, rc = 0;
    (void)t;
    if (todoLoad(&l) != 0) { termErr("todo", "cannot load todo.txt: %s", strerror(errno)); return 1; }
    if (argc == 1) { todoPrint(&l, 0); }
    else if (!strcmp(argv[1], "add") && argc >= 3) {
        joinArgs(text, sizeof text, argc, argv, 2);
        if (todoAdd(&l, text, err, sizeof err) != 0) { termErr("todo", "%s", err); rc = 1; }
        else if (todoSave(&l) != 0) { termErr("todo", "cannot save: %s", strerror(errno)); rc = 1; }
        else printf("task added\n");
    } else if ((!strcmp(argv[1], "done") || !strcmp(argv[1], "rm")) && argc == 3) {
        if (parseIndex(argv[2], l.count, &idx) != 0) { termErr("todo", "invalid task number"); rc = 1; }
        else {
            if (!strcmp(argv[1], "done")) l.items[idx - 1].done = 1;
            else { memmove(&l.items[idx - 1], &l.items[idx], (size_t)(l.count - idx) * sizeof(TodoItem)); l.count--; }
            if (todoSave(&l) != 0) { termErr("todo", "cannot save: %s", strerror(errno)); rc = 1; }
            else printf("ok\n");
        }
    } else { termErr("todo", "usage: todo | todo add <text> | todo done <n> | todo rm <n>"); rc = 1; }
    todoFree(&l);
    return rc;
}

static const Command COMMANDS[] = {
    { "help",     "help",                 "Show this list of commands",           cmd_help },
    { "clear",    "clear",                "Clear the terminal screen",            cmd_clear },
    { "cls",      "cls",                  "Alias for clear",                      cmd_clear },
    { "exit",     "exit",                 "Return to the MetaShell desktop",      cmd_exit },
    { "quit",     "quit",                 "Alias for exit",                       cmd_exit },
    { "whoami",   "whoami",               "Show the current MetaShell user",      cmd_whoami },
    { "pwd",      "pwd",                  "Print the working directory",          cmd_pwd },
    { "date",     "date",                 "Show the current date",                cmd_date },
    { "time",     "time",                 "Show the current time",                cmd_time },
    { "ls",       "ls [-a] [-l] [path]",  "List files and folders",               cmd_ls },
    { "dir",      "dir [-a] [path]",      "Detailed directory listing",           cmd_ls },
    { "cd",       "cd [folder]",          "Change directory (cd, cd .., cd \"my folder\")", cmd_cd },
    { "mkdir",    "mkdir <name>",         "Create a folder",                      cmd_mkdir },
    { "touch",    "touch <name>",         "Create an empty file (never overwrites)", cmd_touch },
    { "cat",      "cat <file>",           "Display a text file",                  cmd_cat },
    { "echo",     "echo <text>",          "Print text",                           cmd_echo },
    { "history",  "history",              "Show commands entered (use !! or !N)", cmd_history },
    { "version",  "version",              "Show the MetaShell OS version",        cmd_version },
    { "uname",    "uname [-a]",           "Show OS and platform information",     cmd_uname },
    { "free",     "free",                 "Show host memory information",         cmd_free },
    { "calc",     "calc <n> <op> <n>",    "Calculate, e.g. calc 5 + 5",           cmd_calc },
    { "info",     "info",                 "Show system information",              cmd_info },
    { "open",     "open <file>",          "Open a file with the File Manager viewer", cmd_open },
    { "todo",     "todo [add|done|rm]",   "Show / edit the todo list",            cmd_todo },
    { "logout",   "logout",               "Log off and return to the login screen", cmd_logout },
    { "restart",  "restart",              "Restart MetaShell OS (asks first)",    cmd_restart },
    { "shutdown", "shutdown",             "Exit MetaShell OS (asks first)",       cmd_shutdown },
    { NULL, NULL, NULL, NULL }
};

static int cmd_help(TermCtx *t, int argc, char **argv)
{
    const Command *c;
    UNUSED3;
    printf("%sMetaShell Terminal commands%s\n", sty(ST_BOLD), sty(ST_RESET));
    for (c = COMMANDS; c->name; c++)
        printf("  %s%-22s%s %s\n", sty(ST_ACCENT), c->usage, sty(ST_RESET), c->help);
    printf("%sQuote arguments containing spaces. Only built-in commands run; the host shell is never used.%s\n",
           sty(ST_DIM), sty(ST_RESET));
    return 0;
}

static void historyAdd(TermCtx *t, const char *cmd)
{
    if (t->count == HIST_MAX) {
        memmove(t->hist, t->hist + 1, (size_t)(HIST_MAX - 1) * sizeof(HistLine));
        t->count--; t->base++;
    }
    safeCopy(t->hist[t->count], sizeof(HistLine), cmd);
    t->count++;
}

/* "!!" and "!N" history recall. Returns 1 and fills out on success. */
static int expandHistory(const TermCtx *t, const char *cmd, char *out, size_t outSz)
{
    int idx;
    if (t->count == 0) { termErr("history", "no previous commands"); return 0; }
    if (!strcmp(cmd, "!!")) { safeCopy(out, outSz, t->hist[t->count - 1]); return 1; }
    if (parseIndex(cmd + 1, t->base + t->count, &idx) == 0 && idx > t->base) {
        safeCopy(out, outSz, t->hist[idx - t->base - 1]);
        return 1;
    }
    termErr("history", "event not found: %s", cmd);
    return 0;
}

static void executeLine(TermCtx *t, const char *line)
{
    char work[TERM_LINE_MAX], err[96], *argv[32];
    const Command *c;
    int argc = 0;
    safeCopy(work, sizeof work, line);
    if (tokenize(work, argv, 32, &argc, err, sizeof err) != 0) { termErr("parse error", "%s", err); return; }
    if (argc == 0) return;
    for (c = COMMANDS; c->name; c++) {
        if (ciEqual(argv[0], c->name)) { c->fn(t, argc, argv); return; }
    }
    printf("%s%s: command not found (type 'help' for the command list)%s\n", sty(ST_BAD), argv[0], sty(ST_RESET));
}

static SysAction terminalApp(void)
{
    TermCtx ctx;
    char line[TERM_LINE_MAX], expanded[TERM_LINE_MAX];
    memset(&ctx, 0, sizeof ctx);
    ctx.hist = (HistLine *)calloc(HIST_MAX, sizeof(HistLine));
    if (!ctx.hist) { setNotice(ST_BAD, "Out of memory: cannot start the terminal."); return SYS_NONE; }
    terminalHeader();
    while (!ctx.quit) {
        InStatus st;
        char *cmd;
        printPrompt();
        st = readInput(line, sizeof line);
        if (st == IN_EOF) { printf("\n"); break; }
        if (st == IN_TOO_LONG) { termErr("input", "command too long (limit %d characters)", TERM_LINE_MAX - 1); continue; }
        if (st == IN_BAD) { termErr("input", "unsupported control characters"); continue; }
        cmd = trim(line);
        if (!*cmd) continue;
        if (cmd[0] == '!') {
            if (!expandHistory(&ctx, cmd, expanded, sizeof expanded)) continue;
            cmd = expanded;
            printf("%s%s%s\n", sty(ST_DIM), cmd, sty(ST_RESET));
        }
        historyAdd(&ctx, cmd);
        executeLine(&ctx, cmd);
    }
    free(ctx.hist);
    return ctx.action;
}

/* ================= IN-OS SYSTEM APPLICATIONS ================= */

static void appBackPrompt(void) { pauseProgram(); }

/* Persistent event/log/notification helpers shared by the in-OS applications. */
static void appLog(const char *category, const char *fmt, ...)
{
    FILE *f = fopen("system.log", "ab");
    char ts[32]; va_list ap;
    if (!f) return;
    fmtTime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", time(NULL));
    fprintf(f, "[%s] [%s] ", ts, category ? category : "APP");
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
static void appNotify(const char *category, const char *fmt, ...)
{
    FILE *f = fopen("notifications.log", "ab");
    char ts[32]; va_list ap;
    if (!f) return;
    fmtTime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", time(NULL));
    fprintf(f, "[%s] [%s] ", ts, category ? category : "APP");
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
static int readWholeFile(const char *path, char **out, size_t *len)
{
    FILE *f; long n; char *p;
    *out = NULL; if (len) *len = 0; f = fopen(path, "rb"); if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || n > MAX_VIEW_BYTES) { fclose(f); return -1; }
    rewind(f); p = (char *)malloc((size_t)n + 1); if (!p) { fclose(f); return -1; }
    if (n && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return -1; }
    p[n] = '\0'; fclose(f); *out = p; if (len) *len = (size_t)n; return 0;
}
static int writeWholeFile(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    if (n && fwrite(data, 1, n, f) != n) { fclose(f); return -1; }
    return fclose(f);
}

static void systemMonitorApp(void)
{
    char line[256], mem[32], totalS[32], availS[32], disk[32] = "unavailable", up[64] = "unavailable";
    unsigned long long total = 0, avail = 0; FILE *f; struct statvfs v;
    for (;;) {
        appHeader("System Monitor");
        f = fopen("/proc/meminfo", "r");
        if (f) { while (fgets(line, sizeof line, f)) { unsigned long long x; char key[64];
            if (sscanf(line, "%63[^:]: %llu kB", key, &x) == 2) { if (!strcmp(key,"MemTotal")) total=x*1024ULL; if (!strcmp(key,"MemAvailable")) avail=x*1024ULL; } } fclose(f); }
        f = fopen("/proc/uptime", "r"); if (f) { double x; if (fscanf(f, "%lf", &x)==1) snprintf(up,sizeof up,"%.0f seconds",x); fclose(f); }
#ifndef _WIN32
        if (statvfs(".", &v) == 0) fmtSize(disk,sizeof disk,(long long)v.f_bavail*v.f_frsize);
#else
        { ULARGE_INTEGER freeb,totalb,availb; if (GetDiskFreeSpaceExA(".",&freeb,&totalb,&availb)) fmtSize(disk,sizeof disk,(long long)availb.QuadPart); }
#endif
        printf("\n  Host OS      : %s\n  Architecture : %s\n  User         : %s\n  CPU          : %s\n  Uptime       : %s\n  Disk free    : %s\n", platformName(), archName(), os.user, "available through system information", up, disk);
        if (total) { fmtSize(totalS,sizeof totalS,(long long)total); fmtSize(availS,sizeof availS,(long long)avail); fmtSize(mem,sizeof mem,(long long)(total-avail)); printf("  Memory       : %s used / %s total (%s available)\n",mem,totalS,availS); } else printf("  Memory       : unavailable on this platform\n");
        printf("\n  r Refresh   q Back\n"); { char b[16]; if (readLine("  > ",b,sizeof b)!=IN_OK || ciEqual(trim(b),"q")) return; }
    }
}
static void taskManagerApp(void)
{
    char b[32];
    for (;;) {
        DIR *d; struct dirent *e; int shown = 0; appHeader("Task Manager");
        printf("\n  PID      PROCESS\n");
#ifndef _WIN32
        d = opendir("/proc"); if (d) { while ((e=readdir(d))) { char p[128], name[128] = "?"; FILE *f; int pid;
            if (!isdigit((unsigned char)e->d_name[0])) continue; pid=atoi(e->d_name); snprintf(p,sizeof p,"/proc/%d/comm",pid); f=fopen(p,"r"); if(f){if(fgets(name,sizeof name,f)) name[strcspn(name,"\r\n")]=0; fclose(f);} printf("  %-8d %s\n",pid,name); shown++; if(shown>=80) break; } closedir(d); }
#else
        printf("  Process enumeration is not available in this build.\n");
#endif
        printf("\n  k <pid> terminate a non-critical process   r refresh   q back\n");
        if (readLine("  > ",b,sizeof b)!=IN_OK) return; if (ciEqual(trim(b),"q")) return; if (ciEqual(trim(b),"r")) continue;
#ifndef _WIN32
        if (b[0]=='k' && isspace((unsigned char)b[1])) { int pid=atoi(trim(b+1)); if (pid<=1 || pid==(int)getpid() || pid==(int)getppid()) { setNotice(ST_BAD,"That process is protected."); continue; } if (!confirm("Terminate the selected process?")) continue; if (kill((pid_t)pid,SIGTERM)!=0) setNotice(ST_BAD,"Terminate failed: %s",strerror(errno)); else { appLog("TASK", "terminated pid %d",pid); appNotify("Task Manager", "Process %d was terminated.",pid); setNotice(ST_GOOD,"Termination signal sent to PID %d.",pid); } }
#endif
    }
}
static int writeTextFileInOs(const char *name, const char *text)
{ FILE *f; if (!validPathArg(name) || pathIsDir(name)) return -1; f=fopen(name,"wb"); if(!f)return -1; if(fputs(text,f)==EOF || fclose(f)!=0)return -1; return 0; }
static void textEditorProApp(void)
{
    char name[MS_PATH_MAX], line[TERM_LINE_MAX], *buf=NULL; size_t used=0, cap=0; InStatus st; int exists;
    appHeader("Text Editor Pro"); st=readLine("  File name (new or existing; blank cancels): ",name,sizeof name); if(st!=IN_OK||!*trim(name))return;
    if(!validPathArg(trim(name))){setNotice(ST_BAD,"Invalid path.");return;} exists=pathIsFile(trim(name));
    if(exists && readWholeFile(trim(name),&buf,&used)!=0){setNotice(ST_BAD,"Cannot open file: %s",strerror(errno));return;}
    printf("\n  %sEditing %s%s%s\n  Enter lines. Commands: :w save, :w <name> save-as, :q quit, :q! discard.\n\n",sty(ST_DIM),sty(ST_BOLD),trim(name),sty(ST_RESET));
    if(buf){fwrite(buf,1,used,stdout); if(used && buf[used-1]!='\n')putchar('\n'); free(buf); buf=NULL; used=0;}
    for(;;){ st=readLine("  edit> ",line,sizeof line); if(st!=IN_OK)break; if(!strcmp(line,":q")){break;} if(!strcmp(line,":q!")){used=0;break;} if(!strncmp(line,":w",2) && (line[2]=='\0'||isspace((unsigned char)line[2]))) { char *p=trim(line+2); if(*p) safeCopy(name,sizeof name,p); if(writeWholeFile(trim(name),buf?buf:"",used)==0){appLog("EDITOR","saved %s",name);appNotify("Text Editor","Saved %s",name);setNotice(ST_GOOD,"Saved %s.",name);}else setNotice(ST_BAD,"Save failed: %s",strerror(errno)); continue;} if(used+strlen(line)+1>MAX_VIEW_BYTES){setNotice(ST_BAD,"Editor buffer limit reached.");continue;} if(used+strlen(line)+1>cap){size_t nc=cap?cap*2:4096;while(nc<used+strlen(line)+1)nc*=2;buf=(char*)realloc(buf,nc);if(!buf){setNotice(ST_BAD,"Out of memory.");break;}cap=nc;} memcpy(buf+used,line,strlen(line));used+=strlen(line);buf[used++]='\n'; }
    free(buf); if(!exists && !pathExists(name)) setNotice(ST_WARN,"File was not saved.");
}
static void calendarApp(void)
{
    char b[256]; for(;;){time_t now=time(NULL);struct tm tmv;int y,m,day,w,days,i;FILE*f;appHeader("Calendar");localTm(now,&tmv);y=tmv.tm_year+1900;m=tmv.tm_mon+1;day=tmv.tm_mday;tmv.tm_mday=1;mktime(&tmv);w=tmv.tm_wday;days=31;if(m==4||m==6||m==9||m==11)days=30;else if(m==2)days=((y%4==0&&y%100!=0)||y%400==0)?29:28;printf("\n  %s %d\n  Su Mo Tu We Th Fr Sa\n",(const char*[]){"","January","February","March","April","May","June","July","August","September","October","November","December"}[m],y);for(i=0;i<w;i++)printf("   ");for(i=1;i<=days;i++){printf("%s%2d%s%c",i==day?sty(ST_GOOD):"",i,sty(ST_RESET),((i+w)%7==0)?'\n':' ');}printf("\n  Events:\n");f=fopen("calendar.txt","r");if(f){while(fgets(b,sizeof b,f))printf("  %s",b);fclose(f);}else printf("  (none)\n");printf("\n  a Add event   d Delete event line   q Back\n");if(readLine("  > ",b,sizeof b)!=IN_OK||ciEqual(trim(b),"q"))return;if(b[0]=='a'){char e[200];if(readLine("  Event: ",e,sizeof e)==IN_OK&&*trim(e)){f=fopen("calendar.txt","ab");if(f){fprintf(f,"%s\n",trim(e));fclose(f);appLog("CALENDAR","added event");appNotify("Calendar","Event added.");}}}else if(b[0]=='d'){long n=strtol(trim(b+1),NULL,10);char tmp[64],*all=NULL;size_t u=0;f= fopen("calendar.txt","r");if(f){while(fgets(tmp,sizeof tmp,f)){if(n-->1){size_t z=strlen(tmp);all=realloc(all,u+z+1);memcpy(all+u,tmp,z);u+=z;}}fclose(f);if(all){writeWholeFile("calendar.txt",all,u);free(all);}}}}
}
static void notesApp(void)
{ char b[256]; for(;;){appHeader("Notes");printf("\n");if(viewTextFile("notes.txt",0)!=0)printf("  (no notes)\n");printf("\n  a Add   e Append/edit   d Delete all   q Back\n");if(readLine("  > ",b,sizeof b)!=IN_OK||ciEqual(trim(b),"q"))return;if(b[0]=='a'||b[0]=='e'){char n[1024];if(readLine("  Note: ",n,sizeof n)==IN_OK&&*trim(n)){FILE*f=fopen("notes.txt","ab");if(f){fprintf(f,"%s\n",trim(n));fclose(f);appLog("NOTES","saved note");appNotify("Notes","A note was saved.");}}}else if(b[0]=='d'&&confirm("Delete all notes?")){remove("notes.txt");appLog("NOTES","deleted all notes");}}}
static void diskManagerApp(void)
{ char a[32],b[32],c[32];appHeader("Disk Manager");printf("\n");
#ifndef _WIN32
{struct statvfs v;if(statvfs(".",&v)==0){fmtSize(a,sizeof a,(long long)v.f_blocks*v.f_frsize);fmtSize(b,sizeof b,(long long)v.f_bavail*v.f_frsize);fmtSize(c,sizeof c,(long long)(v.f_blocks-v.f_bavail)*v.f_frsize);printf("  Mount .   total %s   used %s   free %s\n",a,c,b);}else printf("  Storage unavailable: %s\n",strerror(errno));}
#else
{ULARGE_INTEGER q,r,s;if(GetDiskFreeSpaceExA(".",&r,&q,&s)){fmtSize(a,sizeof a,(long long)q.QuadPart);fmtSize(b,sizeof b,(long long)s.QuadPart);fmtSize(c,sizeof c,(long long)(q.QuadPart-s.QuadPart));printf("  Drive .   total %s   used %s   free %s\n",a,c,b);}else printf("  Storage unavailable.\n");}
#endif
appBackPrompt();}
static void systemInformationApp(void)
{ char host[128]="unknown",cwd[MS_PATH_MAX];gethostname(host,sizeof host-1);host[sizeof host-1]='\0';MS_GETCWD(cwd,sizeof cwd);appHeader("System Information");printf("\n  OS: %s\n  Version: %s\n  Architecture: %s\n  Host name: %s\n  User: %s\n  Working directory: %s\n  Compiler: %s\n",MS_NAME,MS_VERSION,archName(),host,os.user,cwd, __VERSION__);appBackPrompt();}
static void networkManagerApp(void)
{ appHeader("Network Manager");printf("\n  Interfaces:\n");
#ifndef _WIN32
{DIR*d=opendir("/sys/class/net");struct dirent*e;if(d){while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){char p[128],s[16]="?";FILE*f;snprintf(p,sizeof p,"/sys/class/net/%s/operstate",e->d_name);f=fopen(p,"r");if(f){fgets(s,sizeof s,f);fclose(f);}printf("  %-16s %s",e->d_name,s);}}closedir(d);}printf("\n  Internet test: use the Browser to test an actual URL.\n");
#else
printf("  Windows interface enumeration is not compiled into this build.\n");
#endif
appBackPrompt();}
static void appInstallerApp(void)
{ char b[160];const char*apps[]={"System Monitor","Task Manager","Text Editor Pro","Calendar","Notes","Disk Manager","System Information","Network Manager","Software Centre","Backup Manager","System Log Viewer","User Manager","Help Centre","Notification Centre","MetaShell Browser",NULL};appHeader("App Installer");printf("\n  Built-in installable modules (already included in this executable):\n");for(int i=0;apps[i];i++)printf("  %2d  %s\n",i+1,apps[i]);printf("\n  The installer records enabled modules; it never launches host installers.\n  Enter a module name to enable, or q to cancel: ");if(readLine(NULL,b,sizeof b)!=IN_OK||ciEqual(trim(b),"q"))return;{FILE*f=fopen("installed_apps.txt","ab");if(f){fprintf(f,"%s\n",trim(b));fclose(f);appLog("INSTALLER","enabled module %s",trim(b));appNotify("App Installer","Enabled %s.",trim(b));setNotice(ST_GOOD,"Module enabled in MetaDOS.");}else setNotice(ST_BAD,"Cannot update registry: %s",strerror(errno));}}
static void softwareCentreApp(void)
{appHeader("Software Centre");printf("\n  Built-in modules are available from System Apps.\n  Enabled registry entries:\n");if(viewTextFile("installed_apps.txt",0)!=0)printf("  (none recorded; built-ins remain available)\n");appBackPrompt();}
static int copyFileInOs(const char *src,const char *dst){FILE*a=fopen(src,"rb"),*b;char buf[4096];size_t n;if(!a)return -1;b=fopen(dst,"wb");if(!b){fclose(a);return -1;}while((n=fread(buf,1,sizeof buf,a))>0)if(fwrite(buf,1,n,b)!=n){fclose(a);fclose(b);return -1;}fclose(a);return fclose(b);}
static void backupManagerApp(void)
{char b[32];appHeader("Backup Manager");printf("\n  1 Backup user data   2 Restore   3 Back\n");if(readLine("  > ",b,sizeof b)!=IN_OK)return;if(!strcmp(trim(b),"1")){if(!pathIsDir("backups"))MS_MKDIR("backups");copyFileInOs("notes.txt","backups/notes.txt");copyFileInOs("todo.txt","backups/todo.txt");copyFileInOs("calendar.txt","backups/calendar.txt");copyFileInOs("system.log","backups/system.log");copyFileInOs("notifications.log","backups/notifications.log");appLog("BACKUP","created backup");appNotify("Backup Manager","Backup completed.");setNotice(ST_GOOD,"Backup created in backups/.");}else if(!strcmp(trim(b),"2")){if(!confirm("Restore and overwrite existing data?"))return;copyFileInOs("backups/notes.txt","notes.txt");copyFileInOs("backups/todo.txt","todo.txt");copyFileInOs("backups/calendar.txt","calendar.txt");copyFileInOs("backups/system.log","system.log");copyFileInOs("backups/notifications.log","notifications.log");appLog("BACKUP","restored backup");setNotice(ST_GOOD,"Backup restored.");}}
static void systemLogViewerApp(void)
{char b[160],*all=NULL;size_t n=0;for(;;){appHeader("System Log Viewer");if(viewTextFile("system.log",0)!=0)printf("\n  No log entries yet.\n");printf("\n  s <text> search   c clear   q back\n");if(readLine("  > ",b,sizeof b)!=IN_OK||ciEqual(trim(b),"q")){free(all);return;}if(b[0]=='s'){readWholeFile("system.log",&all,&n);if(all){char*p=all;while((p=strstr(p,b+2))) {char*e=strchr(p,'\n');if(e)*e='\0';printf("  %s\n",p);if(e)*e='\n';p+=strlen(b+2);}free(all);all=NULL;}}else if(b[0]=='c'&&confirm("Clear all system logs?")){remove("system.log");appLog("LOG","log cleared");}}}
static void userManagerApp(void)
{char b[256],why[128];for(;;){UserDb db;appHeader("User Manager");if(dbLoad(&db)>=0){for(size_t i=0;i<db.count;i++)if(db.items[i].kind!=U_INVALID)printf("  %-24s %s\n",db.items[i].name,ciEqual(db.items[i].name,os.user)?"(active)":"");dbFree(&db);}printf("\n  a Add account   r Rename active account   d Delete account   q Back\n");if(readLine("  > ",b,sizeof b)!=IN_OK||ciEqual(trim(b),"q"))return;if(b[0]=='a'){createAccount();}else if(b[0]=='r'){printf("  Use Settings to rename the active account.\n");}else if(b[0]=='d'){char u[64];if(readLine("  Username to delete: ",u,sizeof u)!=IN_OK||ciEqual(trim(u),os.user))continue;if(confirm("Delete this account?")){UserDb x;if(dbLoad(&x)==0){UserRec*r=dbFind(&x,trim(u));if(r){memmove(r,r+1,(x.count-(size_t)(r-x.items)-1)*sizeof *r);x.count--;dbSave(&x);appLog("USERS","deleted account %s",u);}dbFree(&x);}}} (void)why;}}
static void helpCentreApp(void)
{char b[160];for(;;){appHeader("Help Centre");printf("\n  MetaDOS keeps user data in the working directory.\n  File Manager manages files without launching host programs.\n  Terminal runs built-in commands only.\n  Text Editor uses :w to save and :q to leave.\n  Browser opens real HTTP/HTTPS pages: numbered links, back/forward, reload,\n  search, bookmarks, history and web forms (e fills, t toggles, x presses),\n  rendered as readable text with cookies kept between pages.\n  Backup Manager copies notes, tasks, events, logs and notifications.\n\n  Search help (optional) or q to return: ");if(readLine(NULL,b,sizeof b)!=IN_OK||ciEqual(trim(b),"q"))return;if(*trim(b))printf("\n  Search results for '%s': see the matching application help above.\n",trim(b));appBackPrompt();}}
static void notificationCentreApp(void)
{char b[32];for(;;){appHeader("Notification Centre");if(viewTextFile("notifications.log",0)!=0)printf("\n  No notifications.\n");printf("\n  d dismiss all   q back\n");if(readLine("  > ",b,sizeof b)!=IN_OK||ciEqual(trim(b),"q"))return;if(b[0]=='d'&&confirm("Dismiss all notifications?")){remove("notifications.log");setNotice(ST_GOOD,"Notifications dismissed.");}}}

/* ================= META DOS BROWSER ================= */

/*
 * A text-mode web browser. libcurl fetches the page; the HTML is flattened into
 * word-wrapped readable text and every hyperlink is numbered, so links can be
 * followed from the keyboard. Back/forward, reload, search, bookmarks and
 * history work without a mouse or a graphics stack. There is no JavaScript
 * engine: this is a reader, not a rendering engine.
 */

#ifdef HAVE_CURL

#define BROWSER_COLS       78           /* wrapped text width, in columns   */
#define BROWSER_LINES      14           /* page lines shown per screen      */
#define BROWSER_MAX_TEXT   (512 * 1024) /* rendered text cap                */
#define BROWSER_MAX_LINKS  300          /* link table per page              */
#define BROWSER_HISTORY    "browser_history.txt"
#define BROWSER_BOOKMARKS  "browser_bookmarks.txt"
#define BROWSER_UA \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) " \
    "MetaDOS-Browser/2.0 Safari/537.36"

static char browserError[200];

/* ---- growable byte buffer ---- */

typedef struct { char *p; size_t n, cap; int oom; } BBuf;

static void bbInit(BBuf *b) { b->p = NULL; b->n = b->cap = 0; b->oom = 0; }
static void bbFree(BBuf *b) { free(b->p); bbInit(b); }

static int bbReserve(BBuf *b, size_t extra)
{
    size_t cap;
    char *np;
    if (b->oom) return -1;
    if (b->n + extra + 1 <= b->cap) return 0;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra + 1) cap *= 2;
    np = (char *)realloc(b->p, cap);
    if (!np) { b->oom = 1; return -1; }
    b->p = np;
    b->cap = cap;
    return 0;
}

static void bbPut(BBuf *b, const char *s, size_t n)
{
    if (bbReserve(b, n) != 0) return;
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void bbPuts(BBuf *b, const char *s) { bbPut(b, s, strlen(s)); }
static void bbChar(BBuf *b, char c) { bbPut(b, &c, 1); }

static char *msDup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ---- small string helpers ---- */

static int ciPrefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++;
        prefix++;
    }
    return 1;
}

static const char *findCI(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle), i;
    if (nl == 0 || n < nl) return NULL;
    for (i = 0; i + nl <= n; i++)
        if (ciPrefix(hay + i, needle)) return hay + i;
    return NULL;
}

/* Truncate to at most `maxcols` visible columns, marking the cut with "...". */
static void truncCols(char *dst, size_t dsz, const char *src, int maxcols)
{
    int seen = 0;
    size_t o = 0;
    const char *p = src;
    if (dsz == 0) return;
    while (*p) {
        size_t l = 1;
        while (((unsigned char)p[l] & 0xC0) == 0x80) l++;
        if (maxcols > 4 && seen + 3 >= maxcols) {
            if (o + 4 < dsz) { memcpy(dst + o, "...", 3); o += 3; }
            break;
        }
        if (o + l + 1 >= dsz) break;
        memcpy(dst + o, p, l);
        o += l;
        p += l;
        seen++;
    }
    dst[o] = '\0';
}

/* ---- UTF-8 helpers ---- */

static int u8Len(const char *s, size_t avail)
{
    unsigned char c = (unsigned char)*s;
    int n = 1;
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    if ((size_t)n > avail) n = 1;
    return n;
}

static int u8Count(const char *s, size_t n)
{
    size_t i = 0;
    int c = 0;
    while (i < n) { i += (size_t)u8Len(s + i, n - i); c++; }
    return c;
}

static int utf8Encode(unsigned cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* ---- character entities ---- */

static const struct { const char *name, *rep; } ENTITIES[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" },
    { "nbsp", " " }, { "copy", "\xC2\xA9" }, { "reg", "\xC2\xAE" },
    { "trade", "\xE2\x84\xA2" }, { "mdash", "\xE2\x80\x94" }, { "ndash", "\xE2\x80\x93" },
    { "hellip", "\xE2\x80\xA6" }, { "lsquo", "\xE2\x80\x98" }, { "rsquo", "\xE2\x80\x99" },
    { "ldquo", "\xE2\x80\x9C" }, { "rdquo", "\xE2\x80\x9D" }, { "laquo", "\xC2\xAB" },
    { "raquo", "\xC2\xBB" }, { "bull", "\xE2\x80\xA2" }, { "middot", "\xC2\xB7" },
    { "deg", "\xC2\xB0" }, { "euro", "\xE2\x82\xAC" }, { "pound", "\xC2\xA3" },
    { "yen", "\xC2\xA5" }, { "cent", "\xC2\xA2" }, { "times", "\xC3\x97" },
    { "divide", "\xC3\xB7" }, { "plusmn", "\xC2\xB1" }, { "frac12", "\xC2\xBD" },
    { "larr", "\xE2\x86\x90" }, { "rarr", "\xE2\x86\x92" }, { "uarr", "\xE2\x86\x91" },
    { "darr", "\xE2\x86\x93" }, { "check", "\xE2\x9C\x93" }, { "shy", "" },
    { NULL, NULL }
};

/* If `s` starts with a character entity, append its text and return the number
 * of input bytes consumed; returns 0 when this is not an entity. */
static size_t emitEntity(const char *s, size_t n, BBuf *out)
{
    size_t i;
    if (n < 3 || s[0] != '&') return 0;
    for (i = 1; i < n && i < 12 && s[i] != ';'; i++)
        ;
    if (i >= n || s[i] != ';' || i < 2) return 0;

    if (s[1] == '#') {
        unsigned long cp = 0;
        int hex = (s[2] == 'x' || s[2] == 'X');
        size_t j = hex ? 3 : 2;
        char enc[4];
        int len;
        if (j >= i) return 0;
        for (; j < i; j++) {
            int d;
            unsigned char c = (unsigned char)s[j];
            if (hex && isdigit(c)) d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else if (!hex && isdigit(c)) d = c - '0';
            else return 0;
            cp = cp * (hex ? 16u : 10u) + (unsigned long)d;
            if (cp > 0x10FFFFul) return 0;
        }
        len = utf8Encode((unsigned)cp, enc);
        if (len > 0) bbPut(out, enc, (size_t)len);
        return i + 1;
    }
    {
        char name[12];
        size_t l = i - 1, k;
        if (l >= sizeof name) return 0;
        memcpy(name, s + 1, l);
        name[l] = '\0';
        for (k = 0; ENTITIES[k].name; k++) {
            if (!strcmp(name, ENTITIES[k].name)) {
                bbPuts(out, ENTITIES[k].rep);
                return i + 1;
            }
        }
    }
    return 0;
}

/* Copy text, decoding entities and dropping terminal control characters so a
 * hostile page cannot drive the terminal. */
static void decodeText(BBuf *out, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '&') {
            size_t used = emitEntity(s + i, n - i, out);
            if (used) { i += used; continue; }
        }
        if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F) { i++; continue; }
        bbChar(out, (char)c);
        i++;
    }
}

static void decodeInto(char *dst, size_t dsz, const char *s, size_t n)
{
    BBuf b;
    bbInit(&b);
    decodeText(&b, s, n);
    if (b.p) safeCopy(dst, dsz, b.p);
    else if (dsz) dst[0] = '\0';
    bbFree(&b);
}

/* ---- page model ---- */

#define BROWSER_MAX_FIELDS 64
#define BROWSER_MAX_FORMS  24
#define BROWSER_OPTIONS    8
#define BROWSER_OPT_LEN    44

/* One form control: a text box, checkbox, radio button, menu, textarea or
 * button. Controls are numbered from 1 in reading order so they can be used
 * from the keyboard. */
typedef struct {
    int form;                                     /* owning form, -1 = none */
    char type[12];                                /* text, checkbox, select */
    char name[72];                                /* submitted field name   */
    char label[132];                              /* shown to the user      */
    char value[400];
    int checked;                                  /* checkbox / radio state */
    int optionCount, optionIndex;
    char optionValue[BROWSER_OPTIONS][BROWSER_OPT_LEN];
    char optionLabel[BROWSER_OPTIONS][BROWSER_OPT_LEN];
} BrowserField;

typedef struct {
    char action[MS_PATH_MAX];
    char method[8];
} BrowserForm;

typedef struct {
    char *text;                 /* rendered, wrapped page text              */
    char **links;               /* unique link targets, numbered from 1     */
    int linkCount, linkCap;
    BrowserField fields[BROWSER_MAX_FIELDS];
    int fieldCount;
    BrowserForm forms[BROWSER_MAX_FORMS];
    int formCount;
    char *html;                 /* markup kept while the page has forms    */
    size_t htmlLen;
    char url[MS_PATH_MAX];      /* effective URL after redirects            */
    char title[160];
    char host[128];
    char type[64];
    long status;
} BrowserPage;

static void pageFree(BrowserPage *pg)
{
    int i;
    free(pg->text);
    free(pg->html);
    for (i = 0; i < pg->linkCount; i++) free(pg->links[i]);
    free(pg->links);
    memset(pg, 0, sizeof *pg);
}

static int pageAddLink(BrowserPage *pg, const char *url)
{
    int i;
    char *copy;
    for (i = 0; i < pg->linkCount; i++)
        if (!strcmp(pg->links[i], url)) return i + 1;
    if (pg->linkCount >= BROWSER_MAX_LINKS) return 0;
    if (pg->linkCount == pg->linkCap) {
        int nc = pg->linkCap ? pg->linkCap * 2 : 32;
        char **np = (char **)realloc(pg->links, (size_t)nc * sizeof *np);
        if (!np) return 0;
        pg->links = np;
        pg->linkCap = nc;
    }
    copy = msDup(url);
    if (!copy) return 0;
    pg->links[pg->linkCount++] = copy;
    return pg->linkCount;
}

/* ---- form controls ---- */

static int pageAddField(BrowserPage *pg, int form)
{
    BrowserField *f;
    if (pg->fieldCount >= BROWSER_MAX_FIELDS) return 0;
    f = &pg->fields[pg->fieldCount];
    memset(f, 0, sizeof *f);
    f->form = form;
    pg->fieldCount++;
    return pg->fieldCount;
}

static BrowserField *pageField(BrowserPage *pg, int no)
{
    if (no < 1 || no > pg->fieldCount) return NULL;
    return &pg->fields[no - 1];
}

static int fieldIsText(const BrowserField *f)
{
    return strcmp(f->type, "checkbox") && strcmp(f->type, "radio") &&
           strcmp(f->type, "select") && strcmp(f->type, "submit") &&
           strcmp(f->type, "reset") && strcmp(f->type, "button") &&
           strcmp(f->type, "image") && strcmp(f->type, "hidden");
}

/* Uncheck every other radio button in the same group. */
static void pageRadioClear(BrowserPage *pg, int form, const char *name)
{
    int i;
    for (i = 0; i < pg->fieldCount; i++)
        if (pg->fields[i].form == form && !strcmp(pg->fields[i].type, "radio") &&
            !strcmp(pg->fields[i].name, name))
            pg->fields[i].checked = 0;
}

/* ---- URL handling ---- */

/* Collapse "." and ".." segments in a path (query/fragment already removed). */
static void normalizePathOnly(const char *in, char *out, size_t osz)
{
    const char *s = in;
    size_t o = 0;
    if (osz < 2) { if (osz) out[0] = '\0'; return; }
    out[0] = '\0';
    while (*s == '/') s++;
    while (*s) {
        const char *e = s;
        while (*e && *e != '/') e++;
        if (e - s == 1 && s[0] == '.') {
            /* current directory: drop */
        } else if (e - s == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            out[o] = '\0';
        } else if (e > s) {
            if (o + (size_t)(e - s) + 2 < osz) {
                out[o++] = '/';
                memcpy(out + o, s, (size_t)(e - s));
                o += (size_t)(e - s);
                out[o] = '\0';
            }
        }
        s = e;
        while (*s == '/') s++;
    }
    if (o == 0) { out[0] = '/'; out[1] = '\0'; }
}

static void browserNormalizeUrl(const char *url, char *out, size_t osz)
{
    const char *sep = strstr(url, "://");
    char scheme[16], auth[256], path[MS_PATH_MAX * 2], query[MS_PATH_MAX], norm[MS_PATH_MAX * 2];
    size_t sl, al;
    const char *q;
    char *m;

    if (!sep) { safeCopy(out, osz, url); return; }
    sl = (size_t)(sep - url);
    if (sl >= sizeof scheme) sl = sizeof scheme - 1;
    memcpy(scheme, url, sl);
    scheme[sl] = '\0';
    q = sep + 3;
    al = strcspn(q, "/?#");
    if (al >= sizeof auth) al = sizeof auth - 1;
    memcpy(auth, q, al);
    auth[al] = '\0';
    safeCopy(path, sizeof path, q + al);

    query[0] = '\0';
    m = path + strcspn(path, "?#");
    if (*m) {
        safeCopy(query, sizeof query, m);
        *m = '\0';
    }
    normalizePathOnly(path, norm, sizeof norm);
    snprintf(out, osz, "%s://%s%s%s", scheme, auth, norm, query);
}

static void browserResolve(const char *base, const char *href, char *out, size_t osz)
{
    char scheme[16] = "", auth[256] = "", path[MS_PATH_MAX] = "", cand[MS_PATH_MAX * 3];
    const char *sep, *q, *hash, *quest;
    size_t sl, al, dl;

    sep = strstr(base, "://");
    if (sep) {
        sl = (size_t)(sep - base);
        if (sl >= sizeof scheme) sl = sizeof scheme - 1;
        memcpy(scheme, base, sl);
        scheme[sl] = '\0';
        q = sep + 3;
        al = strcspn(q, "/?#");
        if (al >= sizeof auth) al = sizeof auth - 1;
        memcpy(auth, q, al);
        auth[al] = '\0';
        safeCopy(path, sizeof path, q + al);
    } else {
        safeCopy(path, sizeof path, base);
    }

    if (strstr(href, "://")) {
        browserNormalizeUrl(href, out, osz);
        return;
    }
    if (href[0] == '/' && href[1] == '/' && scheme[0]) {
        snprintf(cand, sizeof cand, "%s:%s", scheme, href);
        browserNormalizeUrl(cand, out, osz);
        return;
    }
    if (!scheme[0]) { safeCopy(out, osz, href); return; }

    if (href[0] == '#') {
        hash = strchr(path, '#');
        dl = hash ? (size_t)(hash - path) : strlen(path);
        snprintf(cand, sizeof cand, "%s://%s%.*s%s", scheme, auth, (int)dl, path, href);
        safeCopy(out, osz, cand);
        return;
    }
    if (href[0] == '?') {
        quest = strpbrk(path, "?#");
        dl = quest ? (size_t)(quest - path) : strlen(path);
        snprintf(cand, sizeof cand, "%s://%s%.*s%s", scheme, auth, (int)dl, path, href);
        safeCopy(out, osz, cand);
        return;
    }
    if (href[0] == '/') {
        snprintf(cand, sizeof cand, "%s://%s%s", scheme, auth, href);
        browserNormalizeUrl(cand, out, osz);
        return;
    }
    {
        const char *last = NULL, *s;
        for (s = path; *s && *s != '?' && *s != '#'; s++)
            if (*s == '/') last = s;
        if (!last) snprintf(cand, sizeof cand, "%s://%s/%s", scheme, auth, href);
        else {
            size_t dl2 = (size_t)(last - path) + 1;
            snprintf(cand, sizeof cand, "%s://%s%.*s%s", scheme, auth, (int)dl2, path, href);
        }
    }
    browserNormalizeUrl(cand, out, osz);
}

static void browserSearch(const char *terms, char *out, size_t osz)
{
    CURL *c = curl_easy_init();
    char *esc = c ? curl_easy_escape(c, terms, 0) : NULL;
    snprintf(out, osz, "https://www.bing.com/search?q=%s", esc ? esc : "");
    if (esc) curl_free(esc);
    if (c) curl_easy_cleanup(c);
}

/* Turn what the user typed into a URL: an address stays an address, a bare
 * domain gets https://, and anything else becomes a web search. */
static void browserNormalizeInput(const char *raw, char *out, size_t osz)
{
    char t[MS_PATH_MAX];
    char *v;
    int local;

    safeCopy(t, sizeof t, raw);
    v = trim(t);
    if (!*v) { safeCopy(out, osz, "https://example.com/"); return; }
    if (strstr(v, "://")) { safeCopy(out, osz, v); return; }
    if (v[0] == '/' && v[1] == '/') { snprintf(out, osz, "https:%s", v); return; }
    if (strpbrk(v, " \t")) { browserSearch(v, out, osz); return; }
    if (!strchr(v, '.') && !strchr(v, ':')) { browserSearch(v, out, osz); return; }
    local = ciPrefix(v, "localhost") || !strncmp(v, "127.", 4) || !strncmp(v, "192.168.", 8);
    snprintf(out, osz, "%s://%s", local ? "http" : "https", v);
}

/* ---- HTML to text renderer ---- */

typedef struct {
    BBuf out;
    int col;
    int indent;
    int pendingNL;      /* 0 none, 1 new line, 2 blank line */
    int space;
    int pre;
    int linkOpen;
    int linkNo;
    int anchorText;
    int olDepth;
    int olNext[4];
    int truncated;
    int form;           /* current <form> index, -1 when outside a form     */
    int capture;        /* 0 none, 1 textarea, 2 option, 3 button           */
    int capField;       /* field number being captured                      */
    BBuf cap;           /* text collected for that element                  */
    char optValue[64];  /* <option> value, kept until its closing tag       */
    int optSelected;
    const BrowserField *restore;  /* values carried over a redraw            */
    int restoreCount;
} Render;

static void rIndent(Render *r)
{
    int i;
    for (i = 0; i < r->indent && r->col < BROWSER_COLS; i++) {
        bbChar(&r->out, ' ');
        r->col++;
    }
}

static void rWrap(Render *r)
{
    bbChar(&r->out, '\n');
    r->col = 0;
    r->space = 0;
    rIndent(r);
}

static void rFlush(Render *r)
{
    if (!r->pendingNL) return;
    while (r->pendingNL > 0) {
        bbChar(&r->out, '\n');
        r->pendingNL--;
    }
    r->col = 0;
    r->space = 0;
    rIndent(r);
}

static void rBreak(Render *r, int lines)
{
    if (r->out.n == 0) return;
    if (lines > 2) lines = 2;
    if (r->pendingNL < lines) r->pendingNL = lines;
    r->space = 0;
}

static void rWord(Render *r, const char *w, size_t n)
{
    int wl;
    size_t i = 0;

    if (n == 0 || r->out.n >= BROWSER_MAX_TEXT) return;
    wl = u8Count(w, n);
    rFlush(r);
    if (r->space) {
        if (r->col > r->indent && r->col + 1 + wl > BROWSER_COLS) rWrap(r);
        else { bbChar(&r->out, ' '); r->col++; }
        r->space = 0;
    }
    if (r->col > r->indent && r->col + wl > BROWSER_COLS) rWrap(r);
    while (i < n) {
        int len;
        if (r->col >= BROWSER_COLS) rWrap(r);
        len = u8Len(w + i, n - i);
        bbPut(&r->out, w + i, (size_t)len);
        r->col++;
        i += (size_t)len;
    }
}

static void rText(Render *r, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (isspace((unsigned char)s[i])) {
            r->space = 1;
            i++;
            continue;
        }
        {
            size_t j = i;
            while (j < n && !isspace((unsigned char)s[j])) j++;
            rWord(r, s + i, j - i);
            i = j;
        }
    }
}

/* Preformatted text: keep spaces and line breaks, wrap only when a line is
 * wider than the page. */
static void rPreText(Render *r, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\n') {
            r->pendingNL = 0;
            bbChar(&r->out, '\n');
            r->col = 0;
            i++;
            continue;
        }
        if (c == '\r') { i++; continue; }
        if (c == '\t') {
            int k;
            for (k = 0; k < 4; k++) {
                if (r->col >= BROWSER_COLS) {
                    bbChar(&r->out, '\n');
                    r->col = 0;
                }
                bbChar(&r->out, ' ');
                r->col++;
            }
            i++;
            continue;
        }
        {
            int len = u8Len(s + i, n - i);
            if (r->col >= BROWSER_COLS) {
                bbChar(&r->out, '\n');
                r->col = 0;
            }
            bbPut(&r->out, s + i, (size_t)len);
            r->col++;
            i += (size_t)len;
        }
    }
}

static void rMarker(Render *r, int no)
{
    char m[16];
    if (no <= 0) return;
    snprintf(m, sizeof m, "[%d]", no);
    r->space = 1;
    rWord(r, m, strlen(m));
}

/* Put the value a control had before a redraw back into it. */
static void fieldRestore(Render *r, BrowserPage *pg, int no)
{
    BrowserField *f;
    if (!r->restore || no < 1 || no > r->restoreCount) return;
    f = &pg->fields[no - 1];
    safeCopy(f->value, sizeof f->value, r->restore[no - 1].value);
    f->checked = r->restore[no - 1].checked;
    f->optionIndex = r->restore[no - 1].optionIndex;
}

/* Draw one form control inline, e.g. "{2} Search: ______" or "{3} [x] News". */
static void rField(Render *r, const BrowserPage *pg, int no)
{
    const BrowserField *f = &pg->fields[no - 1];
    char line[800];
    const char *shown;

    if (!strcmp(f->type, "hidden")) return;
    if (!strcmp(f->type, "checkbox"))
        snprintf(line, sizeof line, "{%d} %s %s", no, f->checked ? "[x]" : "[ ]", f->label);
    else if (!strcmp(f->type, "radio"))
        snprintf(line, sizeof line, "{%d} %s %s", no, f->checked ? "(x)" : "( )", f->label);
    else if (!strcmp(f->type, "submit") || !strcmp(f->type, "button") ||
             !strcmp(f->type, "reset") || !strcmp(f->type, "image"))
        snprintf(line, sizeof line, "{%d} [%s]", no, f->label[0] ? f->label : "Submit");
    else if (!strcmp(f->type, "select")) {
        shown = f->optionIndex < f->optionCount ? f->optionLabel[f->optionIndex] : f->value;
        snprintf(line, sizeof line, "{%d} %s: [%s]", no, f->label, shown && shown[0] ? shown : "-");
    } else if (!strcmp(f->type, "password"))
        snprintf(line, sizeof line, "{%d} %s: %s", no, f->label, f->value[0] ? "********" : "________");
    else if (fieldIsText(f))
        snprintf(line, sizeof line, "{%d} %s: %s", no, f->label, f->value[0] ? f->value : "________");
    else
        return;
    rBreak(r, 1);
    rText(r, line, strlen(line));
}

static void emitHtmlText(Render *r, const char *s, size_t n)
{
    BBuf b;
    size_t i;

    bbInit(&b);
    decodeText(&b, s, n);
    if (!b.p) { bbFree(&b); return; }
    if (r->capture) {
        /* Inside <textarea>, <option> or <button>: collect the text instead
         * of rendering it. */
        for (i = 0; i < b.n; i++) {
            char c = b.p[i];
            if (c == '\n' || c == '\t' || c == '\r') c = ' ';
            bbChar(&r->cap, c);
        }
        bbFree(&b);
        return;
    }
    if (r->pre) rPreText(r, b.p, b.n);
    else rText(r, b.p, b.n);
    if (r->linkOpen && !r->anchorText) {
        for (i = 0; i < b.n; i++)
            if (!isspace((unsigned char)b.p[i])) { r->anchorText = 1; break; }
    }
    bbFree(&b);
}

static const char *const SKIP_TAGS[] = {
    "script", "style", "head", "noscript", "svg", "template", "iframe",
    "object", "canvas", NULL
};
static const char *const BLOCK2_TAGS[] = {
    "p", "pre", "blockquote", "ul", "ol", "table", "hr", "section", "article",
    "main", "header", "footer", "aside", "nav", "figure", "dl", "form",
    "fieldset", "address", NULL
};
static const char *const BLOCK1_TAGS[] = {
    "div", "tr", "td", "th", "dt", "dd", "br", "figcaption", "tbody", "thead",
    "tfoot", "caption", "details", "summary", NULL
};

static int tagIn(const char *name, const char *const *list)
{
    for (; *list; list++)
        if (!strcmp(name, *list)) return 1;
    return 0;
}

/* Skip the body of an element we do not render, e.g. <script>...</script>. */
static const char *skipElement(const char *p, const char *end, const char *name)
{
    char close[24];
    size_t cl;
    snprintf(close, sizeof close, "</%s", name);
    cl = strlen(close);
    while (p < end) {
        if (*p == '<' && (size_t)(end - p) > cl && ciPrefix(p, close)) {
            p += cl;
            while (p < end && *p != '>') p++;
            return p < end ? p + 1 : end;
        }
        p++;
    }
    return end;
}

/* Find the '>' that closes a tag, ignoring ones inside quoted values. */
static const char *tagEnd(const char *q, const char *end)
{
    while (q < end && *q != '>') {
        if (*q == '"' || *q == '\'') {
            char quote = *q++;
            while (q < end && *q != quote) q++;
            if (q < end) q++;
            continue;
        }
        q++;
    }
    return q;
}

/* Read attributes from the text of one tag. Returns 1 when `want` is present;
 * its value, when it has one, is decoded into `out`. */
static int attrScan(const char *attrs, size_t n, const char *want, char *out, size_t osz)
{
    size_t i = 0;
    if (out && osz) out[0] = '\0';
    while (i < n) {
        char name[24];
        size_t nl = 0, vs = 0, vn = 0;
        unsigned char c;

        while (i < n && isspace((unsigned char)attrs[i])) i++;
        if (i < n && attrs[i] == '/') { i++; continue; }
        if (i >= n) break;
        c = (unsigned char)attrs[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != ':') { i++; continue; }
        while (i < n && !isspace((unsigned char)attrs[i]) && attrs[i] != '=' &&
               attrs[i] != '>') {
            if (nl + 1 < sizeof name) name[nl++] = (char)tolower((unsigned char)attrs[i]);
            i++;
        }
        name[nl] = '\0';
        while (i < n && isspace((unsigned char)attrs[i])) i++;
        if (i < n && attrs[i] == '=') {
            i++;
            while (i < n && isspace((unsigned char)attrs[i])) i++;
            if (i < n && (attrs[i] == '"' || attrs[i] == '\'')) {
                char quote = attrs[i++];
                vs = i;
                while (i < n && attrs[i] != quote) i++;
                vn = i - vs;
                if (i < n) i++;
            } else {
                vs = i;
                while (i < n && !isspace((unsigned char)attrs[i])) i++;
                vn = i - vs;
            }
        }
        if (nl && !strcmp(name, want)) {
            if (vn && out) decodeInto(out, osz, attrs + vs, vn);
            return 1;
        }
    }
    return 0;
}

static void attrValue(const char *attrs, size_t n, const char *want, char *out, size_t osz)
{
    attrScan(attrs, n, want, out, osz);
}

static int attrHas(const char *attrs, size_t n, const char *want)
{
    return attrScan(attrs, n, want, NULL, 0);
}

static void capReset(Render *r)
{
    if (r->cap.p) { r->cap.n = 0; r->cap.p[0] = '\0'; }
}

static void capText(const Render *r, char *out, size_t osz)
{
    if (osz) safeCopy(out, osz, r->cap.p ? trim(r->cap.p) : "");
}

static void applyTag(Render *r, BrowserPage *pg, const char *name, int closing,
                     const char *attrs, size_t alen)
{
    char href[MS_PATH_MAX], alt[200];

    href[0] = '\0';
    alt[0] = '\0';

    /* Inside a control we only care about the tag that ends it. */
    if (r->capture &&
        !((r->capture == 1 && !strcmp(name, "textarea")) ||
          (r->capture == 2 && (!strcmp(name, "option") || !strcmp(name, "select") ||
                               !strcmp(name, "optgroup"))) ||
          (r->capture == 3 && !strcmp(name, "button"))))
        return;

    if (!strcmp(name, "form")) {
        if (closing) {
            r->form = -1;
            rBreak(r, 2);
        } else {
            char action[MS_PATH_MAX], method[16];
            int idx;
            attrValue(attrs, alen, "action", action, sizeof action);
            attrValue(attrs, alen, "method", method, sizeof method);
            rBreak(r, 2);
            if (pg->formCount >= BROWSER_MAX_FORMS) { r->form = -1; return; }
            idx = pg->formCount++;
            memset(&pg->forms[idx], 0, sizeof pg->forms[0]);
            if (!action[0])
                safeCopy(pg->forms[idx].action, sizeof pg->forms[idx].action, pg->url);
            else
                browserResolve(pg->url, action, pg->forms[idx].action,
                               sizeof pg->forms[idx].action);
            safeCopy(pg->forms[idx].method, sizeof pg->forms[idx].method,
                     ciPrefix(method, "post") ? "POST" : "GET");
            r->form = idx;
        }
        return;
    }

    if (!strcmp(name, "input") && !closing) {
        char type[16], fname[80], value[400], ph[160];
        BrowserField *f;
        int no;

        attrValue(attrs, alen, "type", type, sizeof type);
        attrValue(attrs, alen, "name", fname, sizeof fname);
        attrValue(attrs, alen, "value", value, sizeof value);
        attrValue(attrs, alen, "placeholder", ph, sizeof ph);
        if (!type[0]) safeCopy(type, sizeof type, "text");
        no = pageAddField(pg, r->form);
        if (!no) return;
        f = pageField(pg, no);
        safeCopy(f->type, sizeof f->type,
                 ciEqual(type, "submit") ? "submit" : (ciEqual(type, "reset") ? "reset" : type));
        safeCopy(f->name, sizeof f->name, fname);
        safeCopy(f->value, sizeof f->value, value);
        if (!strcmp(f->type, "submit") || !strcmp(f->type, "image") ||
            !strcmp(f->type, "button") || !strcmp(f->type, "reset"))
            safeCopy(f->label, sizeof f->label,
                     value[0] ? value : (!strcmp(f->type, "reset") ? "Reset" : "Submit"));
        else
            safeCopy(f->label, sizeof f->label,
                     ph[0] ? ph
                           : (fname[0] ? fname
                                       : (ciEqual(type, "password") ? "Password" : "Text")));
        if (ciEqual(type, "radio") && attrHas(attrs, alen, "checked"))
            pageRadioClear(pg, r->form, f->name);
        if (ciEqual(type, "checkbox") || ciEqual(type, "radio")) {
            f->checked = attrHas(attrs, alen, "checked");
            if (!f->value[0]) safeCopy(f->value, sizeof f->value, "on");
        }
        fieldRestore(r, pg, no);
        rField(r, pg, no);
        return;
    }

    if (!strcmp(name, "textarea")) {
        if (closing) {
            if (r->capture == 1) {
                BrowserField *f = pageField(pg, r->capField);
                char text[400];
                capText(r, text, sizeof text);
                if (f) safeCopy(f->value, sizeof f->value, text);
                r->capture = 0;
                fieldRestore(r, pg, r->capField);
                rField(r, pg, r->capField);
            }
            return;
        }
        {
            char fname[80], ph[160];
            BrowserField *f;
            int no;
            attrValue(attrs, alen, "name", fname, sizeof fname);
            attrValue(attrs, alen, "placeholder", ph, sizeof ph);
            no = pageAddField(pg, r->form);
            if (!no) return;
            f = pageField(pg, no);
            safeCopy(f->type, sizeof f->type, "textarea");
            safeCopy(f->name, sizeof f->name, fname);
            safeCopy(f->label, sizeof f->label,
                     ph[0] ? ph : (fname[0] ? fname : "Text"));
            r->capture = 1;
            r->capField = no;
            capReset(r);
        }
        return;
    }

    if (!strcmp(name, "select")) {
        if (closing) {
            if (r->capture == 2) {
                BrowserField *f = pageField(pg, r->capField);
                if (f) {
                    if (f->optionCount == 0) {
                        safeCopy(f->optionValue[0], BROWSER_OPT_LEN, "");
                        safeCopy(f->optionLabel[0], BROWSER_OPT_LEN, "-");
                        f->optionCount = 1;
                    }
                    if (f->optionIndex < 0 || f->optionIndex >= f->optionCount)
                        f->optionIndex = 0;
                    safeCopy(f->value, sizeof f->value, f->optionValue[f->optionIndex]);
                }
                r->capture = 0;
                rField(r, pg, r->capField);
            }
            return;
        }
        {
            char fname[80];
            BrowserField *f;
            int no;
            attrValue(attrs, alen, "name", fname, sizeof fname);
            no = pageAddField(pg, r->form);
            if (!no) return;
            f = pageField(pg, no);
            safeCopy(f->type, sizeof f->type, "select");
            safeCopy(f->name, sizeof f->name, fname);
            safeCopy(f->label, sizeof f->label, fname[0] ? fname : "Choice");
            f->optionIndex = 0;
            fieldRestore(r, pg, no);
            r->capture = 2;
            r->capField = no;
            capReset(r);
        }
        return;
    }

    if (!strcmp(name, "option")) {
        BrowserField *f;
        if (r->capture != 2) return;
        if (!closing) {
            attrValue(attrs, alen, "value", r->optValue, sizeof r->optValue);
            r->optSelected = attrHas(attrs, alen, "selected");
            capReset(r);
            return;
        }
        f = pageField(pg, r->capField);
        if (f && f->optionCount < BROWSER_OPTIONS) {
            char option[BROWSER_OPT_LEN];
            int idx;
            capText(r, option, sizeof option);
            idx = f->optionCount++;
            if (!r->optValue[0]) safeCopy(r->optValue, sizeof r->optValue, option);
            safeCopy(f->optionValue[idx], BROWSER_OPT_LEN, r->optValue);
            safeCopy(f->optionLabel[idx], BROWSER_OPT_LEN, option[0] ? option : r->optValue);
            if (r->optSelected) f->optionIndex = idx;
        }
        capReset(r);
        return;
    }

    if (!strcmp(name, "optgroup")) return;

    if (!strcmp(name, "button")) {
        if (closing) {
            if (r->capture == 3) {
                BrowserField *f = pageField(pg, r->capField);
                char text[132];
                capText(r, text, sizeof text);
                if (f && text[0]) safeCopy(f->label, sizeof f->label, text);
                r->capture = 0;
                rField(r, pg, r->capField);
            }
            return;
        }
        {
            char type[16], fname[80], value[400];
            BrowserField *f;
            int no;
            attrValue(attrs, alen, "type", type, sizeof type);
            attrValue(attrs, alen, "name", fname, sizeof fname);
            attrValue(attrs, alen, "value", value, sizeof value);
            no = pageAddField(pg, r->form);
            if (!no) return;
            f = pageField(pg, no);
            safeCopy(f->type, sizeof f->type,
                     ciPrefix(type, "reset") ? "reset" : "submit");
            safeCopy(f->name, sizeof f->name, fname);
            safeCopy(f->value, sizeof f->value, value);
            safeCopy(f->label, sizeof f->label, "Submit");
            fieldRestore(r, pg, no);
            r->capture = 3;
            r->capField = no;
            capReset(r);
        }
        return;
    }

    if (!strcmp(name, "a")) {
        attrValue(attrs, alen, "href", href, sizeof href);
        if (closing) {
            if (r->linkOpen) {
                if (r->anchorText && r->linkNo > 0) {
                    int keep = r->pendingNL;
                    r->pendingNL = 0;
                    rMarker(r, r->linkNo);
                    if (r->pendingNL < keep) r->pendingNL = keep;
                }
                r->linkOpen = 0;
                r->linkNo = 0;
                r->anchorText = 0;
            }
        } else if (href[0] && !ciPrefix(href, "javascript:") && !ciPrefix(href, "mailto:")) {
            char abs[MS_PATH_MAX];
            browserResolve(pg->url, href, abs, sizeof abs);
            if (abs[0]) {
                int no = pageAddLink(pg, abs);
                if (no > 0) {
                    r->linkNo = no;
                    r->linkOpen = 1;
                    r->anchorText = 0;
                }
            }
        }
        return;
    }

    if (!strcmp(name, "h1") || !strcmp(name, "h2") || !strcmp(name, "h3") ||
        !strcmp(name, "h4") || !strcmp(name, "h5") || !strcmp(name, "h6")) {
        if (closing) { rBreak(r, 1); return; }
        rBreak(r, 2);
        if (name[1] == '1') rWord(r, "#", 1);
        else if (name[1] == '2') rWord(r, "##", 2);
        else rWord(r, "###", 3);
        r->space = 1;
        return;
    }

    if (!strcmp(name, "li")) {
        char bullet[16];
        if (closing) { rBreak(r, 1); return; }
        rBreak(r, 1);
        if (r->olDepth > 0) {
            snprintf(bullet, sizeof bullet, "%d.", r->olNext[r->olDepth - 1]);
            if (r->olNext[r->olDepth - 1] < 9999) r->olNext[r->olDepth - 1]++;
        } else {
            safeCopy(bullet, sizeof bullet, "-");
        }
        rWord(r, bullet, strlen(bullet));
        r->space = 1;
        return;
    }

    if (!strcmp(name, "ul")) {
        if (closing) {
            if (r->indent >= 2) r->indent -= 2;
            rBreak(r, 1);
        } else {
            rBreak(r, 2);
            r->indent += 2;
        }
        return;
    }
    if (!strcmp(name, "ol")) {
        if (closing) {
            if (r->olDepth > 0) r->olDepth--;
            if (r->indent >= 2) r->indent -= 2;
            rBreak(r, 1);
        } else {
            rBreak(r, 2);
            if (r->olDepth < 4) {
                r->olNext[r->olDepth] = 1;
                r->olDepth++;
            }
            r->indent += 2;
        }
        return;
    }
    if (!strcmp(name, "blockquote")) {
        if (closing) {
            if (r->indent >= 2) r->indent -= 2;
            rBreak(r, 2);
        } else {
            rBreak(r, 2);
            r->indent += 2;
        }
        return;
    }
    if (!strcmp(name, "pre")) {
        if (closing) {
            r->pre = 0;
            if (r->col > 0) { bbChar(&r->out, '\n'); r->col = 0; }
            rBreak(r, 2);
        } else {
            rBreak(r, 2);
            rFlush(r);
            r->pre = 1;
        }
        return;
    }
    if (!strcmp(name, "br")) { rBreak(r, 1); return; }
    if (!strcmp(name, "hr")) {
        int i;
        rBreak(r, 2);
        rFlush(r);
        for (i = 0; i < 46; i++) bbChar(&r->out, '-');
        bbChar(&r->out, '\n');
        r->col = 0;
        rBreak(r, 2);
        return;
    }
    if (!strcmp(name, "img")) {
        attrValue(attrs, alen, "alt", alt, sizeof alt);
        if (!closing && alt[0]) {
            char buf[240];
            snprintf(buf, sizeof buf, "[image: %s]", alt);
            r->space = 1;
            rWord(r, buf, strlen(buf));
            if (r->linkOpen) r->anchorText = 1;
        }
        return;
    }
    if (tagIn(name, BLOCK2_TAGS)) { if (closing) rBreak(r, 1); else rBreak(r, 2); return; }
    if (tagIn(name, BLOCK1_TAGS)) { rBreak(r, 1); return; }
}

/* Strip trailing spaces and collapse runs of blank lines. */
static void cleanText(BBuf *b)
{
    size_t i, o = 0;
    int nl = 0;
    if (!b->p) return;
    for (i = 0; i < b->n; i++) {
        char c = b->p[i];
        if (c == '\n') {
            while (o > 0 && b->p[o - 1] == ' ') o--;
            nl++;
            if (nl > 2) continue;
            b->p[o++] = '\n';
        } else if (c == '\r') {
            continue;
        } else {
            nl = 0;
            b->p[o++] = c;
        }
    }
    while (o > 0 && (b->p[o - 1] == '\n' || b->p[o - 1] == ' ')) o--;
    if (o) b->p[o++] = '\n';
    b->p[o] = '\0';
    b->n = o;
}

static void renderHtmlRestore(BrowserPage *pg, const char *html, size_t len,
                              const BrowserField *restore, int restoreCount)
{
    Render r;
    const char *p = html, *end = html + len;

    memset(&r, 0, sizeof r);
    bbInit(&r.out);
    bbInit(&r.cap);
    r.form = -1;
    r.restore = restore;
    r.restoreCount = restoreCount;

    while (p < end && r.out.n < BROWSER_MAX_TEXT) {
        if (*p != '<') {
            const char *q = p;
            while (q < end && *q != '<') q++;
            emitHtmlText(&r, p, (size_t)(q - p));
            p = q;
            continue;
        }
        if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            const char *q = p + 4;
            while (q + 2 < end && !(q[0] == '-' && q[1] == '-' && q[2] == '>')) q++;
            p = (q + 2 < end) ? q + 3 : end;
            continue;
        }
        if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
            const char *q = p + 1;
            while (q < end && *q != '>') q++;
            p = (q < end) ? q + 1 : end;
            continue;
        }
        {
            const char *q = p + 1;
            const char *attrs, *gt;
            char name[16];
            size_t nl = 0, alen;
            int closing = 0;

            if (q < end && *q == '/') { closing = 1; q++; }
            while (q < end && isspace((unsigned char)*q)) q++;
            while (q < end && nl + 1 < sizeof name &&
                   (isalnum((unsigned char)*q) || *q == ':' || *q == '-'))
                name[nl++] = (char)tolower((unsigned char)*q++);
            name[nl] = '\0';
            attrs = q;
            gt = tagEnd(q, end);
            alen = (size_t)(gt - attrs);
            p = (gt < end) ? gt + 1 : end;

            if (!closing && tagIn(name, SKIP_TAGS)) {
                p = skipElement(p, end, name);
                continue;
            }
            applyTag(&r, pg, name, closing, attrs, alen);
        }
    }

    rFlush(&r);
    if (r.truncated || r.out.n >= BROWSER_MAX_TEXT)
        bbPuts(&r.out, "\n[Page truncated: too large to display]\n");
    cleanText(&r.out);
    bbFree(&r.cap);
    pg->text = r.out.p;
    if (!pg->text) pg->text = msDup("\n");
}

static void renderHtml(BrowserPage *pg, const char *html, size_t len)
{
    renderHtmlRestore(pg, html, len, NULL, 0);
}

static void renderPlainText(BrowserPage *pg, const char *s, size_t n)
{
    Render r;
    size_t i = 0;

    memset(&r, 0, sizeof r);
    bbInit(&r.out);
    while (i < n && r.out.n < BROWSER_MAX_TEXT) {
        size_t j = i;
        while (j < n && s[j] != '\n') j++;
        rText(&r, s + i, j - i);
        rBreak(&r, 1);
        i = (j < n) ? j + 1 : n;
    }
    rFlush(&r);
    cleanText(&r.out);
    pg->text = r.out.p;
    if (!pg->text) pg->text = msDup("\n");
}

static void htmlTitle(const char *html, size_t len, char *out, size_t osz)
{
    const char *start, *gt, *close, *end = html + len;
    BBuf b;

    if (osz) out[0] = '\0';
    start = findCI(html, len, "<title");
    if (!start) return;
    for (gt = start; gt < end && *gt != '>'; gt++)
        ;
    if (gt >= end) return;
    gt++;
    close = findCI(gt, (size_t)(end - gt), "</title");
    if (!close) close = end;
    bbInit(&b);
    decodeText(&b, gt, (size_t)(close - gt));
    if (b.p) {
        size_t i, o = 0;
        int space = 0;
        for (i = 0; i < b.n && o + 1 < osz; i++) {
            unsigned char c = (unsigned char)b.p[i];
            if (isspace(c)) { space = 1; continue; }
            if (space && o > 0 && o + 1 < osz) out[o++] = ' ';
            space = 0;
            out[o++] = (char)c;
        }
        out[o] = '\0';
    }
    bbFree(&b);
}

static int looksBinary(const char *b, size_t n)
{
    size_t lim = n < 4096 ? n : 4096;
    if (!b) return 0;
    return memchr(b, '\0', lim) != NULL;
}

static int looksLikeHtml(const char *type, const char *body, size_t n)
{
    if (type && (ciPrefix(type, "text/html") || ciPrefix(type, "application/xhtml"))) return 1;
    if (type && *type && !ciPrefix(type, "text/plain")) return 0;
    if (body && n) {
        size_t lim = n < 2048 ? n : 2048;
        if (findCI(body, lim, "<html") || findCI(body, lim, "<!doctype")) return 1;
    }
    return 0;
}

/* ---- fetching ---- */

static size_t browserCurlWrite(void *ptr, size_t sz, size_t nm, void *ud)
{
    BBuf *b = (BBuf *)ud;
    size_t n = sz * nm;
    if (b->n + n > MAX_VIEW_BYTES) return 0;
    if (bbReserve(b, n) != 0) return 0;
    memcpy(b->p + b->n, ptr, n);
    b->n += n;
    b->p[b->n] = '\0';
    return n;
}

static void urlHost(const char *url, char *out, size_t osz)
{
    const char *sep = strstr(url, "://");
    const char *q = sep ? sep + 3 : url;
    size_t n = strcspn(q, "/?#");
    if (n >= osz) n = osz ? osz - 1 : 0;
    if (n) memcpy(out, q, n);
    if (osz) out[n] = '\0';
}

#define BROWSER_COOKIES "browser_cookies.txt"

/* Fetch a URL, optionally posting a form body. `referer` (may be NULL) is the
 * page the request came from. Cookies live in a file so logins and sessions
 * survive between page loads. */
static int browserFetchEx(BrowserPage *pg, const char *url, const char *post,
                          const char *referer, char *err, size_t errsz)
{
    CURL *c;
    CURLcode ce;
    BBuf raw;
    long status = 0;
    const char *info = NULL;
    char ctype[64] = "", eff[MS_PATH_MAX] = "";
    char errbuf[CURL_ERROR_SIZE];

    memset(pg, 0, sizeof *pg);
    errbuf[0] = '\0';
    c = curl_easy_init();
    if (!c) {
        snprintf(err, errsz, "networking is unavailable");
        return -1;
    }
    bbInit(&raw);

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, BROWSER_UA);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, BROWSER_COOKIES);
    curl_easy_setopt(c, CURLOPT_COOKIEJAR, BROWSER_COOKIES);
    if (referer && referer[0]) curl_easy_setopt(c, CURLOPT_REFERER, referer);
    if (post) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(post));
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, browserCurlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &raw);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    ce = curl_easy_perform(c);
    if (ce == CURLE_WRITE_ERROR && raw.n >= MAX_VIEW_BYTES) ce = CURLE_OK;
    if (ce != CURLE_OK) {
        snprintf(err, errsz, "%s", errbuf[0] ? errbuf : curl_easy_strerror(ce));
        curl_easy_cleanup(c);
        bbFree(&raw);
        return -1;
    }
    /* These strings belong to the easy handle, so copy them before cleanup. */
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &info) == CURLE_OK && info)
        safeCopy(ctype, sizeof ctype, info);
    info = NULL;
    if (curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &info) == CURLE_OK && info)
        safeCopy(eff, sizeof eff, info);
    curl_easy_cleanup(c);

    pg->status = status;
    safeCopy(pg->type, sizeof pg->type, ctype[0] ? ctype : "text/html");
    safeCopy(pg->url, sizeof pg->url, eff[0] ? eff : url);
    urlHost(pg->url, pg->host, sizeof pg->host);

    if (looksBinary(raw.p, raw.n) || ciPrefix(pg->type, "image/") ||
        ciPrefix(pg->type, "application/octet-stream") ||
        ciPrefix(pg->type, "application/pdf") || ciPrefix(pg->type, "audio/") ||
        ciPrefix(pg->type, "video/")) {
        BBuf b;
        char sz[32];
        bbInit(&b);
        bbChar(&b, '[');
        bbPuts(&b, pg->type);
        bbPuts(&b, "]  ");
        fmtSize(sz, sizeof sz, (long long)raw.n);
        bbPuts(&b, sz);
        bbPuts(&b, "\n\nThis is not a text document, so there is nothing to read here.");
        cleanText(&b);
        pg->text = b.p;
        if (!pg->text) pg->text = msDup("\n");
        safeCopy(pg->title, sizeof pg->title, pg->host);
    } else if (looksLikeHtml(pg->type, raw.p, raw.n)) {
        htmlTitle(raw.p ? raw.p : "", raw.n, pg->title, sizeof pg->title);
        if (!pg->title[0]) safeCopy(pg->title, sizeof pg->title, pg->host);
        renderHtml(pg, raw.p ? raw.p : "", raw.n);
        if (pg->fieldCount > 0) {
            pg->html = (char *)malloc(raw.n + 1);
            if (pg->html) {
                memcpy(pg->html, raw.p ? raw.p : "", raw.n);
                pg->html[raw.n] = '\0';
                pg->htmlLen = raw.n;
            }
        }
    } else {
        safeCopy(pg->title, sizeof pg->title, pg->host);
        renderPlainText(pg, raw.p ? raw.p : "", raw.n);
    }
    bbFree(&raw);
    return 0;
}

static int browserFetch(BrowserPage *pg, const char *url, char *err, size_t errsz)
{
    return browserFetchEx(pg, url, NULL, NULL, err, errsz);
}

/* ---- session navigation ---- */

typedef struct { char **items; int n, cap; } UrlStack;

static void urlPush(UrlStack *s, const char *url)
{
    char *copy;
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        char **ni = (char **)realloc(s->items, (size_t)nc * sizeof *ni);
        if (!ni) return;
        s->items = ni;
        s->cap = nc;
    }
    copy = msDup(url);
    if (!copy) return;
    s->items[s->n++] = copy;
}

static int urlPop(UrlStack *s, char *out, size_t osz)
{
    if (s->n <= 0) return 0;
    s->n--;
    safeCopy(out, osz, s->items[s->n]);
    free(s->items[s->n]);
    s->items[s->n] = NULL;
    return 1;
}

static void urlClear(UrlStack *s)
{
    while (s->n > 0) free(s->items[--s->n]);
    free(s->items);
    s->items = NULL;
    s->cap = 0;
}

static void browserHistoryAdd(const char *url)
{
    FILE *f = fopen(BROWSER_HISTORY, "ab");
    if (!f) return;
    fprintf(f, "%s\n", url);
    fclose(f);
}

/* Redraw the page after a control changed so the new value is on screen. The
 * markup is parsed again and the control states are carried over. */
static void pageRefreshFields(BrowserPage *pg)
{
    BrowserField *save;
    int i, fc = pg->fieldCount;

    if (!pg->html) return;
    save = (BrowserField *)malloc(sizeof(BrowserField) * (size_t)(fc ? fc : 1));
    if (!save) return;
    if (fc) memcpy(save, pg->fields, sizeof(BrowserField) * (size_t)fc);

    free(pg->text);
    pg->text = NULL;
    for (i = 0; i < pg->linkCount; i++) free(pg->links[i]);
    free(pg->links);
    pg->links = NULL;
    pg->linkCount = pg->linkCap = 0;
    pg->fieldCount = 0;
    pg->formCount = 0;

    renderHtmlRestore(pg, pg->html, pg->htmlLen, save, fc);
    for (i = 0; i < pg->fieldCount; i++) {
        int j;
        if (strcmp(pg->fields[i].type, "radio") || !pg->fields[i].checked) continue;
        for (j = i + 1; j < pg->fieldCount; j++)
            if (pg->fields[j].form == pg->fields[i].form &&
                !strcmp(pg->fields[j].type, "radio") &&
                !strcmp(pg->fields[j].name, pg->fields[i].name))
                pg->fields[j].checked = 0;
    }
    free(save);
}

static int browserOpenEx(BrowserPage *pg, UrlStack *back, UrlStack *fwd,
                         const char *url, const char *post, const char *referer,
                         int record)
{
    BrowserPage np;
    char err[200];

    memset(&np, 0, sizeof np);
    if (browserFetchEx(&np, url, post, referer, err, sizeof err) != 0) {
        safeCopy(browserError, sizeof browserError, err);
        return 0;
    }
    if (record) {
        if (pg->url[0]) urlPush(back, pg->url);
        urlClear(fwd);
        browserHistoryAdd(np.url);
    }
    pageFree(pg);
    *pg = np;
    return 1;
}

static int browserOpen(BrowserPage *pg, UrlStack *back, UrlStack *fwd,
                       const char *url, int record)
{
    return browserOpenEx(pg, back, fwd, url, NULL, NULL, record);
}

/* ---- form submission ---- */

static void bbUrlEncoded(BBuf *b, CURL *c, const char *s)
{
    char *esc = curl_easy_escape(c, s, 0);
    if (esc) { bbPuts(b, esc); curl_free(esc); }
}

/* Submit form `formIdx`. `submitNo` is the button that was pressed (1-based
 * field number, 0 for none). Returns 1 when a new page was loaded. */
static int browserFormSubmit(BrowserPage *pg, UrlStack *back, UrlStack *fwd,
                             int formIdx, int submitNo)
{
    BrowserForm *form;
    BBuf body;
    CURL *c;
    int i, first = 1, ok;

    if (formIdx < 0 || formIdx >= pg->formCount) return 0;
    form = &pg->forms[formIdx];
    c = curl_easy_init();
    if (!c) return 0;
    bbInit(&body);

    for (i = 0; i < pg->fieldCount; i++) {
        BrowserField *f = &pg->fields[i];
        const char *val;
        if (f->form != formIdx || !f->name[0]) continue;
        if (!strcmp(f->type, "submit") || !strcmp(f->type, "button") ||
            !strcmp(f->type, "reset") || !strcmp(f->type, "image")) {
            if (i + 1 != submitNo) continue;
        }
        if ((!strcmp(f->type, "checkbox") || !strcmp(f->type, "radio")) && !f->checked)
            continue;
        if (!strcmp(f->type, "select"))
            val = f->optionIndex < f->optionCount ? f->optionValue[f->optionIndex] : f->value;
        else
            val = f->value;
        if (!first) bbChar(&body, '&');
        first = 0;
        bbUrlEncoded(&body, c, f->name);
        bbChar(&body, '=');
        bbUrlEncoded(&body, c, val ? val : "");
    }
    curl_easy_cleanup(c);

    if (!strcmp(form->method, "POST")) {
        ok = browserOpenEx(pg, back, fwd, form->action, body.p ? body.p : "",
                           pg->url, 1);
        bbFree(&body);
        return ok;
    }
    if (!body.p || body.n == 0) {
        bbFree(&body);
        return browserOpen(pg, back, fwd, form->action, 1);
    }
    {
        size_t need = strlen(form->action) + body.n + 2;
        char *url = (char *)malloc(need);
        if (!url) { bbFree(&body); return 0; }
        snprintf(url, need, "%s%s%s", form->action,
                 strchr(form->action, '?') ? "&" : "?", body.p);
        ok = browserOpen(pg, back, fwd, url, 1);
        free(url);
        bbFree(&body);
        return ok;
    }
}

static void browserFormReset(BrowserPage *pg, int formIdx)
{
    int i;
    for (i = 0; i < pg->fieldCount; i++) {
        BrowserField *f = &pg->fields[i];
        if (f->form != formIdx) continue;
        if (fieldIsText(f)) f->value[0] = '\0';
        f->checked = 0;
        f->optionIndex = 0;
    }
}

static int browserBack(BrowserPage *pg, UrlStack *back, UrlStack *fwd)
{
    char url[MS_PATH_MAX], err[200];
    BrowserPage np;

    if (!urlPop(back, url, sizeof url)) {
        safeCopy(browserError, sizeof browserError, "No earlier page in this session.");
        return 0;
    }
    memset(&np, 0, sizeof np);
    if (browserFetch(&np, url, err, sizeof err) != 0) {
        urlPush(back, url);
        safeCopy(browserError, sizeof browserError, err);
        return 0;
    }
    urlPush(fwd, pg->url);
    pageFree(pg);
    *pg = np;
    return 1;
}

static int browserForward(BrowserPage *pg, UrlStack *back, UrlStack *fwd)
{
    char url[MS_PATH_MAX], err[200];
    BrowserPage np;

    if (!urlPop(fwd, url, sizeof url)) {
        safeCopy(browserError, sizeof browserError, "No later page in this session.");
        return 0;
    }
    memset(&np, 0, sizeof np);
    if (browserFetch(&np, url, err, sizeof err) != 0) {
        urlPush(fwd, url);
        safeCopy(browserError, sizeof browserError, err);
        return 0;
    }
    urlPush(back, pg->url);
    pageFree(pg);
    *pg = np;
    return 1;
}

static int browserReload(BrowserPage *pg)
{
    BrowserPage np;
    char err[200];

    memset(&np, 0, sizeof np);
    if (browserFetch(&np, pg->url, err, sizeof err) != 0) {
        safeCopy(browserError, sizeof browserError, err);
        return 0;
    }
    pageFree(pg);
    *pg = np;
    return 1;
}

/* ---- screen drawing ---- */

static void browserSay(Style st, const char *fmt, ...) MS_FMT(2, 3);
static void browserSay(Style st, const char *fmt, ...)
{
    va_list ap;
    printf("  %s", sty(st));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("%s\n", sty(ST_RESET));
}

static int textLineCount(const char *t)
{
    int n = 0;
    if (!t) return 0;
    while (*t) {
        if (*t == '\n') n++;
        t++;
    }
    return n;
}

static const char *textLine(const char *t, int idx)
{
    while (idx-- > 0 && t) {
        const char *nl = strchr(t, '\n');
        t = nl ? nl + 1 : NULL;
    }
    return t;
}

static void browserDraw(const BrowserPage *pg, int top)
{
    char title[120], url[MS_PATH_MAX], type[24];
    int total = textLineCount(pg->text);
    int pages = total ? (total + BROWSER_LINES - 1) / BROWSER_LINES : 1;
    int page = total ? (top / BROWSER_LINES) + 1 : 1;
    int i;

    truncCols(title, sizeof title, pg->title[0] ? pg->title : "(untitled)", 70);
    truncCols(url, sizeof url, pg->url, 72);
    truncCols(type, sizeof type, pg->type, 20);

    printf("\n  %s%s%s\n", sty(ST_BOLD), title, sty(ST_RESET));
    printf("  %s%s%s\n", sty(ST_DIM), url, sty(ST_RESET));
    printf("%s", sty(ST_DIM));
    repeat("-", 62);
    printf("%s\n", sty(ST_RESET));

    if (total == 0) {
        printf("  %s(no readable text on this page)%s\n", sty(ST_DIM), sty(ST_RESET));
    }
    for (i = 0; i < BROWSER_LINES && top + i < total; i++) {
        const char *ln = textLine(pg->text, top + i);
        const char *nl;
        int len;
        if (!ln) break;
        nl = strchr(ln, '\n');
        len = nl ? (int)(nl - ln) : (int)strlen(ln);
        printf("  %.*s\n", len, ln);
    }

    printf("%s", sty(ST_DIM));
    repeat("-", 40);
    printf("%s  %sHTTP %ld%s  %s%s%s  %d/%d\n",
           sty(ST_RESET),
           pg->status >= 400 ? sty(ST_BAD) : sty(ST_DIM), pg->status, sty(ST_RESET),
           sty(ST_DIM), type, sty(ST_RESET),
           page, pages);
    printf("  %s# link  n/p page  u url  s find  l links  b/f back/fwd  r reload  m marks  h hist  q quit%s\n",
           sty(ST_DIM), sty(ST_RESET));
    if (pg->fieldCount)
        printf("  %s{N} field  e <n> fill  t <n> toggle  x <n> press%s\n",
               sty(ST_DIM), sty(ST_RESET));
}

static void browserShowLinks(const BrowserPage *pg)
{
    int i, cap;
    appHeader("Page Links");
    if (pg->linkCount == 0) {
        printf("\n  This page has no links.\n");
        return;
    }
    cap = pg->linkCount < 100 ? pg->linkCount : 100;
    printf("\n");
    for (i = 0; i < cap; i++) {
        char u[120];
        truncCols(u, sizeof u, pg->links[i], 96);
        printf("  %s%3d%s  %s\n", sty(ST_ACCENT), i + 1, sty(ST_RESET), u);
    }
    if (pg->linkCount > cap) printf("\n  ... %d more not shown.\n", pg->linkCount - cap);
}

/* Handle "e <n> [text]", "t <n>" and "x <n>" for form controls. Returns 0 when
 * the command is not one of these, 1 when the page stayed the same, 2 when a
 * new page was loaded. */
static int browserFieldCommand(BrowserPage *pg, UrlStack *back, UrlStack *fwd,
                               const char *cmd)
{
    char work[MS_PATH_MAX];
    char verb;
    char *rest;
    char *endp = NULL;
    long no;
    BrowserField *f;

    safeCopy(work, sizeof work, cmd);
    verb = (char)tolower((unsigned char)work[0]);
    rest = work + 1;
    if ((verb != 'e' && verb != 't' && verb != 'x') ||
        (*rest && !isspace((unsigned char)*rest)))
        return 0;
    while (*rest && isspace((unsigned char)*rest)) rest++;
    if (!*rest) {
        int i;
        if (!pg->fieldCount) {
            browserSay(ST_WARN, "This page has no form fields.");
            return 1;
        }
        browserSay(ST_DIM, "e <n> type in a field, t <n> toggle, x <n> press a button.");
        for (i = 0; i < pg->fieldCount; i++)
            if (strcmp(pg->fields[i].type, "hidden"))
                browserSay(ST_RESET, "  {%d} %s (%s)", i + 1, pg->fields[i].label,
                           pg->fields[i].type);
        return 1;
    }
    no = strtol(rest, &endp, 10);
    f = pageField(pg, (int)no);
    if (!f) {
        browserSay(ST_BAD, "This page has no field {%ld}.", no);
        return 1;
    }

    if (verb == 't') {
        if (!strcmp(f->type, "select")) {
            if (f->optionCount > 0) f->optionIndex = (f->optionIndex + 1) % f->optionCount;
        } else if (!strcmp(f->type, "checkbox")) {
            f->checked = !f->checked;
        } else if (!strcmp(f->type, "radio")) {
            pageRadioClear(pg, f->form, f->name);
            f->checked = 1;
        } else {
            browserSay(ST_WARN, "{%ld} is not a checkbox, radio button or menu.", no);
        }
        return 1;
    }

    if (verb == 'e') {
        char *value = trim(endp);
        char entered[400];
        if (!fieldIsText(f)) {
            browserSay(ST_WARN, "{%ld} is a %s control; press x to use it.", no, f->type);
            return 1;
        }
        if (!*value) {
            char prompt[200];
            InStatus st;
            snprintf(prompt, sizeof prompt, "  %s > ", f->label);
            st = readLine(prompt, entered, sizeof entered);
            if (st != IN_OK) return 1;
            value = trim(entered);
        }
        safeCopy(f->value, sizeof f->value, value);
        return 1;
    }

    /* x: press the control */
    if (!strcmp(f->type, "submit") || !strcmp(f->type, "button") ||
        !strcmp(f->type, "reset") || !strcmp(f->type, "image")) {
        if (f->form < 0) {
            browserSay(ST_WARN, "That button is not inside a form.");
            return 1;
        }
        if (!strcmp(f->type, "reset")) {
            browserFormReset(pg, f->form);
            browserSay(ST_GOOD, "Form cleared.");
            return 1;
        }
        if (browserFormSubmit(pg, back, fwd, f->form, (int)no)) return 2;
        browserSay(ST_BAD, "Cannot submit: %s", browserError);
        return 1;
    }
    if (!strcmp(f->type, "select")) {
        if (f->optionCount > 0) f->optionIndex = (f->optionIndex + 1) % f->optionCount;
        return 1;
    }
    if (!strcmp(f->type, "checkbox") || !strcmp(f->type, "radio")) {
        f->checked = !f->checked;
        return 1;
    }
    browserSay(ST_WARN, "{%ld} is a text field; use e %ld <value>.", no, no);
    return 1;
}

/* ---- bookmarks and history ---- */

static void freeLines(char **v, int n)
{
    int i;
    for (i = 0; i < n; i++) free(v[i]);
    free(v);
}

static void loadLines(const char *path, char ***out, int *count)
{
    FILE *f = fopen(path, "rb");
    char line[MS_PATH_MAX];
    char **v = NULL;
    int n = 0, cap = 0;

    *out = NULL;
    *count = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *t = trim(line);
        char *d;
        if (!*t) continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 32;
            char **nv = (char **)realloc(v, (size_t)nc * sizeof *nv);
            if (!nv) break;
            v = nv;
            cap = nc;
        }
        d = msDup(t);
        if (!d) break;
        v[n++] = d;
    }
    fclose(f);
    *out = v;
    *count = n;
}

static void loadBookmarks(char ***urls, char ***titles, int *count)
{
    FILE *f = fopen(BROWSER_BOOKMARKS, "rb");
    char line[MS_PATH_MAX];
    char **u = NULL, **t = NULL;
    int n = 0, cap = 0;

    *urls = NULL;
    *titles = NULL;
    *count = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *v = trim(line);
        char *tab, *url, *title;
        if (!*v) continue;
        tab = strchr(v, '\t');
        if (tab) {
            *tab = '\0';
            url = trim(v);
            title = trim(tab + 1);
        } else {
            url = v;
            title = v;
        }
        if (n == cap) {
            int nc = cap ? cap * 2 : 16;
            char **nu = (char **)realloc(u, (size_t)nc * sizeof *nu);
            char **nt;
            if (!nu) break;
            u = nu;
            nt = (char **)realloc(t, (size_t)nc * sizeof *nt);
            if (!nt) break;
            t = nt;
            cap = nc;
        }
        u[n] = msDup(url);
        t[n] = msDup(title);
        if (!u[n] || !t[n]) break;
        n++;
    }
    fclose(f);
    *urls = u;
    *titles = t;
    *count = n;
}

static void saveBookmarks(char **urls, char **titles, int n)
{
    FILE *f = fopen(BROWSER_BOOKMARKS, "wb");
    int i;
    if (!f) return;
    for (i = 0; i < n; i++)
        fprintf(f, "%s\t%s\n", urls[i], titles[i] ? titles[i] : "");
    fclose(f);
}

static int browserBookmarksMenu(const BrowserPage *pg, char *openUrl, size_t osz)
{
    char cmd[MS_PATH_MAX];

    for (;;) {
        char **urls = NULL, **titles = NULL;
        int n = 0, i;

        loadBookmarks(&urls, &titles, &n);
        appHeader("Bookmarks");
        if (n == 0) printf("\n  No bookmarks yet - open a page and press a here.\n");
        else {
            printf("\n");
            for (i = 0; i < n; i++) {
                char t[70], u[80];
                truncCols(t, sizeof t, titles[i][0] ? titles[i] : urls[i], 56);
                truncCols(u, sizeof u, urls[i], 72);
                printf("  %s%3d%s  %s\n        %s%s%s\n", sty(ST_ACCENT), i + 1, sty(ST_RESET),
                       t, sty(ST_DIM), u, sty(ST_RESET));
            }
        }
        {
            char cur[90];
            truncCols(cur, sizeof cur, pg->title[0] ? pg->title : pg->url, 60);
            printf("\n  a add \"%s\"   d <number> remove   number open   q back\n", cur);
        }
        if (readLine("  > ", cmd, sizeof cmd) != IN_OK) {
            freeLines(urls, n);
            freeLines(titles, n);
            return 0;
        }
        {
            char *v = trim(cmd);
            if (!*v || ciEqual(v, "q")) {
                freeLines(urls, n);
                freeLines(titles, n);
                return 0;
            }
            if (ciEqual(v, "a")) {
                FILE *f = fopen(BROWSER_BOOKMARKS, "ab");
                if (!f) browserSay(ST_BAD, "Cannot write bookmarks: %s", strerror(errno));
                else {
                    fprintf(f, "%s\t%s\n", pg->url, pg->title);
                    fclose(f);
                    browserSay(ST_GOOD, "Bookmark saved.");
                }
            } else if (v[0] == 'd' && (v[1] == '\0' || isspace((unsigned char)v[1]))) {
                long k = strtol(trim(v + 1), NULL, 10);
                if (k >= 1 && k <= n) {
                    free(urls[k - 1]);
                    free(titles[k - 1]);
                    memmove(urls + k - 1, urls + k, (size_t)(n - k) * sizeof *urls);
                    memmove(titles + k - 1, titles + k, (size_t)(n - k) * sizeof *titles);
                    n--;
                    saveBookmarks(urls, titles, n);
                    browserSay(ST_GOOD, "Bookmark removed.");
                } else {
                    browserSay(ST_BAD, "There is no bookmark %ld.", k);
                }
            } else {
                char *endp = NULL;
                long k = strtol(v, &endp, 10);
                if (endp && *trim(endp)) {
                    browserSay(ST_BAD, "Enter a bookmark number, a, d <n> or q.");
                } else if (k >= 1 && k <= n) {
                    safeCopy(openUrl, osz, urls[k - 1]);
                    freeLines(urls, n);
                    freeLines(titles, n);
                    return 1;
                } else {
                    browserSay(ST_BAD, "There is no bookmark %ld.", k);
                }
            }
        }
        freeLines(urls, n);
        freeLines(titles, n);
    }
}

static int browserHistoryMenu(char *openUrl, size_t osz)
{
    char cmd[64];

    for (;;) {
        char **lines = NULL;
        int n = 0, i, shown = 0;

        loadLines(BROWSER_HISTORY, &lines, &n);
        appHeader("History");
        if (n == 0) printf("\n  No pages visited yet.\n");
        else {
            shown = n < 30 ? n : 30;
            printf("\n");
            for (i = 0; i < shown; i++) {
                char u[100];
                truncCols(u, sizeof u, lines[n - 1 - i], 92);
                printf("  %s%3d%s  %s\n", sty(ST_ACCENT), i + 1, sty(ST_RESET), u);
            }
            if (n > shown) printf("\n  (showing the %d most recent of %d)\n", shown, n);
        }
        if (n < shown) shown = n;
        printf("\n  number open   c clear history   q back\n");
        if (readLine("  > ", cmd, sizeof cmd) != IN_OK) {
            freeLines(lines, n);
            return 0;
        }
        {
            char *v = trim(cmd);
            if (!*v || ciEqual(v, "q")) {
                freeLines(lines, n);
                return 0;
            }
            if (ciEqual(v, "c")) {
                if (confirm("Clear the whole browsing history?")) {
                    remove(BROWSER_HISTORY);
                    browserSay(ST_GOOD, "History cleared.");
                }
            } else {
                char *endp = NULL;
                long k = strtol(v, &endp, 10);
                if (endp && *trim(endp)) {
                    browserSay(ST_BAD, "Enter a history number, c or q.");
                } else if (k >= 1 && k <= shown) {
                    safeCopy(openUrl, osz, lines[n - (int)k]);
                    freeLines(lines, n);
                    return 1;
                } else {
                    browserSay(ST_BAD, "There is no history entry %ld.", k);
                }
            }
        }
        freeLines(lines, n);
    }
}

/* ---- the application ---- */

#endif /* HAVE_CURL */

static void browserApp(void)
{
#ifdef HAVE_CURL
    BrowserPage page;
    UrlStack back, fwd;
    char line[MS_PATH_MAX], target[MS_PATH_MAX], argbuf[MS_PATH_MAX];
    int havePage = 0, top = 0;

    memset(&page, 0, sizeof page);
    memset(&back, 0, sizeof back);
    memset(&fwd, 0, sizeof fwd);

    for (;;) {
        InStatus st;
        char *cmd;

        if (!havePage) {
            appHeader("MetaDOS Browser");
            printf("\n  Type an address or search terms, then press ENTER.\n");
            printf("  Links are numbered: type a number to follow one.  q returns.\n");
            printf("  Forms work too: e <n> fills a field, t <n> toggles, x <n> presses.\n");
            st = readLine("\n  Address > ", line, sizeof line);
            if (st == IN_EOF) break;
            if (st != IN_OK) {
                browserSay(ST_BAD, "Input rejected: unsupported characters or too long.");
                appBackPrompt();
                continue;
            }
            cmd = trim(line);
            if (!*cmd || ciEqual(cmd, "q")) break;
            browserNormalizeInput(cmd, target, sizeof target);
            if (!browserOpen(&page, &back, &fwd, target, 1)) {
                printf("\n  %sCannot load: %s%s\n", sty(ST_BAD), browserError, sty(ST_RESET));
                appBackPrompt();
                continue;
            }
            havePage = 1;
            top = 0;
            continue;
        }

        appHeader("MetaDOS Browser");
        browserDraw(&page, top);
        st = readLine("  > ", line, sizeof line);
        if (st == IN_EOF) break;
        if (st != IN_OK) {
            browserSay(ST_BAD, "Input rejected: unsupported characters or too long.");
            appBackPrompt();
            continue;
        }
        cmd = trim(line);

        if (!*cmd || ciEqual(cmd, "n") || ciEqual(cmd, "next")) {
            int total = textLineCount(page.text);
            if (top + BROWSER_LINES < total) top += BROWSER_LINES;
            else browserSay(ST_WARN, "This is the end of the page.");
            continue;
        }
        if (ciEqual(cmd, "p") || ciEqual(cmd, "prev")) {
            top -= BROWSER_LINES;
            if (top < 0) top = 0;
            continue;
        }
        if (ciEqual(cmd, "l") || ciEqual(cmd, "links")) {
            browserShowLinks(&page);
            appBackPrompt();
            continue;
        }
        if (ciEqual(cmd, "b") || ciEqual(cmd, "back")) {
            if (browserBack(&page, &back, &fwd)) top = 0;
            else {
                browserSay(ST_WARN, "%s", browserError);
                appBackPrompt();
            }
            continue;
        }
        if (ciEqual(cmd, "f") || ciEqual(cmd, "fwd") || ciEqual(cmd, "forward")) {
            if (browserForward(&page, &back, &fwd)) top = 0;
            else {
                browserSay(ST_WARN, "%s", browserError);
                appBackPrompt();
            }
            continue;
        }
        if (ciEqual(cmd, "r") || ciEqual(cmd, "reload")) {
            if (browserReload(&page)) top = 0;
            else {
                browserSay(ST_BAD, "Cannot reload: %s", browserError);
                appBackPrompt();
            }
            continue;
        }
        if (ciEqual(cmd, "m") || ciEqual(cmd, "marks") || ciEqual(cmd, "bookmarks")) {
            if (browserBookmarksMenu(&page, target, sizeof target)) {
                if (browserOpen(&page, &back, &fwd, target, 1)) top = 0;
                else {
                    browserSay(ST_BAD, "Cannot load: %s", browserError);
                    appBackPrompt();
                }
            }
            continue;
        }
        if (ciEqual(cmd, "h") || ciEqual(cmd, "hist") || ciEqual(cmd, "history")) {
            if (browserHistoryMenu(target, sizeof target)) {
                if (browserOpen(&page, &back, &fwd, target, 1)) top = 0;
                else {
                    browserSay(ST_BAD, "Cannot load: %s", browserError);
                    appBackPrompt();
                }
            }
            continue;
        }
        if (ciEqual(cmd, "q") || ciEqual(cmd, "quit") || ciEqual(cmd, "exit")) break;

        if (ciEqual(cmd, "u") || ciEqual(cmd, "url") || ciPrefix(cmd, "u ") ||
            ciPrefix(cmd, "open ")) {
            char *arg;
            if (ciEqual(cmd, "u") || ciEqual(cmd, "url")) {
                st = readLine("  Address > ", argbuf, sizeof argbuf);
                if (st == IN_EOF) break;
                if (st != IN_OK) {
                    browserSay(ST_BAD, "Input rejected: unsupported characters or too long.");
                    appBackPrompt();
                    continue;
                }
                arg = trim(argbuf);
            } else {
                arg = trim(cmd + (cmd[0] == 'u' ? 1 : 4));
            }
            if (!*arg) continue;
            browserNormalizeInput(arg, target, sizeof target);
            if (browserOpen(&page, &back, &fwd, target, 1)) top = 0;
            else {
                browserSay(ST_BAD, "Cannot load: %s", browserError);
                appBackPrompt();
            }
            continue;
        }
        if (ciEqual(cmd, "s") || ciEqual(cmd, "find") || ciEqual(cmd, "search") ||
            ciPrefix(cmd, "s ") || ciPrefix(cmd, "find ") || ciPrefix(cmd, "search ")) {
            char *arg = trim(cmd + strcspn(cmd, " \t"));
            if (!*arg) {
                st = readLine("  Search > ", argbuf, sizeof argbuf);
                if (st == IN_EOF) break;
                if (st != IN_OK) {
                    browserSay(ST_BAD, "Input rejected: unsupported characters or too long.");
                    appBackPrompt();
                    continue;
                }
                arg = trim(argbuf);
            }
            if (!*arg) continue;
            browserSearch(arg, target, sizeof target);
            if (browserOpen(&page, &back, &fwd, target, 1)) top = 0;
            else {
                browserSay(ST_BAD, "Cannot search: %s", browserError);
                appBackPrompt();
            }
            continue;
        }
        if (isdigit((unsigned char)cmd[0])) {
            char *endp = NULL;
            long n = strtol(cmd, &endp, 10);
            if (endp && !*trim(endp)) {
                if (n >= 1 && n <= page.linkCount) {
                    safeCopy(target, sizeof target, page.links[n - 1]);
                    if (browserOpen(&page, &back, &fwd, target, 1)) top = 0;
                    else {
                        browserSay(ST_BAD, "Cannot load: %s", browserError);
                        appBackPrompt();
                    }
                } else {
                    browserSay(ST_BAD, "This page has links 1-%d, not %ld.", page.linkCount, n);
                    appBackPrompt();
                }
                continue;
            }
        }
        {
            int res = browserFieldCommand(&page, &back, &fwd, cmd);
            if (res == 2) { top = 0; continue; }
            if (res == 1) {
                pageRefreshFields(&page);
                continue;
            }
        }

        /* Anything else is an address or a search. */
        browserNormalizeInput(cmd, target, sizeof target);
        if (browserOpen(&page, &back, &fwd, target, 1)) top = 0;
        else {
            browserSay(ST_BAD, "Cannot load: %s", browserError);
            appBackPrompt();
        }
    }

    pageFree(&page);
    urlClear(&back);
    urlClear(&fwd);
#else
    appHeader("MetaDOS Browser");
    printf("\n  Browser networking is not compiled in. Build with libcurl:\n"
           "  gcc -std=c11 -DHAVE_CURL -o metashell main_system.c $(pkg-config --cflags --libs libcurl)\n\n"
           "  The browser downloads real HTTP/HTTPS pages, renders them as readable text\n"
           "  with numbered links, and keeps back/forward, reload, search, bookmarks and history.\n");
    appBackPrompt();
#endif
}

static void systemAppsHub(void)
{
    const char *names[]={"System Monitor","Task Manager","Text Editor Pro","Calendar","Notes","Disk Manager","System Information","Network Manager","App Installer","Software Centre","Backup Manager","System Log Viewer","User Manager","Help Centre","Notification Centre","MetaDOS Browser"}; int choice; InStatus st;
    for(;;){int i;appHeader("System Apps");printf("\n");for(i=0;i<16;i++)printf("  %2d  %s\n",i+1,names[i]);printf("  0   Back\n\n");st=readChoice("  Select App > ",0,16,&choice);if(st==IN_EOF||choice==0)return;if(st!=IN_OK)continue;switch(choice){case 1:systemMonitorApp();break;case 2:taskManagerApp();break;case 3:textEditorProApp();break;case 4:calendarApp();break;case 5:notesApp();break;case 6:diskManagerApp();break;case 7:systemInformationApp();break;case 8:networkManagerApp();break;case 9:appInstallerApp();break;case 10:softwareCentreApp();break;case 11:backupManagerApp();break;case 12:systemLogViewerApp();break;case 13:userManagerApp();break;case 14:helpCentreApp();break;case 15:notificationCentreApp();break;case 16:browserApp();break;}}
}

/* ================= DESKTOP ================= */

static void drawDesktop(void)
{
    static const char *const left[5]  = { "File Manager", "Calculator", "Clock", "Random Number", "Todo List" };
    static const char *const right[5] = { "Password Generator", "Quote Generator", "Settings", "Terminal", "System Apps" };
    char l[160], r[160], line[400], when[64], ver[32];
    int i;
    fmtTime(when, sizeof when, "%a %d %b %Y  %H:%M", time(NULL));
    clear();
    boxTop();
    snprintf(l, sizeof l, "%s%s %s%s%s", sty(ST_ACCENT), gl->diamond, sty(ST_BOLD), MS_NAME, sty(ST_RESET));
    snprintf(ver, sizeof ver, "%sv%s%s", sty(ST_DIM), MS_VERSION, sty(ST_RESET));
    boxRow2(l, ver);
    boxMid();
    snprintf(l, sizeof l, "%s%s%s %s", sty(ST_GOOD), gl->dot, sty(ST_RESET), os.user);
    snprintf(r, sizeof r, "%s%s%s", sty(ST_DIM), when, sty(ST_RESET));
    boxRow2(l, r);
    boxMid();
    snprintf(line, sizeof line, "%sAPPLICATIONS%s", sty(ST_DIM), sty(ST_RESET));
    boxRow(line);
    for (i = 0; i < 5; i++) {
        menuItem(l, sizeof l, i + 1, left[i], 22);
        if (i < 5) menuItem(r, sizeof r, i + 6, right[i], 22); else r[0] = '\0';
        snprintf(line, sizeof line, "%s    %s", l, r);
        boxRow(line);
    }
    boxRow("");
    snprintf(line, sizeof line, "%sSYSTEM%s", sty(ST_DIM), sty(ST_RESET));
    boxRow(line);
    {
        char a[96], b[96], c[96];
        menuItem(a, sizeof a, 11, "Log Off", 10);
        menuItem(b, sizeof b, 12, "Restart", 10);
        menuItem(c, sizeof c, 13, "Shutdown", 10);
        snprintf(line, sizeof line, "%s    %s    %s", a, b, c);
        boxRow(line);
    }
    boxBottom();
}

static void launching(const char *what)
{
    if (!os.tty) return;
    printf("  %sOpening %s...%s\n", sty(ST_DIM), what, sty(ST_RESET));
    delay(180);
}

static SysAction desktop(void)
{
    for (;;) {
        int choice = 0;
        InStatus st;
        SysAction a;
        if (os.eof) return SYS_SHUTDOWN;
        setTheme();
        drawDesktop();
        showNotice();
        st = readChoice("  Select App > ", 1, 13, &choice);
        if (st == IN_EOF) return SYS_SHUTDOWN;
        if (st != IN_OK) continue;
        switch (choice) {
            case 1:  launching("File Manager");       fileManager(); break;
            case 2:  launching("Calculator");         calculator(); break;
            case 3:  launching("Clock");              clockApp(); break;
            case 4:  launching("Random Number");      randomNumber(); break;
            case 5:  launching("Todo List");          todo(); break;
            case 6:  launching("Password Generator"); passwordGen(); break;
            case 7:  launching("Quote Generator");    quote(); break;
            case 8:  launching("Settings");           settings(); break;
            case 9:  launching("Terminal");
                     a = terminalApp();
                     if (a != SYS_NONE) return a;
                     break;
            case 10: launching("System Apps"); systemAppsHub(); break;
            case 11: logoff(); return SYS_LOGOUT;
            case 12: if (restart()) return SYS_RESTART; break;
            case 13: if (shutdownSystem()) return SYS_SHUTDOWN; break;
            default: break;
        }
    }
}

/* Login / create-account menu. Returns 1 when logged in, 0 to exit. */
static int loginScreen(void)
{
    for (;;) {
        char l[96], line[200];
        int choice = 0, r;
        InStatus st;
        if (os.eof) return 0;
        clear();
        printLogo();
        printf("\n");
        boxTop();
        snprintf(line, sizeof line, "%s%s %s%s%s", sty(ST_ACCENT), gl->diamond, sty(ST_BOLD), "Welcome to MetaShell OS", sty(ST_RESET));
        boxRow(line);
        boxMid();
        menuItem(l, sizeof l, 1, "Login", 30); boxRow(l);
        menuItem(l, sizeof l, 2, "Create Account", 30); boxRow(l);
        menuItem(l, sizeof l, 3, "Exit", 30); boxRow(l);
        boxBottom();
        showNotice();
        st = readChoice("  Select > ", 1, 3, &choice);
        if (st == IN_EOF) return 0;
        if (st != IN_OK) continue;
        if (choice == 1) { r = login(); if (r == 1) return 1; if (r < 0) return 0; }
        else if (choice == 2) createAccount();
        else return 0;
    }
}

/* ================= SELF TEST ================= */

static int selfTest(void)
{
    int fails = 0;
    unsigned char dk[32], d2[32];
    char hex[65], err[96];
    double r;
    Sha256 c;
#define CHECK(cond, what) do { int ok_ = (cond); printf("  [%s] %s\n", ok_ ? "PASS" : "FAIL", what); if (!ok_) fails++; } while (0)
    sha256Init(&c); sha256Update(&c, (const unsigned char *)"abc", 3); sha256Final(&c, dk); toHex(dk, 32, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256(\"abc\")");
    pbkdf2Sha256((const unsigned char *)"password", 8, (const unsigned char *)"salt", 4, 1, dk); toHex(dk, 32, hex);
    CHECK(!strcmp(hex, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"), "PBKDF2-HMAC-SHA256 c=1 (RFC 7914 vector)");
    pbkdf2Sha256((const unsigned char *)"password", 8, (const unsigned char *)"salt", 4, 4096, d2); toHex(d2, 32, hex);
    CHECK(!strcmp(hex, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"), "PBKDF2-HMAC-SHA256 c=4096");
    {
        char enc[200];
        CHECK(hashPassword("hunter2", enc, sizeof enc) == 0 && verifyHash("hunter2", enc) && !verifyHash("hunter3", enc), "hash/verify round trip");
    }
    CHECK(evalExpression("5 + 2", &r, err, sizeof err) == 0 && r == 7.0, "calc 5 + 2");
    CHECK(evalExpression("10 / 4", &r, err, sizeof err) == 0 && r == 2.5, "calc 10 / 4");
    CHECK(evalExpression("-5 + 10", &r, err, sizeof err) == 0 && r == 5.0, "calc -5 + 10");
    CHECK(evalExpression("2.5 * 4", &r, err, sizeof err) == 0 && r == 10.0, "calc 2.5 * 4");
    CHECK(evalExpression("5 - -2", &r, err, sizeof err) == 0 && r == 7.0, "calc 5 - -2");
    CHECK(evalExpression("5 / 0", &r, err, sizeof err) != 0, "calc rejects division by zero");
    CHECK(evalExpression("abc", &r, err, sizeof err) != 0, "calc rejects garbage");
    CHECK(evalExpression("5 +", &r, err, sizeof err) != 0, "calc rejects missing operand");
    CHECK(evalExpression("5 % 2", &r, err, sizeof err) != 0, "calc rejects unknown operator");
    CHECK(evalExpression("1 + 2 + 3", &r, err, sizeof err) != 0, "calc rejects trailing input");
    {
        char line[64] = "echo \"a b\" c 'd e'", *av[8];
        int ac = 0;
        CHECK(tokenize(line, av, 8, &ac, err, sizeof err) == 0 && ac == 4 && !strcmp(av[1], "a b") && !strcmp(av[3], "d e"), "tokenizer handles quotes");
        strcpy(line, "cd \"oops");
        CHECK(tokenize(line, av, 8, &ac, err, sizeof err) != 0, "tokenizer rejects unterminated quote");
    }
    {
        char why[64];
        CHECK(!validLeafName("a/b", why, sizeof why) && !validLeafName("", why, sizeof why) && validLeafName("notes.txt", why, sizeof why), "leaf-name validation");
        CHECK(!validUsernameStrict("ab", why, sizeof why) && validUsernameStrict("princewill", why, sizeof why), "username validation");
    }
#ifdef HAVE_CURL
    {
        BBuf b;
        char out[256];
        BrowserPage pg;
        const char *src = "a &amp; b &#65;&#x42; &nbsp;c";
        const char *html = "<html><head><title>Hi &amp; bye</title></head><body>"
                           "<h1>Title</h1><p>Read <a href=\"/x\">this</a>.</p>"
                           "<ul><li>one</li><li>two</li></ul></body></html>";
        bbInit(&b);
        decodeText(&b, src, strlen(src));
        CHECK(b.p && !strcmp(b.p, "a & b AB  c"), "browser decodes HTML entities");
        bbFree(&b);
        browserResolve("https://example.com/a/b.html", "../c.html", out, sizeof out);
        CHECK(!strcmp(out, "https://example.com/c.html"), "browser resolves ../ links");
        browserResolve("https://example.com/a/b.html", "/root.html", out, sizeof out);
        CHECK(!strcmp(out, "https://example.com/root.html"), "browser resolves absolute paths");
        browserResolve("http://example.com/x", "//cdn.test/y", out, sizeof out);
        CHECK(!strcmp(out, "http://cdn.test/y"), "browser resolves protocol-relative links");
        browserNormalizeInput("example.com", out, sizeof out);
        CHECK(!strcmp(out, "https://example.com"), "browser expands a bare domain");
        memset(&pg, 0, sizeof pg);
        safeCopy(pg.url, sizeof pg.url, "https://example.com/");
        renderHtml(&pg, html, strlen(html));
        htmlTitle(html, strlen(html), pg.title, sizeof pg.title);
        CHECK(pg.linkCount == 1 && !strcmp(pg.links[0], "https://example.com/x"), "browser collects link targets");
        CHECK(pg.text && strstr(pg.text, "this [1]") && strstr(pg.text, "one") && strstr(pg.text, "two"), "browser renders links and lists");
        CHECK(!strcmp(pg.title, "Hi & bye"), "browser reads the page title");
        pageFree(&pg);
    }
    {
        const char *form =
            "<form action=\"find\" method=\"post\">"
            "<input name=\"q\" placeholder=\"Search words\">"
            "<input type=\"checkbox\" name=\"all\" checked>"
            "<select name=\"n\"><option value=\"a\">A</option>"
            "<option value=\"b\" selected>B</option></select>"
            "<textarea name=\"t\">hello</textarea>"
            "<input type=\"submit\" value=\"Go\"></form>";
        BrowserPage fp;
        memset(&fp, 0, sizeof fp);
        safeCopy(fp.url, sizeof fp.url, "https://example.com/search?x=1");
        renderHtml(&fp, form, strlen(form));
        CHECK(fp.formCount == 1 && fp.fieldCount == 5, "browser collects form fields");
        CHECK(!strcmp(fp.forms[0].method, "POST") &&
              !strcmp(fp.forms[0].action, "https://example.com/find"),
              "browser resolves the form action");
        CHECK(fp.fields[1].checked && !strcmp(fp.fields[2].value, "b") &&
              !strcmp(fp.fields[3].value, "hello"),
              "browser reads control values");
        CHECK(fp.text && strstr(fp.text, "Search words") && strstr(fp.text, "[Go]"),
              "browser shows form controls");
        pageFree(&fp);
    }
#endif
#undef CHECK
    printf("%s\n", fails ? "SELF-TEST FAILED" : "All self-tests passed.");
    return fails ? 1 : 0;
}

/* ================= MAIN ================= */

static int initPaths(void)
{
    char cwd[MS_PATH_MAX];
    if (!MS_GETCWD(cwd, sizeof cwd)) return -1;
    safeCopy(os.home, sizeof os.home, cwd);
    if (!fits(snprintf(os.usersPath, sizeof os.usersPath, "%s/users.txt", cwd), sizeof os.usersPath) ||
        !fits(snprintf(os.cfgPath, sizeof os.cfgPath, "%s/metashell.cfg", cwd), sizeof os.cfgPath) ||
        !fits(snprintf(os.todoPath, sizeof os.todoPath, "%s/todo.txt", cwd), sizeof os.todoPath))
        return -1;
    if (strlen(cwd) > MS_PATH_MAX - 40) return -1;
    return 0;
}

static void resetSession(void)
{
    char err[64];
    safeCopy(os.user, sizeof os.user, "user");
    os.notice[0] = '\0';
    os.eof = os.eof;                               /* a closed stdin stays closed */
    if (MS_CHDIR(os.home) != 0) safeCopy(err, sizeof err, "");
}

int main(int argc, char **argv)
{
    int i, forceAscii = 0;
    SysAction act;

    memset(&os, 0, sizeof os);
    os.theme = DEFAULT_THEME;
    safeCopy(os.user, sizeof os.user, "user");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fast")) os.fast = 1;
        else if (!strcmp(argv[i], "--ascii")) forceAscii = 1;
        else if (!strcmp(argv[i], "--selftest")) { os.fast = 1; platformInit(1); return selfTest(); }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("%s %s\nUsage: %s [--fast] [--ascii] [--selftest]\n", MS_NAME, MS_VERSION, argv[0]);
            return 0;
        } else { fprintf(stderr, "Unknown option: %s (try --help)\n", argv[i]); return 2; }
    }
    platformInit(forceAscii);
    if (initPaths() != 0) { fprintf(stderr, "MetaShell: cannot determine a usable working directory.\n"); return 1; }

    for (;;) {                                       /* one iteration = one power cycle */
        configLoad();
        setTheme();
        boot();
        act = SYS_SHUTDOWN;
        for (;;) {                                   /* login <-> desktop session loop */
            if (!loginScreen()) { act = SYS_SHUTDOWN; break; }
            act = desktop();
            if (act == SYS_LOGOUT) { safeCopy(os.user, sizeof os.user, "user"); continue; }
            break;
        }
        if (act == SYS_RESTART) { resetSession(); continue; }
        break;
    }
    clear();
    printf("\n  %s%s MetaShell OS has shut down. Goodbye.%s\n\n", sty(ST_ACCENT), gl->diamond, sty(ST_RESET));
    return 0;
}
