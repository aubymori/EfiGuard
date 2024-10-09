#include <Uefi.h>
#include <Pi/PiDxeCis.h>

#include <IndustryStandard/LegacyVgaBios.h>
#include <Protocol/EfiGuard.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/LegacyBios.h>
#include <Protocol/LegacyRegion.h>
#include <Protocol/LegacyRegion2.h>
#include <Library/PcdLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MtrrLib.h>
#include <Library/ReportStatusCodeLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "Display.h"
#include "Utils.h"
#include "Int10hHandler.h"

#pragma pack(1)
typedef struct {
  UINT16  Offset;
  UINT16  Segment;
} IVT_ENTRY;
#pragma pack()

typedef enum {
  LOCK,
  UNLOCK
} MEMORY_LOCK_OPERATION;

STATIC CONST  CHAR8                 VENDOR_NAME[]       = "EfiGuard";
STATIC CONST  CHAR8                 PRODUCT_NAME[]      = "Emulated VGA";
STATIC CONST  CHAR8                 PRODUCT_REVISION[]  = "OVMF Int10h (fake)";
STATIC CONST  EFI_PHYSICAL_ADDRESS  VGA_ROM_ADDRESS     = 0xC0000;
STATIC CONST  EFI_PHYSICAL_ADDRESS  IVT_ADDRESS         = 0x00000;
STATIC CONST  UINTN                 VGA_ROM_SIZE        = 0x10000;
STATIC CONST  UINTN                 FIXED_MTRR_SIZE     = 0x20000;

DISPLAY_INFO                        mDisplayInfo;

