/*
 * version.dll proxy (64-bit).
 *
 * Every export is a tiny asm stub that jumps to the same function in the real
 * version.dll from the system directory. The real address is looked up on the
 * first call (not in DllMain); after that it is a direct jump. Because the
 * stub jumps instead of calls, the caller's arguments and stack stay intact.
 */
#include <windows.h>
#include "log.h"

#define VERSION_EXPORTS(X)               \
    X(0, GetFileVersionInfoA)            \
    X(1, GetFileVersionInfoByHandle)     \
    X(2, GetFileVersionInfoExA)          \
    X(3, GetFileVersionInfoExW)          \
    X(4, GetFileVersionInfoSizeA)        \
    X(5, GetFileVersionInfoSizeExA)      \
    X(6, GetFileVersionInfoSizeExW)      \
    X(7, GetFileVersionInfoSizeW)        \
    X(8, GetFileVersionInfoW)            \
    X(9, VerFindFileA)                   \
    X(10, VerFindFileW)                  \
    X(11, VerInstallFileA)               \
    X(12, VerInstallFileW)               \
    X(13, VerLanguageNameA)              \
    X(14, VerLanguageNameW)              \
    X(15, VerQueryValueA)                \
    X(16, VerQueryValueW)

#define EXPORT_NAME(idx, name) #name,
static const char *const g_export_names[] = { VERSION_EXPORTS(EXPORT_NAME) };
#define EXPORT_COUNT (sizeof(g_export_names) / sizeof(g_export_names[0]))

/* Read by the asm stubs, so not static. */
void *g_real[EXPORT_COUNT];

static HMODULE g_real_module;

static HMODULE load_real_version_dll(void)
{
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len + 13 >= MAX_PATH)
        return NULL;
    lstrcatW(path, L"\\version.dll");
    HMODULE mod = LoadLibraryW(path);
    dm_log(L"real version.dll: %ls -> %ls", path, mod ? L"loaded" : L"NOT FOUND");
    return mod;
}

/* Called by a stub on the first call of export 'idx'. */
void *dm_resolve_export(int idx)
{
    if (!g_real_module)
        g_real_module = load_real_version_dll();

    void *fn = g_real_module ? (void *)GetProcAddress(g_real_module, g_export_names[idx]) : NULL;
    if (!fn) {
        /* Without the real function the call cannot continue. */
        dm_log(L"ERROR: export %hs is missing from the real version.dll", g_export_names[idx]);
        MessageBoxW(NULL, L"Dark mode for FileZilla: the real version.dll could not be loaded.\n"
                          L"Remove version.dll from the FileZilla folder.",
                    L"Dark mode for FileZilla", MB_ICONERROR);
        ExitProcess(1);
    }
    g_real[idx] = fn;
    dm_log(L"forwarded: %hs", g_export_names[idx]);
    return fn;
}

/*
 * Windows x64 calling convention: the first four arguments are in rcx, rdx,
 * r8 and r9 (version.dll has no floating point arguments), the rest on the
 * stack above the return address. The slow path saves the four registers,
 * calls dm_resolve_export with 32 bytes of shadow space and a 16-byte aligned
 * stack (entry rsp = 8 mod 16, 4 pushes + 40 = 0 mod 16), restores them and
 * jumps, so the stack arguments are where the real function expects them.
 */
#define EXPORT_STUB(idx, name)                          \
    __asm__(".text\n"                                   \
            ".globl dm_stub_" #name "\n"                \
            "dm_stub_" #name ":\n"                      \
            "\tmovq g_real+" #idx "*8(%rip), %rax\n"    \
            "\ttestq %rax, %rax\n"                      \
            "\tjz 1f\n"                                 \
            "\tjmp *%rax\n"                             \
            "1:\tpushq %rcx\n"                          \
            "\tpushq %rdx\n"                            \
            "\tpushq %r8\n"                             \
            "\tpushq %r9\n"                             \
            "\tsubq $40, %rsp\n"                        \
            "\tmovl $" #idx ", %ecx\n"                  \
            "\tcall dm_resolve_export\n"                \
            "\taddq $40, %rsp\n"                        \
            "\tpopq %r9\n"                              \
            "\tpopq %r8\n"                              \
            "\tpopq %rdx\n"                             \
            "\tpopq %rcx\n"                             \
            "\tjmp *%rax\n");
VERSION_EXPORTS(EXPORT_STUB)
