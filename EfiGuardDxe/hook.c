#include "hook.h"

// This represents a 64-bit absolute JMP.
// The 8 bytes after this represent the 64-bit address
// to jump to.
STATIC CONST UINT8 Detour[] = {
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00
};

EFI_STATUS
EFIAPI
HookFunction(
    IN VOID* Func,
    IN VOID* HookFunc,
    IN UINT64 PrologueLength,
    OUT VOID* OrigFunc
)
{
    if (!Func || !HookFunc || !OrigFunc)
        return EFI_INVALID_PARAMETER;

    if (PrologueLength < sizeof(Detour) + sizeof(VOID*))
        return EFI_INVALID_PARAMETER;

    // First make the _orig function.
    //
    // <prologue>
    // FF 25 00 00 00 00
    // XX XX XX XX XX XX XX XX
    UINT64 OrigJumpAddr = PrologueLength + (UINT64)Func;
    UINT8* Orig = OrigFunc;
    CopyWpMem(Orig, Func, PrologueLength);
    CopyWpMem(Orig + PrologueLength, Detour, sizeof(Detour));
    CopyWpMem(Orig + PrologueLength + sizeof(Detour), &OrigJumpAddr, sizeof(VOID*));

    // Now inject detour into the real function.
    //
    // FF 25 00 00 00 00
    // XX XX XX XX XX XX XX XX
    CopyWpMem(Func, Detour, sizeof(Detour));
    CopyWpMem((UINT8*)Func + sizeof(Detour), &HookFunc, sizeof(VOID*));
    return EFI_SUCCESS;
}