/**
  Fills in VESA-compatible information about supported video modes
  in the space left for this purpose at the beginning of the
  generated VGA ROM assembly code.
  (See VESA BIOS EXTENSION Core Functions Standard v3.0, p26+.)

  @param[in] StartAddress Where to begin writing VESA information.
  @param[in] EndAddress   Pointer to the next byte after the end
                          of all video mode information data.

  @retval EFI_SUCCESS     The operation was successful
  @return other           The operation failed.

**/
EFI_STATUS
ShimVesaInformation (
  IN  EFI_PHYSICAL_ADDRESS  StartAddress,
  OUT EFI_PHYSICAL_ADDRESS  *EndAddress
  )
{
  VBE_INFO              *VbeInfoFull;
  VBE_INFO_BASE         *VbeInfo;
  VBE_MODE_INFO         *VbeModeInfo;
  UINT8                 *BufferPtr;
  UINT32                HorizontalOffsetPx;
  UINT32                VerticalOffsetPx;
  EFI_PHYSICAL_ADDRESS  FrameBufferBaseWithOffset;

  if ((StartAddress == 0) || (EndAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get basic video hardware information first.
  //
  if (EFI_ERROR (EnsureDisplayAvailable ())) {
    PrintError (L"No display adapters were found, unable to fill in VESA information\n");
    return EFI_NOT_FOUND;
  }

  //
  // VESA general information.
  //
  VbeInfoFull = (VBE_INFO *)(UINTN)StartAddress;
  VbeInfo   = &VbeInfoFull->Base;
  BufferPtr = VbeInfoFull->Buffer;
  CopyMem (VbeInfo->Signature, "VESA", 4);
  VbeInfo->VesaVersion                  = 0x0300;
  VbeInfo->OemNameAddress               = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  CopyMem (BufferPtr, VENDOR_NAME, sizeof (VENDOR_NAME));
  BufferPtr += sizeof (VENDOR_NAME);
  VbeInfo->Capabilities                 = BIT0;     // DAC width supports 8-bit color mode
  VbeInfo->ModeListAddress              = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  *(UINT16 *)BufferPtr = 0x00F1;   // mode number
  BufferPtr += 2;
  *(UINT16 *)BufferPtr = 0xFFFF;   // mode list terminator
  BufferPtr += 2;
  VbeInfo->VideoMem64K                  = (UINT16)((mDisplayInfo.FrameBufferSize + 65535) / 65536);
  VbeInfo->OemSoftwareVersion           = 0x0000;
  VbeInfo->VendorNameAddress            = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  CopyMem (BufferPtr, VENDOR_NAME, sizeof (VENDOR_NAME));
  BufferPtr += sizeof (VENDOR_NAME);
  VbeInfo->ProductNameAddress           = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  CopyMem (BufferPtr, PRODUCT_NAME, sizeof (PRODUCT_NAME));
  BufferPtr += sizeof (PRODUCT_NAME);
  VbeInfo->ProductRevAddress            = (UINT32)StartAddress << 12 | (UINT16)(UINTN)BufferPtr;
  CopyMem (BufferPtr, PRODUCT_REVISION, sizeof (PRODUCT_REVISION));
  BufferPtr += sizeof (PRODUCT_REVISION);

  //
  // Basic VESA mode information.
  //
  VbeModeInfo = (VBE_MODE_INFO *)(VbeInfoFull + 1); // jump ahead by sizeof (VBE_INFO) ie. 256 bytes
  // bit0: mode supported by present hardware configuration
  // bit1: must be set for VBE v1.2+
  // bit3: color mode
  // bit4: graphics mode
  // bit5: mode not VGA-compatible (do not access VGA I/O ports and registers)
  // bit6: disable windowed memory mode = linear framebuffer only
  // bit7: linear framebuffer supported
  VbeModeInfo->ModeAttr                 = BIT7 | BIT6 | BIT5 | BIT4 | BIT3 | BIT1 | BIT0;

  //
  // Resolution.
  //
  VbeModeInfo->Width                    = 1024;   // as expected by Windows installer
  VbeModeInfo->Height                   = 768;    // as expected by Windows installer
  VbeModeInfo->CharCellWidth            = 8;      // used to calculate resolution in text modes
  VbeModeInfo->CharCellHeight           = 16;     // used to calculate resolution in text modes

  //
  // Center visible image on screen using framebuffer offset.
  //
  HorizontalOffsetPx        = (mDisplayInfo.HorizontalResolution - 1024) / 2;
  VerticalOffsetPx          = (mDisplayInfo.VerticalResolution - 768) / 2 * mDisplayInfo.PixelsPerScanLine;
  FrameBufferBaseWithOffset = mDisplayInfo.FrameBufferBase
                                + VerticalOffsetPx * 4      // 4 bytes per pixel
                                + HorizontalOffsetPx * 4;   // 4 bytes per pixel

  //
  // Memory access (banking, windowing, paging).
  //
  VbeModeInfo->NumBanks                 = 1;      // disable memory banking
  VbeModeInfo->BankSizeKB               = 0;      // disable memory banking
  VbeModeInfo->LfbAddress               = (UINT32)FrameBufferBaseWithOffset;            // 32-bit physical address
  VbeModeInfo->BytesPerScanLineLinear   = (UINT16)mDisplayInfo.PixelsPerScanLine * 4;   // logical bytes in linear modes
  VbeModeInfo->NumImagePagesLessOne     = 0;      // disable image paging
  VbeModeInfo->NumImagesLessOneLinear   = 0;      // disable image paging
  VbeModeInfo->WindowPositioningAddress = 0x0;    // force windowing to Function 5h
  VbeModeInfo->WindowAAttr              = 0x0;    // window disabled
  VbeModeInfo->WindowBAttr              = 0x0;    // window disabled
  VbeModeInfo->WindowGranularityKB      = 0x0;    // window disabled ie. not relocatable
  VbeModeInfo->WindowSizeKB             = 0x0;    // window disabled
  VbeModeInfo->WindowAStartSegment      = 0x0;    // linear framebuffer only
  VbeModeInfo->WindowBStartSegment      = 0x0;    // linear framebuffer only

  //
  // Color mode.
  //
  VbeModeInfo->NumPlanes                = 1;      // packed pixel mode
  VbeModeInfo->MemoryModel              = 6;      // Direct Color
  VbeModeInfo->DirectColorModeInfo      = BIT1;   // alpha bytes may be used by application
  VbeModeInfo->BitsPerPixel             = 32;     // 8+8+8+8 bits per channel
  VbeModeInfo->BlueMaskSizeLinear       = 8;
  VbeModeInfo->GreenMaskSizeLinear      = 8;
  VbeModeInfo->RedMaskSizeLinear        = 8;
  VbeModeInfo->ReservedMaskSizeLinear   = 8;

  if (mDisplayInfo.PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
    VbeModeInfo->BlueMaskPosLinear      = 0;      // blue offset
    VbeModeInfo->GreenMaskPosLinear     = 8;      // green offset
    VbeModeInfo->RedMaskPosLinear       = 16;     // red offset
    VbeModeInfo->ReservedMaskPosLinear  = 24;     // reserved offset
  } else if (mDisplayInfo.PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
    VbeModeInfo->RedMaskPosLinear       = 0;      // red offset
    VbeModeInfo->GreenMaskPosLinear     = 8;      // green offset
    VbeModeInfo->BlueMaskPosLinear      = 16;     // blue offset
    VbeModeInfo->ReservedMaskPosLinear  = 24;     // alpha offset
  } else {
    PrintError (L"Unsupported value of PixelFormat (%d), aborting\n", mDisplayInfo.PixelFormat);
    return EFI_UNSUPPORTED;
  }

  //
  // Other.
  //
  VbeModeInfo->OffScreenAddress         = 0;      // reserved, always set to 0
  VbeModeInfo->OffScreenSizeKB          = 0;      // reserved, always set to 0
  VbeModeInfo->MaxPixelClockHz          = 0;      // maximum available refresh rate
  VbeModeInfo->Vbe3                     = 0x01;   // reserved, always set to 1

  *EndAddress = (UINTN)(VbeModeInfo + 1);         // jump ahead by sizeof (VBE_MODE_INFO) ie. 256 bytes

  return EFI_SUCCESS;
}


/**
  Checkes if an Int10h handler is already defined in the
  Interrupt Vector Table (IVT), points to somewhere
  within VGA ROM memory and this memory is not filled
  with protective opcodes.

  @retval TRUE            An Int10h handler was found in IVT.
  @retval FALSE           An Int10h handler was not found in IVT.

**/
BOOLEAN
IsInt10hHandlerDefined (
  VOID
  )
{
  CONST STATIC UINT8    PROTECTIVE_OPCODE_1 = 0xFF;
  CONST STATIC UINT8    PROTECTIVE_OPCODE_2 = 0x00;
  IVT_ENTRY             *Int10hEntry;
  EFI_PHYSICAL_ADDRESS  Int10hHandler;
  UINT8                 Opcode;

  // Fetch 10h entry in IVT.
  Int10hEntry = (IVT_ENTRY *)(UINTN)IVT_ADDRESS + 0x10;
  // Convert handler address from real mode segment address to 32bit physical address.
  Int10hHandler = (Int10hEntry->Segment << 4) + Int10hEntry->Offset;

  if ((Int10hHandler >= VGA_ROM_ADDRESS) && (Int10hHandler < (VGA_ROM_ADDRESS + VGA_ROM_SIZE))) {
    PrintDebug (L"Int10h IVT entry points at location within VGA ROM memory area (%04x:%04x)\n",
      Int10hEntry->Segment, Int10hEntry->Offset);

    Opcode = *((UINT8 *)Int10hHandler);
    if ((Opcode == PROTECTIVE_OPCODE_1) || (Opcode == PROTECTIVE_OPCODE_2)) {
      PrintDebug (L"First Int10h handler instruction at %04x:%04x (%02x) not valid, rejecting handler\n",
        Int10hEntry->Segment, Int10hEntry->Offset, Opcode);
      return FALSE;
    } else {
      PrintDebug (L"First Int10h handler instruction at %04x:%04x (%02x) valid, accepting handler\n",
        Int10hEntry->Segment, Int10hEntry->Offset, Opcode);
      return TRUE;
    }
  } else {
    PrintDebug (L"Int10h IVT entry points at location (%04x:%04x) outside VGA ROM memory area (%04x..%04x), rejecting handler\n",
      Int10hEntry->Segment, Int10hEntry->Offset, VGA_ROM_ADDRESS, VGA_ROM_ADDRESS+VGA_ROM_SIZE);
    return FALSE;
  }
}

/**
  Checks if writes are possible in a particular memory area.

  @param[in] Address      The memory location to be checked.

  @retval TRUE            Writes to the specified location are
                          allowed changes are persisted.
  @retval FALSE           Writes to the specified location are
                          not allowed or have no effect.

**/
BOOLEAN
CanWriteAtAddress (
  IN  EFI_PHYSICAL_ADDRESS  Address
  )
{
  BOOLEAN   CanWrite;
  UINT8     *TestPtr;
  UINT8     OldValue;

  TestPtr = (UINT8 *)(Address);
  OldValue = *TestPtr;

  *TestPtr = *TestPtr + 1;
  CanWrite = OldValue != *TestPtr;

  *TestPtr = OldValue;

  return CanWrite;
}

/**
  Attempts to either unlock a memory area for writing or
  lock it to prevent writes. Makes use of a number of approaches
  to achieve the desired result.

  @param[in] StartAddress   Where the desired memory area begins.
  @param[in] Length         Number of bytes from StartAddress that
                            need to be locked or unlocked.
  @param[in] Operation      Whether the area is to be locked or unlocked.

  @retval TRUE              An Int10h handler was found in IVT.
  @retval FALSE             An Int10h handler was not found in IVT.

**/
EFI_STATUS
EnsureMemoryLock (
  IN  EFI_PHYSICAL_ADDRESS    StartAddress,
  IN  UINT32                  Length,
  IN  MEMORY_LOCK_OPERATION   Operation
  )
{
  EFI_STATUS                    Status = EFI_NOT_READY;
  UINT32                        Granularity;
  EFI_LEGACY_REGION_PROTOCOL    *LegacyRegion;
  EFI_LEGACY_REGION2_PROTOCOL   *LegacyRegion2;
  CONST CHAR16                  *OperationStr;

  if ((StartAddress == 0) || (Length == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  switch (Operation) {
    case UNLOCK:
      OperationStr = L"unlock";
      break;
    case LOCK:
      OperationStr = L"lock";
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  //
  // Check if we need to perform any operation.
  //
  if ((Operation == UNLOCK) && CanWriteAtAddress (StartAddress)) {
    PrintDebug (L"Memory at %x already %sed\n", StartAddress, OperationStr);
    Status = EFI_SUCCESS;
  } else if ((Operation == LOCK) && !CanWriteAtAddress (StartAddress)) {
    PrintDebug (L"Memory at %x already %sed\n", StartAddress, OperationStr);
    Status = EFI_SUCCESS;
  }

  //
  // Try to lock/unlock with EfiLegacyRegionProtocol.
  //
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiLegacyRegionProtocolGuid, NULL, (VOID **)&LegacyRegion);
    if (!EFI_ERROR (Status)) {
      if (Operation == UNLOCK) {
        /*Status =*/ LegacyRegion->UnLock (LegacyRegion, (UINT32)StartAddress, Length, &Granularity);
        Status = CanWriteAtAddress (StartAddress) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
      } else {
        /*Status =*/ LegacyRegion->Lock (LegacyRegion, (UINT32)StartAddress, Length, &Granularity);
        Status = CanWriteAtAddress (StartAddress) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
      }

      PrintDebug (L"%s %sing memory at %x with EfiLegacyRegionProtocol\n",
        EFI_ERROR (Status) ? L"Failure" : L"Success",
        OperationStr,
        StartAddress);
    }
  }

  //
  // Try to lock/unlock with EfiLegacyRegion2Protocol.
  //
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiLegacyRegion2ProtocolGuid, NULL, (VOID **)&LegacyRegion2);
    if (!EFI_ERROR (Status)) {
      if (Operation == UNLOCK) {
        /*Status =*/ LegacyRegion2->UnLock (LegacyRegion2, (UINT32)StartAddress, Length, &Granularity);
        Status = CanWriteAtAddress (StartAddress) ? EFI_SUCCESS : EFI_DEVICE_ERROR;;
      } else {
        /*Status =*/ LegacyRegion2->Lock (LegacyRegion2, (UINT32)StartAddress, Length, &Granularity);
        Status = CanWriteAtAddress (StartAddress) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
      }

      PrintDebug (L"%s %sing memory at %x with EfiLegacyRegion2Protocol\n",
        EFI_ERROR (Status) ? L"Failure" : L"Success",
        OperationStr,
        StartAddress);
    }
  }

  //
  // Try to lock/unlock via an MTRR.
  //
  if (EFI_ERROR (Status) && IsMtrrSupported () && (FIXED_MTRR_SIZE >= Length)) {
    if (Operation == UNLOCK) {
      MtrrSetMemoryAttribute (StartAddress, FIXED_MTRR_SIZE, CacheUncacheable);
      Status = CanWriteAtAddress (StartAddress) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    } else {
      MtrrSetMemoryAttribute (StartAddress, FIXED_MTRR_SIZE, CacheWriteProtected);
      Status = CanWriteAtAddress (StartAddress) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    }

    PrintDebug (L"%s %sing memory at %x with MTRRs\n",
      EFI_ERROR (Status) ? L"Failure" : L"Success",
      OperationStr,
      StartAddress);
  }

  //
  // None of the methods worked?
  //
  if (EFI_ERROR (Status)) {
    PrintDebug (L"Unable to find a way to %s memory at %x\n", OperationStr, StartAddress);
  }

  return Status;
}

VOID
WaitForEnter (
  IN  BOOLEAN   PrintMessage
  )
{
  EFI_INPUT_KEY   Key;
  UINTN           EventIndex;

  if (PrintMessage) {
    PrintDebug (L"Press Enter to continue\n");
  }

  gST->ConIn->Reset (gST->ConIn, FALSE);
  do {
    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &EventIndex);
    gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  } while (Key.UnicodeChar != CHAR_CARRIAGE_RETURN);
}

