// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Minimal Qualcomm USB device-mode bulk log transport.
//
// The host sees VID:PID 18d1:d00d with a vendor-specific interface and one
// bulk-IN endpoint (0x81). This first stage proves enumeration and polling on
// the dipper DWC3 controller; XNU-side endpoint takeover is a separate stage.

#include <Uefi.h>
#include <IndustryStandard/Usb.h>
#define USB_BOS_DESCRIPTOR QCOM_USB_BOS_DESCRIPTOR
#define USB_INTERFACE_ASSOCIATION_DESCRIPTOR QCOM_USB_INTERFACE_ASSOCIATION_DESCRIPTOR
#define USB_DESC_TYPE_INTERFACE_ASSOCIATION QCOM_USB_DESC_TYPE_INTERFACE_ASSOCIATION
#define USB_DESC_TYPE_BOS QCOM_USB_DESC_TYPE_BOS
#include <Guid/EventGroup.h>
#include <Protocol/EFIUsbDevice.h>
#undef USB_BOS_DESCRIPTOR
#undef USB_INTERFACE_ASSOCIATION_DESCRIPTOR
#undef USB_DESC_TYPE_INTERFACE_ASSOCIATION
#undef USB_DESC_TYPE_BOS
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define LOG_ENDPOINT      0x81U
#define LOG_PACKET_BYTES  256U

#pragma pack(1)
typedef struct {
  USB_CONFIG_DESCRIPTOR     Config;
  USB_INTERFACE_DESCRIPTOR  Interface;
  USB_ENDPOINT_DESCRIPTOR   In;
} DIPPER_LOG_DESCRIPTORS;
#pragma pack()

STATIC USB_DEVICE_DESCRIPTOR mDeviceDescriptor = {
  18, 1, 0x0200, 0, 0, 0, 64, 0x18D1, 0xD00D, 0x0100, 0, 0, 0, 1
};

STATIC DIPPER_LOG_DESCRIPTORS mDescriptors = {
  { 9, 2, sizeof(DIPPER_LOG_DESCRIPTORS), 1, 1, 0, 0x80, 50 },
  { 9, 4, 0, 0, 1, 0xFF, 0x55, 0x4C, 0 },
  { 7, 5, LOG_ENDPOINT, 2, 512, 0 }
};

STATIC USB_DEVICE_QUALIFIER_DESCRIPTOR mQualifier = {
  10, 6, 0x0200, 0, 0, 0, 64, 1, 0
};

STATIC USB_STRING_DESCRIPTOR mString0 = {
  4, 3, { 0x0409 }
};

STATIC CONST UINT16 mProductChars[] = {
  'D', 'i', 'p', 'p', 'e', 'r', 'U', 'S', 'B', 'L', 'o', 'g'
};

STATIC struct {
  UINT8  Length;
  UINT8  DescriptorType;
  CHAR16 String[ARRAY_SIZE(mProductChars)];
} mProductString = {
  sizeof(UINT8) * 2 + sizeof(mProductChars),
  3,
  {}
};

STATIC EFI_USB_DEVICE_PROTOCOL *mUsb;
STATIC EFI_EVENT                 mPollEvent;
STATIC EFI_EVENT                 mExitEvent;
STATIC VOID                     *mTxBuffer;
STATIC BOOLEAN                   mConnected;
STATIC BOOLEAN                   mTxPending;
STATIC UINT32                    mSequence;

STATIC EFI_GUID mUsbConfigGuid = {
  0xE722B03F, 0xB250, 0x42CE,
  { 0x8E, 0xBD, 0x5B, 0xD5, 0x18, 0x12, 0xD0, 0x37 }
};

typedef EFI_STATUS (EFIAPI *USB_CONFIG_START_STOP)(
  IN VOID  *This,
  IN UINT32 CoreNumber,
  IN UINT32 Mode
  );

typedef struct {
  UINT32               Revision;
  VOID                 *GetCoreBaseAddr;
  VOID                 *ConfigUsb;
  VOID                 *ResetUsb;
  VOID                 *GetUsbFnConfig;
  VOID                 *GetSSUsbFnConfig;
  VOID                 *GetUsbFnConnStatus;
  VOID                 *GetUsbHostConfig;
  VOID                 *GetUsbMaxHostCoreNum;
  VOID                 *ExitUsbLibServices;
  USB_CONFIG_START_STOP StartController;
  USB_CONFIG_START_STOP StopController;
} USB_CONFIG_LAYOUT;

