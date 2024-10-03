#pragma once
#include "EfiGuardDxe.h"
#include "util.h"

// ALLOCATING MEMORY MANUALLY CAUSES CPU EXCEPTIONS...
// SO WE JUST USE gKernelPatchInfo INSTEAD...
// SO YEAH THIS THING IS KINDA HARDCODED DONT USE IT FOR
// ANYTHING ITS NOT ALREADY
EFI_STATUS
EFIAPI
HookFunction(
    IN VOID* Func,
    IN VOID* HookFunc,
    IN UINT64 PrologueLength,
    OUT VOID** OrigFunc
);