//
// Paths to the driver to try
//
#define EFIGUARD_DRIVER_FILENAME		L"EfiGuardDxe.efi"
STATIC CHAR16* mDriverPaths[] = {
	L"\\EFI\\Boot\\" EFIGUARD_DRIVER_FILENAME,
	L"\\EFI\\" EFIGUARD_DRIVER_FILENAME,
	L"\\" EFIGUARD_DRIVER_FILENAME
};

STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *mTextInputEx = NULL;

VOID
EFIAPI
BmRepairAllControllers(
	IN UINTN ReconnectRepairCount
	);

VOID
EFIAPI
BmSetMemoryTypeInformationVariable(
	IN BOOLEAN Boot
	);

BOOLEAN
EFIAPI
BmIsAutoCreateBootOption(
	IN EFI_BOOT_MANAGER_LOAD_OPTION *BootOption
	);

STATIC
VOID
ResetTextInput(
	VOID
	)
{
	if (mTextInputEx != NULL)
		mTextInputEx->Reset(mTextInputEx, FALSE);
	else
		gST->ConIn->Reset(gST->ConIn, FALSE);
}

STATIC
UINT16
EFIAPI
WaitForKey(
	VOID
	)
{
	EFI_KEY_DATA KeyData = { 0 };
	UINTN Index = 0;
	if (mTextInputEx != NULL)
	{
		gBS->WaitForEvent(1, &mTextInputEx->WaitForKeyEx, &Index);
		mTextInputEx->ReadKeyStrokeEx(mTextInputEx, &KeyData);
	}
	else
	{
		gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
		gST->ConIn->ReadKeyStroke(gST->ConIn, &KeyData.Key);
	}
	return KeyData.Key.ScanCode;
}