STATIC
VOID
EFIAPI
PollUsb (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  EFI_STATUS             Status;
  USB_DEVICE_EVENT       Kind;
  USB_DEVICE_EVENT_DATA  Data;
  UINTN                  Size;
  UINTN                  Length;
  UINTN                  Index;

  (VOID)Event;
  (VOID)Context;
  if ((mUsb == NULL) || !mConnected) {
    return;
  }

  for (Index = 0; Index < 8; ++Index) {
    Kind = UsbDeviceEventNoEvent;
    Size = sizeof(Data);
    ZeroMem(&Data, sizeof(Data));
    Status = mUsb->HandleEvent(&Kind, &Size, &Data);
    if (EFI_ERROR(Status) || (Kind == UsbDeviceEventNoEvent)) {
      break;
    }
    if ((Kind == UsbDeviceEventDeviceStateChange) &&
        (Size >= sizeof(Data.DeviceState)) &&
        (Data.DeviceState == UsbDeviceStateDisconnected)) {
      mConnected = FALSE;
      mTxPending = FALSE;
      return;
    }
    if ((Kind == UsbDeviceEventTransferNotification) &&
        (Size >= sizeof(Data.TransferOutcome)) &&
        (Data.TransferOutcome.DataBuffer == mTxBuffer) &&
        (Data.TransferOutcome.EndpointIndex == LOG_ENDPOINT)) {
      mTxPending = FALSE;
    }
  }

  if (mTxPending) {
    return;
  }
  Length = AsciiSPrint(
             (CHAR8 *)mTxBuffer,
             LOG_PACKET_BYTES,
             "DIPPER-USB-LOG UEFI-alive seq=%u endpoint=0x81\r\n",
             ++mSequence
             );
  Status = mUsb->Send(LOG_ENDPOINT, Length, mTxBuffer);
  if (!EFI_ERROR(Status)) {
    mTxPending = TRUE;
  }
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  (VOID)Event;
  (VOID)Context;
  if (mPollEvent != NULL) {
    gBS->SetTimer(mPollEvent, TimerCancel, 0);
  }
  // Deliberately do not Stop() the controller: the next stage checks whether
  // UsbfnDwc3's own ExitBootServices callback leaves the endpoint usable.
}

EFI_STATUS
EFIAPI
DipperUsbLogEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  USB_CONFIG_LAYOUT     *UsbConfig;
  VOID                  *Configurations[1];
  USB_STRING_DESCRIPTOR *Strings[2];
  UINTN                  Index;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  UsbConfig = NULL;
  Status = gBS->LocateProtocol(&mUsbConfigGuid, NULL, (VOID **)&UsbConfig);
  if (!EFI_ERROR(Status) && (UsbConfig != NULL) && (UsbConfig->StopController != NULL) &&
      (UsbConfig->StartController != NULL)) {
    UsbConfig->StopController(UsbConfig, 0, 4);
    gBS->Stall(100000);
    UsbConfig->StartController(UsbConfig, 0, 4);
    gBS->Stall(200000);
  }

  mUsb = NULL;
  Status = gBS->LocateProtocol(&gEfiUsbDeviceProtocolGuid, NULL, (VOID **)&mUsb);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "DipperUsbLog: UsbDevice protocol unavailable: %r\n", Status));
    return EFI_SUCCESS;
  }

  for (Index = 0; Index < ARRAY_SIZE(mProductChars); ++Index) {
    mProductString.String[Index] = mProductChars[Index];
  }
  Configurations[0] = &mDescriptors;
  Strings[0] = &mString0;
  Strings[1] = (USB_STRING_DESCRIPTOR *)&mProductString;

  // The ABL/fastboot stack may still own the controller when this DXE starts.
  // Stop that session before advertising the diagnostic interface.
  mUsb->Stop();
  gBS->Stall(100000);
  Status = mUsb->Start(
                   &mDeviceDescriptor,
                   Configurations,
                   &mQualifier,
                   NULL,
                   ARRAY_SIZE(Strings),
                   Strings
                   );
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "DipperUsbLog: controller start failed: %r\n", Status));
    return EFI_SUCCESS;
  }
  Status = mUsb->AllocateTransferBuffer(LOG_PACKET_BYTES, &mTxBuffer);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "DipperUsbLog: transfer buffer failed: %r\n", Status));
    mUsb->Stop();
    return EFI_SUCCESS;
  }

  Status = gBS->CreateEvent(
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  PollUsb,
                  NULL,
                  &mPollEvent
                  );
  if (EFI_ERROR(Status)) {
    return EFI_SUCCESS;
  }
  Status = gBS->SetTimer(mPollEvent, TimerPeriodic, 500000);
  if (EFI_ERROR(Status)) {
    return EFI_SUCCESS;
  }
  Status = gBS->CreateEventEx(
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitEvent
                  );
  if (EFI_ERROR(Status)) {
    return EFI_SUCCESS;
  }

  mConnected = TRUE;
  DEBUG((DEBUG_WARN, "DipperUsbLog: ready vid=18d1 pid=d00d endpoint=0x81\n"));
  return EFI_SUCCESS;
}
