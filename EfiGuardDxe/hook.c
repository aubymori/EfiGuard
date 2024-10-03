#include "hook.h"

EFI_STATUS
EFIAPI
HookFunction(
    IN VOID* Func,
    IN VOID* HookFunc,
    IN UINT64 PrologueLength,
    OUT VOID** OrigFunc
)
{
    if (!Func || !HookFunc || !OrigFunc)
        return EFI_INVALID_PARAMETER;

    if (PrologueLength < MIN_PROLOGUE_LENGTH)
        return EFI_INVALID_PARAMETER;

    // First make the _orig function.
    // FF 25 00 00 00 00
    // XX XX XX XX XX XX XX XX
    UINT8* Orig = gKernelPatchInfo.FindBitmapResource_orig;//AllocatePool(PrologueLength + MIN_PROLOGUE_LENGTH);
    UINT64 OrigJumpAddr = PrologueLength + (UINT64)Func;
    CopyWpMem(Orig, Func, PrologueLength);
    CopyWpMem(Orig + PrologueLength, Detour, sizeof(Detour));
    CopyWpMem(Orig + PrologueLength + sizeof(Detour), &OrigJumpAddr, sizeof(VOID*));
    *OrigFunc = Orig;

    // Now inject detour into the real function.
    // FF 25 00 00 00 00
    // XX XX XX XX XX XX XX XX
    CopyWpMem(Func, Detour, sizeof(Detour));
    CopyWpMem((UINT8*)Func + sizeof(Detour), &HookFunc, sizeof(VOID*));
    return EFI_SUCCESS;
}