STATIC
UINT16
EFIAPI
WaitForKeyWithTimeout(
	IN UINTN Milliseconds
	)
{
	ResetTextInput();
	gBS->Stall(Milliseconds * 1000);

	EFI_KEY_DATA KeyData = { 0 };
	if (mTextInputEx != NULL)
		mTextInputEx->ReadKeyStrokeEx(mTextInputEx, &KeyData);
	else
		gST->ConIn->ReadKeyStroke(gST->ConIn, &KeyData.Key);

	ResetTextInput();
	return KeyData.Key.ScanCode;
}

STATIC
UINT16
EFIAPI
PromptInput(
	IN CONST UINT16* AcceptedChars,
	IN UINTN NumAcceptedChars,
	IN UINT16 DefaultSelection
	)
{
	UINT16 SelectedChar;

	while (TRUE)
	{
		SelectedChar = CHAR_NULL;

		EFI_KEY_DATA KeyData = { 0 };
		UINTN Index = 0;
		if (mTextInputEx != NULL)
		{
			gBS->WaitForEvent(1, &mTextInputEx->WaitForKeyEx, &Index);
			mTextInputEx->ReadKeyStrokeEx(mTextInputEx, &KeyData);
		}
		else
		{
			gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
			gST->ConIn->ReadKeyStroke(gST->ConIn, &KeyData.Key);
		}

		if (KeyData.Key.UnicodeChar == CHAR_LINEFEED || KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN)
		{
			SelectedChar = DefaultSelection;
			break;
		}

		for (UINTN i = 0; i < NumAcceptedChars; ++i)
		{
			if (KeyData.Key.UnicodeChar == AcceptedChars[i])
			{
				SelectedChar = KeyData.Key.UnicodeChar;
				break;
			}
		}

		if (SelectedChar != CHAR_NULL)
			break;
	}

	Print(L"%c\r\n\r\n", SelectedChar);
	return SelectedChar;
}

STATIC
CONST CHAR16*
EFIAPI
StriStr(
	IN CONST CHAR16 *String1,
	IN CONST CHAR16 *String2
	)
{
	if (*String2 == L'\0')
		return String1;

	while (*String1 != L'\0')
	{
		CONST CHAR16* FirstMatch = String1;
		CONST CHAR16* String2Ptr = String2;
		CHAR16 String1Char = CharToUpper(*String1);
		CHAR16 String2Char = CharToUpper(*String2Ptr);

		while (String1Char == String2Char && String1Char != L'\0')
		{
			String1++;
			String2Ptr++;

			String1Char = CharToUpper(*String1);
			String2Char = CharToUpper(*String2Ptr);
		}

		if (String2Char == L'\0')
			return FirstMatch;

		if (String1Char == L'\0')
			return NULL;

		String1 = FirstMatch + 1;
	}
	return NULL;
}

