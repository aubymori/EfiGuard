#pragma once
#include "EfiGuardDxe.h"
#include "util.h"

/**
 * Hooks a function such that any calls to Func are redirected to HookFunc.
 * Also makes the original version callable through the code put out to OrigFunc.
 *
 * Arguments:
 * Func - Pointer to the original function to hook.
 * HookFunc - Pointer to the new function that should be called where the original is called.
 * PrologueLength - Length, in bytes, of the beginning of the function located at Func. Minimum 14 bytes.
 * OrigFunc - Pointer to the buffer where the redirect function to call the original should be stored.
 *   At least PrologueLength + 6 + sizeof(VOID*) bytes should be allocated here.
 */
EFI_STATUS
EFIAPI
HookFunction(
    IN VOID* Func,
    IN VOID* HookFunc,
    IN UINT64 PrologueLength,
    OUT VOID* OrigFunc
);