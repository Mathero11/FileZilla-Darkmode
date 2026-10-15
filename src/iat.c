#include "iat.h"

static BOOL patch_pointer(void **slot, void *value)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
        return FALSE;
    *slot = value;
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return TRUE;
}

static IMAGE_IMPORT_DESCRIPTOR *first_import(HMODULE module)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
        return NULL;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    return dir.VirtualAddress ? (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress) : NULL;
}

void dm_for_each_import(HMODULE module, dm_import_fn fn, void *ctx)
{
    BYTE *base = (BYTE *)module;
    IMAGE_IMPORT_DESCRIPTOR *imp = first_import(module);
    for (; imp && imp->Name; imp++)
        if (!fn((const char *)(base + imp->Name), ctx))
            return;
}

BOOL dm_iat_hook(HMODULE module, const char *dll, const char *func, void *hook, void **original)
{
    BYTE *base = (BYTE *)module;
    IMAGE_IMPORT_DESCRIPTOR *imp = first_import(module);
    if (!imp)
        return FALSE;

    /* Used when the import name table is missing (bound imports). */
    HMODULE target_dll = GetModuleHandleA(dll);
    void *target = target_dll ? (void *)GetProcAddress(target_dll, func) : NULL;

    for (; imp->Name; imp++) {
        if (lstrcmpiA((const char *)(base + imp->Name), dll) != 0)
            continue;

        IMAGE_THUNK_DATA *names = imp->OriginalFirstThunk
                                      ? (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk)
                                      : NULL;
        IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

        for (int i = 0; addrs[i].u1.Function; i++) {
            void **slot = (void **)&addrs[i].u1.Function;
            BOOL match;
            if (names) {
                if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
                    continue;
                IMAGE_IMPORT_BY_NAME *by_name = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
                match = lstrcmpA((const char *)by_name->Name, func) == 0;
            } else {
                match = target && *slot == target;
            }
            if (!match)
                continue;
            if (*slot == hook)
                return TRUE;
            if (original)
                *original = *slot;
            return patch_pointer(slot, hook);
        }
    }
    return FALSE;
}