// 
// Try to find a file by browsing each device
// 
STATIC
EFI_STATUS
LocateFile(
	IN CHAR16* ImagePath,
	OUT EFI_DEVICE_PATH** DevicePath
	)
{
	*DevicePath = NULL;

	UINTN NumHandles;
	EFI_HANDLE* Handles;
	EFI_STATUS Status = gBS->LocateHandleBuffer(ByProtocol,
												&gEfiSimpleFileSystemProtocolGuid,
												NULL,
												&NumHandles,
												&Handles);
	if (EFI_ERROR(Status))
		return Status;

	DEBUG((DEBUG_INFO, "[LOADER] Number of UEFI Filesystem Devices: %llu\r\n", NumHandles));

	for (UINTN i = 0; i < NumHandles; i++)
	{
		EFI_FILE_IO_INTERFACE *IoDevice;
		Status = gBS->OpenProtocol(Handles[i],
									&gEfiSimpleFileSystemProtocolGuid,
									(VOID**)&IoDevice,
									gImageHandle,
									NULL,
									EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (Status != EFI_SUCCESS)
			continue;

		EFI_FILE_HANDLE VolumeHandle;
		Status = IoDevice->OpenVolume(IoDevice, &VolumeHandle);
		if (EFI_ERROR(Status))
			continue;

		EFI_FILE_HANDLE FileHandle;
		Status = VolumeHandle->Open(VolumeHandle,
									&FileHandle,
									ImagePath,
									EFI_FILE_MODE_READ,
									EFI_FILE_READ_ONLY);
		if (!EFI_ERROR(Status))
		{
			FileHandle->Close(FileHandle);
			VolumeHandle->Close(VolumeHandle);
			*DevicePath = FileDevicePath(Handles[i], ImagePath);
			CHAR16 *PathString = ConvertDevicePathToText(*DevicePath, TRUE, TRUE);
			DEBUG((DEBUG_INFO, "[LOADER] Found file at %S.\r\n", PathString));
			if (PathString != NULL)
				FreePool(PathString);
			break;
		}
		VolumeHandle->Close(VolumeHandle);
	}

	FreePool(Handles);

	return Status;
}

//
// Find the optimal available console output mode and set it if it's not already the current mode
//
STATIC
EFI_STATUS
EFIAPI
SetHighestAvailableTextMode(
	VOID
	)
{
	if (gST->ConOut == NULL)
		return EFI_NOT_READY;

	INT32 MaxModeNum = 0;
	UINTN Cols, Rows, MaxWeightedColsXRows = 0;
	EFI_STATUS Status = EFI_SUCCESS;

	for (INT32 ModeNum = 0; ModeNum < gST->ConOut->Mode->MaxMode; ModeNum++)
	{
		Status = gST->ConOut->QueryMode(gST->ConOut, ModeNum, &Cols, &Rows);
		if (EFI_ERROR(Status))
			continue;

		// Accept only modes where the total of (Rows * Columns) >= the previous known best.
		// Use 16:10 as an arbitrary weighting that lies in between the common 4:3 and 16:9 ratios
		CONST UINTN WeightedColsXRows = (16 * Rows) * (10 * Cols);
		if (WeightedColsXRows >= MaxWeightedColsXRows)
		{
			MaxWeightedColsXRows = WeightedColsXRows;
			MaxModeNum = ModeNum;
		}
	}

	if (gST->ConOut->Mode->Mode != MaxModeNum)
	{
		Status = gST->ConOut->SetMode(gST->ConOut, MaxModeNum);
	}

	// Clear screen and enable cursor
	gST->ConOut->ClearScreen(gST->ConOut);
	gST->ConOut->EnableCursor(gST->ConOut, TRUE);

	return Status;
}

STATIC
EFI_STATUS
EFIAPI
StartEfiGuard(
	IN BOOLEAN InteractiveConfiguration
	)
{
	EFIGUARD_DRIVER_PROTOCOL* EfiGuardDriverProtocol;
	EFI_DEVICE_PATH *DriverDevicePath = NULL;

	// 
	// Check if the driver is loaded 
	// 
	EFI_STATUS Status = gBS->LocateProtocol(&gEfiGuardDriverProtocolGuid,
											NULL,
											(VOID**)&EfiGuardDriverProtocol);
	ASSERT((!EFI_ERROR(Status) || Status == EFI_NOT_FOUND));
	if (Status == EFI_NOT_FOUND)
	{
		Print(L"[LOADER] Locating and loading driver file %S...\r\n", EFIGUARD_DRIVER_FILENAME);
		for (UINT32 i = 0; i < ARRAY_SIZE(mDriverPaths); ++i)
		{
			Status = LocateFile(mDriverPaths[i], &DriverDevicePath);
			if (!EFI_ERROR(Status))
				break;
		}
		if (EFI_ERROR(Status))
		{
			Print(L"[LOADER] Failed to find driver file %S.\r\n", EFIGUARD_DRIVER_FILENAME);
			goto Exit;
		}

		EFI_HANDLE DriverHandle = NULL;
		Status = gBS->LoadImage(FALSE, // Request is not from boot manager
								gImageHandle,
								DriverDevicePath,
								NULL,
								0,
								&DriverHandle);
		if (EFI_ERROR(Status))
		{
			Print(L"[LOADER] LoadImage failed: %llx (%r).\r\n", Status, Status);
			goto Exit;
		}

		Status = gBS->StartImage(DriverHandle, NULL, NULL);
		if (EFI_ERROR(Status))
		{
			Print(L"[LOADER] StartImage failed: %llx (%r).\r\n", Status, Status);
			goto Exit;
		}
	}
	else
	{
		ASSERT_EFI_ERROR(Status);
		Print(L"[LOADER] The driver is already loaded.\r\n");
	}

	Status = gBS->LocateProtocol(&gEfiGuardDriverProtocolGuid,
								NULL,
								(VOID**)&EfiGuardDriverProtocol);
	if (EFI_ERROR(Status))
	{
		Print(L"[LOADER] LocateProtocol failed: %llx (%r).\r\n", Status, Status);
		goto Exit;
	}

	if (InteractiveConfiguration)
	{
		//
		// Interactive driver configuration
		//
		Print(L"\r\nChoose the type of DSE bypass to use, or press ENTER for default:\r\n"
			L"    [1] Boot time DSE bypass (default)\r\n    [2] Runtime SetVariable hook\r\n    [3] No DSE bypass\r\n    ");
		CONST UINT16 AcceptedDseBypasses[] = { L'1', L'2', L'3' };
		CONST UINT16 SelectedDseBypass = PromptInput(AcceptedDseBypasses,
													sizeof(AcceptedDseBypasses) / sizeof(UINT16),
													L'1');

		Print(L"Wait for a keypress to continue after each patch stage?\n"
			L"    [1] No (default)\r\n    [2] Yes (for debugging)\r\n    ");
		CONST UINT16 NoYes[] = { L'1', L'2' };
		CONST UINT16 SelectedWaitForKeyPress = PromptInput(NoYes,
														sizeof(NoYes) / sizeof(UINT16),
														L'1');

		EFIGUARD_CONFIGURATION_DATA ConfigData;
		switch (SelectedDseBypass)
		{
		case L'1':
		default:
			ConfigData.DseBypassMethod = DSE_DISABLE_AT_BOOT;
			break;
		case L'2':
			ConfigData.DseBypassMethod = DSE_DISABLE_SETVARIABLE_HOOK;
			break;
		case L'3':
			ConfigData.DseBypassMethod = DSE_DISABLE_NONE;
			break;
		}
		ConfigData.WaitForKeyPress = (BOOLEAN)(SelectedWaitForKeyPress == L'2');

		//
		// Send the configuration data to the driver
		//
		Status = EfiGuardDriverProtocol->Configure(&ConfigData);

		if (EFI_ERROR(Status))
			Print(L"[LOADER] Driver Configure() returned error %llx (%r).\r\n", Status, Status);
	}

Exit:
	if (DriverDevicePath != NULL)
		FreePool(DriverDevicePath);

	return Status;
}

//
// Attempt to boot each Windows boot option in the BootOptions array.
// This function is a combined and simplified version of BootBootOptions (BdsDxe) and EfiBootManagerBoot (UefiBootManagerLib),
// except for the fact that we are of course not in the BDS phase and also not a driver or the platform boot manager.
// The Windows boot manager doesn't have to know about all this, that would only confuse it
//
STATIC
BOOLEAN
TryBootOptionsInOrder(
	IN EFI_BOOT_MANAGER_LOAD_OPTION *BootOptions,
	IN UINTN BootOptionCount,
	IN UINT16 CurrentBootOptionIndex,
	IN BOOLEAN OnlyBootWindows
	)
{
	//
	// Iterate over the boot options 'in BootOrder order'
	//
	EFI_DEVICE_PATH_PROTOCOL* FullPath;
	for (UINTN Index = 0; Index < BootOptionCount; ++Index)
	{
		//
		// This is us
		//
		if (BootOptions[Index].OptionNumber == CurrentBootOptionIndex)
			continue;

		//
		// No LOAD_OPTION_ACTIVE, no load
		//
		if ((BootOptions[Index].Attributes & LOAD_OPTION_ACTIVE) == 0)
			continue;

		//
		// Ignore LOAD_OPTION_CATEGORY_APP entries
		//
		if ((BootOptions[Index].Attributes & LOAD_OPTION_CATEGORY) != LOAD_OPTION_CATEGORY_BOOT)
			continue;

		//
		// Ignore legacy (BBS) entries, unless non-Windows entries are allowed (second boot attempt)
		//
		const BOOLEAN IsLegacy = DevicePathType(BootOptions[Index].FilePath) == BBS_DEVICE_PATH &&
			DevicePathSubType(BootOptions[Index].FilePath) == BBS_BBS_DP;
		if (OnlyBootWindows && IsLegacy)
			continue;

		//
		// Filter out non-Windows boot entries.
		// Check the description first as "Windows Boot Manager" entries are obviously going to boot Windows.
		// However the inverse is not true, i.e. not all entries that boot Windows will have this description.
		//
		BOOLEAN MaybeWindows = FALSE;
		if (BootOptions[Index].Description != NULL &&
			StrStr(BootOptions[Index].Description, L"Windows Boot Manager") != NULL)
		{
			MaybeWindows = TRUE;
		}

		// We need the full path to LoadImage the file with BootPolicy = TRUE.
		UINTN FileSize;
		VOID* FileBuffer = EfiBootManagerGetLoadOptionBuffer(BootOptions[Index].FilePath, &FullPath, &FileSize);
		if (FileBuffer != NULL)
			FreePool(FileBuffer);

		// EDK2's EfiBootManagerGetLoadOptionBuffer will sometimes give a NULL "full path"
		// from an originally non-NULL file path. If so, swap it back (and don't free it).
		if (FullPath == NULL)
			FullPath = BootOptions[Index].FilePath;

		// Get the text representation of the device path
		CHAR16* ConvertedPath = ConvertDevicePathToText(FullPath, FALSE, FALSE);

		// If this is not a named "Windows Boot Manager" entry, apply some heuristics based on the device path,
		// which must end in "bootmgfw.efi" or "bootx64.efi". In the latter case we may get false positives,
		// but for some types of boots the filename will always be bootx64.efi, so this can't be avoided.
		if (!MaybeWindows &&
			ConvertedPath != NULL &&
			(StriStr(ConvertedPath, L"bootmgfw.efi") != NULL || StriStr(ConvertedPath, L"bootx64.efi") != NULL))
		{
			MaybeWindows = TRUE;
		}

		if (OnlyBootWindows && !MaybeWindows)
		{
			if (FullPath != BootOptions[Index].FilePath)
				FreePool(FullPath);
			if (ConvertedPath != NULL)
				FreePool(ConvertedPath);
			
			// Not Windows; skip this entry
			continue;
		}

		// Print what we're booting
		if (ConvertedPath != NULL)
		{
			Print(L"Booting \"%S\"...\r\n    -> %S = %S\r\n",
				(BootOptions[Index].Description != NULL ? BootOptions[Index].Description : L"<null description>"),
				IsLegacy ? L"Legacy path" : L"Path", ConvertedPath);
			FreePool(ConvertedPath);
		}

		//
		// Boot this image.
		//
		// DO NOT: call EfiBootManagerBoot(BootOption) to 'simplify' this process.
		// The driver will not work in this case due to EfiBootManagerBoot calling BmSetMemoryTypeInformationVariable(),
		// which performs a warm reset of the system if, for example, the category of the current boot option changed
		// from 'app' to 'boot'. Which is precisely what we are doing...
		//
		// Change the BootCurrent variable to the option number for our boot selection
		UINT16 OptionNumber = (UINT16)BootOptions[Index].OptionNumber;
		EFI_STATUS Status = gRT->SetVariable(EFI_BOOT_CURRENT_VARIABLE_NAME,
											&gEfiGlobalVariableGuid,
											EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
											sizeof(UINT16),
											&OptionNumber);
		ASSERT_EFI_ERROR(Status);

		// Signal the EVT_SIGNAL_READY_TO_BOOT event
		EfiSignalEventReadyToBoot();
		REPORT_STATUS_CODE(EFI_PROGRESS_CODE, (EFI_SOFTWARE_DXE_BS_DRIVER | EFI_SW_DXE_BS_PC_READY_TO_BOOT_EVENT));

		// Repair system through DriverHealth protocol
		BmRepairAllControllers(0);

		// Save the memory map in the MemoryTypeInformation variable for resuming from ACPI S4 (hibernate)
		BmSetMemoryTypeInformationVariable((BootOptions[Index].Attributes & LOAD_OPTION_CATEGORY) == LOAD_OPTION_CATEGORY_BOOT);

		// Handle BBS entries
		if (IsLegacy)
		{
			Print(L"\r\nNOTE: EfiGuard does not support legacy (non-UEFI) Windows installations.\r\n"
				L"The legacy OS will be booted, but EfiGuard will not work.\r\nPress any key to acknowledge...\r\n");
			WaitForKey();

			EFI_LEGACY_BIOS_PROTOCOL *LegacyBios;
			Status = gBS->LocateProtocol(&gEfiLegacyBiosProtocolGuid,
										NULL,
										(VOID**)&LegacyBios);
			ASSERT_EFI_ERROR(Status);

			BootOptions[Index].Status = LegacyBios->LegacyBoot(LegacyBios,
															(BBS_BBS_DEVICE_PATH*)BootOptions[Index].FilePath,
															BootOptions[Index].OptionalDataSize,
															BootOptions[Index].OptionalData);
			return !EFI_ERROR(BootOptions[Index].Status);
		}

		// Ensure the image path is connected end-to-end by Dispatch()ing any required drivers through DXE services
		EfiBootManagerConnectDevicePath(BootOptions[Index].FilePath, NULL);

		// Instead of creating a ramdisk and reading the file into it (¿que?), just pass the path we saved earlier.
		// This is the point where the driver kicks in via its LoadImage hook.
		REPORT_STATUS_CODE(EFI_PROGRESS_CODE, PcdGet32(PcdProgressCodeOsLoaderLoad));
		EFI_HANDLE ImageHandle = NULL;
		Status = gBS->LoadImage(TRUE,
								gImageHandle,
								FullPath,
								NULL,
								0,
								&ImageHandle);

		if (FullPath != BootOptions[Index].FilePath)
			FreePool(FullPath);

		if (EFI_ERROR(Status))
		{
			// Unload if execution could not be deferred to avoid a resource leak
			if (Status == EFI_SECURITY_VIOLATION)
				gBS->UnloadImage(ImageHandle);

			Print(L"LoadImage error %llx (%r)\r\n", Status, Status);
			BootOptions[Index].Status = Status;
			continue;
		}

		// Get loaded image info
		EFI_LOADED_IMAGE_PROTOCOL* ImageInfo;
		Status = gBS->OpenProtocol(ImageHandle,
									&gEfiLoadedImageProtocolGuid,
									(VOID**)&ImageInfo,
									gImageHandle,
									NULL,
									EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		ASSERT_EFI_ERROR(Status);

		// Set image load options from the boot option
		if (!BmIsAutoCreateBootOption(&BootOptions[Index]))
		{
			ImageInfo->LoadOptionsSize = BootOptions[Index].OptionalDataSize;
			ImageInfo->LoadOptions = BootOptions[Index].OptionalData;
		}

		// "Clean to NULL because the image is loaded directly from the firmware's boot manager." (EDK2) Good call, I agree
		ImageInfo->ParentHandle = NULL;

		// Enable the Watchdog Timer for 5 minutes before calling the image
		gBS->SetWatchdogTimer((UINTN)(5 * 60), 0x0000, 0x00, NULL);

		// Start the image and set the return code in the boot option status
		REPORT_STATUS_CODE(EFI_PROGRESS_CODE, PcdGet32(PcdProgressCodeOsLoaderStart));
		Status = gBS->StartImage(ImageHandle,
								&BootOptions[Index].ExitDataSize,
								&BootOptions[Index].ExitData);
		BootOptions[Index].Status = Status;
		if (EFI_ERROR(Status))
		{
			Print(L"StartImage error %llx (%r)\r\n", Status, Status);
			continue;
		}

		//
		// Success. Code below is never executed
		//

		// Clear the watchdog timer after the image returns
		gBS->SetWatchdogTimer(0x0000, 0x0000, 0x0000, NULL);

		// Clear the BootCurrent variable
		gRT->SetVariable(EFI_BOOT_CURRENT_VARIABLE_NAME,
						&gEfiGlobalVariableGuid,
						0,
						0,
						NULL);

		if (BootOptions[Index].Status == EFI_SUCCESS)
			return TRUE;
	}

	// All boot attempts failed, or no suitable entries were found
	return FALSE;
}

EFI_STATUS
EFIAPI
InstallInt10hHandler(
	VOID
	)
{
	EFI_PHYSICAL_ADDRESS    Int10hHandlerAddress;
	IVT_ENTRY               *IvtInt10hHandlerEntry;
	IVT_ENTRY               NewInt10hHandlerEntry;
	EFI_PHYSICAL_ADDRESS    IvtAddress;
	EFI_STATUS              Status;
	EFI_STATUS              IvtAllocationStatus;
	EFI_STATUS              IvtFreeStatus;

	IvtFreeStatus = gBS->FreePages (IVT_ADDRESS, 1);

	IvtAddress = IVT_ADDRESS;
	IvtAllocationStatus = gBS->AllocatePages (AllocateAddress, EfiBootServicesCode, 1, &IvtAddress);

	PrintDebug (L"Force free IVT area result: %r\n", IvtFreeStatus);

	if (IsInt10hHandlerDefined())
	{
		PrintDebug(L"Int10h already has a handler, no further action required\r\n");
		return EFI_SUCCESS;
	}

	if (sizeof(INT10H_HANDLER) > VGA_ROM_SIZE)
	{
		PrintError(L"Shim size bigger than allowed (%u > %u), aborting\r\n",
      		sizeof(INT10H_HANDLER), VGA_ROM_SIZE);
		return EFI_BUFFER_TOO_SMALL;
	}

	Status = EnsureMemoryLock(VGA_ROM_ADDRESS, (UINT32)VGA_ROM_SIZE, UNLOCK);
	if (EFI_ERROR(Status))
	{
		PrintError(L"Unable to unlock VGA ROM memory at %04x, aborting\r\n", VGA_ROM_ADDRESS);
		return Status;
	}

	ZeroMem((VOID*)VGA_ROM_ADDRESS, VGA_ROM_SIZE);
	CopyMem((VOID*)VGA_ROM_ADDRESS, INT10H_HANDLER, sizeof(INT10H_HANDLER));
	Status = ShimVesaInformation(VGA_ROM_ADDRESS, &Int10hHandlerAddress);
	if (EFI_ERROR(Status))
	{
		PrintError(L"VESA information could not be filled in, aborting\r\n");
		return Status;
	}
	NewInt10hHandlerEntry.Segment = (UINT16)((UINT32)VGA_ROM_ADDRESS >> 4);
	NewInt10hHandlerEntry.Offset  = (UINT16)(Int10hHandlerAddress - VGA_ROM_ADDRESS);
	PrintDebug(L"VESA information filled in, Int10h handler address=%x (%04x:%04x)\r\n",
		Int10hHandlerAddress, NewInt10hHandlerEntry.Segment, NewInt10hHandlerEntry.Offset);

	Status = EnsureMemoryLock (VGA_ROM_ADDRESS, (UINT32)VGA_ROM_SIZE, LOCK);
	if (EFI_ERROR(Status))
	{
		PrintDebug (L"Unable to lock VGA ROM memory at %x but this is not essential\n",
		VGA_ROM_ADDRESS);
  	}

	IvtInt10hHandlerEntry = (IVT_ENTRY *)IVT_ADDRESS + 0x10;
	if (!EFI_ERROR(IvtAllocationStatus))
	{
		IvtInt10hHandlerEntry->Segment = NewInt10hHandlerEntry.Segment;
		IvtInt10hHandlerEntry->Offset = NewInt10hHandlerEntry.Offset;
		PrintDebug(L"Int10h IVT entry modified to point at %04x:%04x\n",
		IvtInt10hHandlerEntry->Segment, IvtInt10hHandlerEntry->Offset);
	}
	else if (IvtInt10hHandlerEntry->Segment == NewInt10hHandlerEntry.Segment
		&& IvtInt10hHandlerEntry->Offset == NewInt10hHandlerEntry.Offset)
	{
		PrintDebug(L"Int10h IVT entry could not be modified but already pointing at %04x:%04x\n",
		IvtInt10hHandlerEntry->Segment, IvtInt10hHandlerEntry->Offset);
	}
	else
	{
		PrintError(L"Unable to claim IVT area at %04x (error: %r)\n", IVT_ADDRESS, IvtAllocationStatus);
		PrintError(L"Int10h IVT entry could not be modified and currently poiting\n");
		PrintError(L"at a wrong memory area (%04x:%04x instead of %04x:%04x).\n",
		IvtInt10hHandlerEntry->Segment, IvtInt10hHandlerEntry->Offset,
		NewInt10hHandlerEntry.Segment, NewInt10hHandlerEntry.Offset);
		return IvtAllocationStatus;
	}

	if (IsInt10hHandlerDefined())
	{
		PrintDebug(L"Pre-boot Int10h sanity check success\n");
		return EFI_SUCCESS;
	}
	else
	{
		PrintError(L"Pre-boot Int10h sanity check failed\n");
		return EFI_ABORTED;
	}
}

EFI_STATUS
EFIAPI
UefiMain(
	IN EFI_HANDLE ImageHandle,
	IN EFI_SYSTEM_TABLE* SystemTable
	)
{
	//
	// Install INT 10H handler.
	//
	CONST EFI_STATUS Int10hStatus = InstallInt10hHandler();
	if (EFI_ERROR(Int10hStatus))
	{
		Print(L"\r\nERROR: Installing the Int10h handler failed with status %llx (%r).\r\n"
			L"Press any key to continue, or press ESC to return to the firmware or shell.\r\n",
			Int10hStatus, Int10hStatus);
		if (WaitForKey() == SCAN_ESC)
		{
			gBS->Exit(gImageHandle, Int10hStatus, 0, NULL);
			return Int10hStatus;
		}
	}

	//
	// Connect all drivers to all controllers
	//
	EfiBootManagerConnectAll();

	//
	// Set the highest available console mode and clear the screen
	//
	SetHighestAvailableTextMode();

	//
	// Turn off the watchdog timer
	//
	gBS->SetWatchdogTimer(0, 0, 0, NULL);

	//
	// Query the console input handle for the Simple Text Input Ex protocol
	//
	gBS->HandleProtocol(gST->ConsoleInHandle, &gEfiSimpleTextInputExProtocolGuid, (VOID **)&mTextInputEx);

	//
	// Allow user to configure the driver by pressing a hotkey
	//
	Print(L"Press <HOME> to configure EfiGuard...\r\n");
	CONST BOOLEAN InteractiveConfiguration = WaitForKeyWithTimeout(1500) == SCAN_HOME;

	//
	// Locate, load, start and configure the driver
	//
	CONST EFI_STATUS DriverStatus = StartEfiGuard(InteractiveConfiguration);
	if (EFI_ERROR(DriverStatus))
	{
		Print(L"\r\nERROR: driver load failed with status %llx (%r).\r\n"
			L"Press any key to continue, or press ESC to return to the firmware or shell.\r\n",
			DriverStatus, DriverStatus);
		if (WaitForKey() == SCAN_ESC)
		{
			gBS->Exit(gImageHandle, DriverStatus, 0, NULL);
			return DriverStatus;
		}
	}

	//
	// Start the "boot through" procedure to boot Windows.
	//
	// First obtain our own boot option number, since we don't want to boot ourselves again
	UINT16 CurrentBootOptionIndex;
	UINT32 Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
	UINTN Size = sizeof(CurrentBootOptionIndex);
	CONST EFI_STATUS Status = gRT->GetVariable(EFI_BOOT_CURRENT_VARIABLE_NAME,
												&gEfiGlobalVariableGuid,
												&Attributes,
												&Size,
												&CurrentBootOptionIndex);
	if (EFI_ERROR(Status))
	{
		CurrentBootOptionIndex = 0xFFFF;
		Print(L"WARNING: failed to query the current boot option index variable.\r\n"
			L"This could lead to the current device being booted recursively.\r\n"
			L"If you booted from a removable device, it is recommended that you remove it now.\r\n"
			L"\r\nPress any key to continue...\r\n");
		WaitForKey();
	}

	// Query all boot options, and try each following the order set in the "BootOrder" variable, except
	// (1) Do not boot ourselves again, and
	// (2) The description or filename must indicate the boot option is some form of Windows.
	UINTN BootOptionCount;
	EFI_BOOT_MANAGER_LOAD_OPTION* BootOptions = EfiBootManagerGetLoadOptions(&BootOptionCount, LoadOptionTypeBoot);
	BOOLEAN BootSuccess = TryBootOptionsInOrder(BootOptions,
												BootOptionCount,
												CurrentBootOptionIndex,
												TRUE);
	if (!BootSuccess)
	{
		// We did not find any Windows boot entry; retry without the "must be Windows" restriction.
		BootSuccess = TryBootOptionsInOrder(BootOptions,
											BootOptionCount,
											CurrentBootOptionIndex,
											FALSE);
	}
	EfiBootManagerFreeLoadOptions(BootOptions, BootOptionCount);

	if (BootSuccess)
		return EFI_SUCCESS;

	// We should never reach this unless something is seriously wrong (no boot device / partition table corrupted / catastrophic boot manager failure...)
	Print(L"Failed to boot anything. This is super bad!\r\n"
		L"Press any key to return to the firmware or shell,\r\nwhich will surely fix this and not make things worse.\r\n");
	WaitForKey();

	gBS->Exit(gImageHandle, EFI_SUCCESS, 0, NULL);

	return EFI_SUCCESS;
}
