/* Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * * Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 *  with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "VerifiedBoot.h"
#include "BootLinux.h"
#include "KeymasterClient.h"
#include "libavb/libavb.h"
#include <Library/MenuKeysDetection.h>
#include <Library/VerifiedBootMenu.h>
#include <Library/LEOEMCertificate.h>

STATIC CONST CHAR8 *VerityMode = " androidboot.veritymode=";
STATIC CONST CHAR8 *VerifiedState = " androidboot.verifiedbootstate=";
STATIC CONST CHAR8 *KeymasterLoadState = " androidboot.keymaster=1";
STATIC CONST CHAR8 *DmVerityCmd = " root=/dev/dm-0 dm=\"system none ro,0 1 "
                                    "android-verity";
STATIC CONST CHAR8 *Space = " ";

STATIC struct verified_boot_verity_mode VbVm[] = {
    {FALSE, "logging"},
    {TRUE, "enforcing"},
};

STATIC struct verified_boot_state_name VbSn[] = {
    {GREEN, "green"},
    {ORANGE, "orange"},
    {YELLOW, "yellow"},
    {RED, "red"},
};

struct boolean_string {
  BOOLEAN value;
  CHAR8 *name;
};

STATIC struct boolean_string BooleanString[] = {
    {FALSE, "false"},
    {TRUE, "true"}
};

typedef struct {
  AvbOps *Ops;
  AvbSlotVerifyData *SlotData;
} VB2Data;

UINT32
GetAVBVersion ()
{
#if VERIFIED_BOOT_LE
  return AVB_LE;
#elif VERIFIED_BOOT_2
  return AVB_2;
#elif VERIFIED_BOOT
  return AVB_1;
#else
  return NO_AVB;
#endif
}

BOOLEAN
VerifiedBootEnbled ()
{
  return (GetAVBVersion () > NO_AVB);
}

STATIC EFI_STATUS
AppendVBCmdLine (BootInfo *Info, CONST CHAR8 *Src)
{
  EFI_STATUS Status = EFI_SUCCESS;
  INT32 SrcLen = AsciiStrLen (Src);
  CHAR8 *Dst = Info->VBCmdLine + Info->VBCmdLineFilledLen;
  INT32 DstLen = Info->VBCmdLineLen - Info->VBCmdLineFilledLen;

  GUARD (AsciiStrnCatS (Dst, DstLen, Src, SrcLen));
  Info->VBCmdLineFilledLen += SrcLen;

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
AppendVBCommonCmdLine (BootInfo *Info)
{
  EFI_STATUS Status = EFI_SUCCESS;

  if (Info->VbIntf->Revision >= QCOM_VERIFIEDBOOT_PROTOCOL_REVISION) {
    GUARD (AppendVBCmdLine (Info, VerifiedState));
    GUARD (AppendVBCmdLine (Info, VbSn[Info->BootState].name));
  }
  GUARD (AppendVBCmdLine (Info, KeymasterLoadState));
  GUARD (AppendVBCmdLine (Info, Space));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
NoAVBLoadDtboImage (BootInfo *Info, VOID **DtboImage,
        UINT32 *DtboSize, CHAR16 *Pname)
{
  EFI_STATUS Status = EFI_SUCCESS;
  Slot CurrentSlot;

  *DtboSize = (UINT32) DTBO_MAX_SIZE_ALLOWED;
  *DtboImage = AllocatePool (DTBO_MAX_SIZE_ALLOWED);
  GUARD ( StrnCpyS (Pname,
              (UINTN)MAX_GPT_NAME_SIZE,
              (CONST CHAR16 *)L"dtbo",
              StrLen (L"dtbo")));

  if (Info->MultiSlotBoot) {
      CurrentSlot = GetCurrentSlotSuffix ();
      GUARD ( StrnCatS (Pname, MAX_GPT_NAME_SIZE,
                  CurrentSlot.Suffix, StrLen (CurrentSlot.Suffix)));
  }
  Status = LoadImageFromPartition (*DtboImage, DtboSize, Pname);
  return Status;
}

STATIC EFI_STATUS
VBCommonInit (BootInfo *Info)
{
  EFI_STATUS Status = EFI_SUCCESS;

  Info->BootState = RED;

  Status = gBS->LocateProtocol (&gEfiQcomVerifiedBootProtocolGuid, NULL,
                                (VOID **)&(Info->VbIntf));
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Unable to locate VB protocol: %r\n", Status));
    return Status;
  }

  return Status;
}

/*
 * Ensure Info->Pname is already updated before this function is called.
 * If Command line already has "root=", return TRUE, else FALSE.
 */
STATIC EFI_STATUS
VBAllocateCmdLine (BootInfo *Info)
{
  EFI_STATUS Status = EFI_SUCCESS;

  /* allocate VB command line*/
  Info->VBCmdLine = AllocatePool (DTB_PAD_SIZE);
  if (Info->VBCmdLine == NULL) {
    DEBUG ((EFI_D_ERROR, "VB CmdLine allocation failed!\n"));
    Status = EFI_OUT_OF_RESOURCES;
    return Status;
  }
  Info->VBCmdLineLen = DTB_PAD_SIZE;
  Info->VBCmdLineFilledLen = 0;
  Info->VBCmdLine[Info->VBCmdLineFilledLen] = '\0';

  return Status;
}

STATIC
BOOLEAN
IsRootCmdLineUpdated (BootInfo *Info)
{
  CHAR8* ImageCmdLine = NULL;

  ImageCmdLine =
    (CHAR8*) & (((boot_img_hdr*) (Info->Images[0].ImageBuffer))->cmdline[0]);

  ImageCmdLine[BOOT_ARGS_SIZE - 1] = '\0';
  if (AsciiStrStr (ImageCmdLine, "root=")) {
    return TRUE;
  } else {
    return FALSE;
  }
}


STATIC EFI_STATUS
LoadImageNoAuth (BootInfo *Info)
{
  EFI_STATUS Status = EFI_SUCCESS;
  CHAR16 Pname[MAX_GPT_NAME_SIZE];

  if (Info->Images[0].ImageBuffer != NULL && Info->Images[0].ImageSize > 0) {
    /* fastboot boot option, boot image is already loaded, check for dtbo */
    goto load_dtbo;
  }

  Status = LoadImage (Info->Pname, (VOID **)&(Info->Images[0].ImageBuffer),
                      (UINT32 *)&(Info->Images[0].ImageSize));
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "ERROR: Failed to load image from partition: %r\n",
            Status));
    return EFI_LOAD_ERROR;
  }
  Info->NumLoadedImages = 1;
  Info->Images[0].Name = AllocatePool (StrLen (Info->Pname) + 1);
  UnicodeStrToAsciiStr (Info->Pname, Info->Images[0].Name);


load_dtbo:
  /*load dt overlay when avb is disabled*/
  Status = NoAVBLoadDtboImage (Info, (VOID **)&(Info->Images[1].ImageBuffer),
          (UINT32 *)&(Info->Images[1].ImageSize), Pname);
  if (Status == EFI_NO_MEDIA) {
      DEBUG ((EFI_D_ERROR, "No dtbo partition is found, Skip dtbo\n"));
      FreePool (Info->Images[1].ImageBuffer);
      return EFI_SUCCESS;
  }
  else if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR,
                  "ERROR: Failed to load dtbo from partition: %r\n", Status));
      FreePool (Info->Images[1].ImageBuffer);
      return EFI_LOAD_ERROR;
  }
  Info-> NumLoadedImages = 2;
  Info-> Images[1].Name = AllocatePool (StrLen (Pname) + 1);
  UnicodeStrToAsciiStr (Pname, Info->Images[1].Name);

  return Status;
}

STATIC EFI_STATUS
LoadImageNoAuthWrapper (BootInfo *Info)
{
  EFI_STATUS Status = EFI_SUCCESS;
  CHAR8 *SystemPath = NULL;
  UINT32 SystemPathLen = 0;

  GUARD (VBAllocateCmdLine (Info));
  GUARD (LoadImageNoAuth (Info));

  if (!IsRootCmdLineUpdated (Info)) {
    SystemPathLen = GetSystemPath (&SystemPath);
    if (SystemPathLen == 0 || SystemPath == NULL) {
      DEBUG ((EFI_D_ERROR, "GetSystemPath failed!\n"));
      return EFI_LOAD_ERROR;
    }
    GUARD (AppendVBCmdLine (Info, SystemPath));
  }

  return Status;
}

STATIC EFI_STATUS
LoadImageAndAuthVB1 (BootInfo *Info)
{
  EFI_STATUS Status = EFI_SUCCESS;
  CHAR8 StrPnameAscii[MAX_GPT_NAME_SIZE]; /* partition name starting with
                                             / and no suffix */
  CHAR8 PnameAscii[MAX_GPT_NAME_SIZE];
  CHAR8 *SystemPath = NULL;
  UINT32 SystemPathLen = 0;
  CHAR8 *Temp = NULL;

  GUARD (VBCommonInit (Info));
  GUARD (VBAllocateCmdLine (Info));
  GUARD (LoadImageNoAuth (Info));

  device_info_vb_t DevInfo_vb;
  DevInfo_vb.is_unlocked = IsUnlocked ();
  DevInfo_vb.is_unlock_critical = IsUnlockCritical ();
  Status = Info->VbIntf->VBDeviceInit (Info->VbIntf,
                                       (device_info_vb_t *)&DevInfo_vb);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error during VBDeviceInit: %r\n", Status));
    return Status;
  }

  AsciiStrnCpyS (StrPnameAscii, ARRAY_SIZE (StrPnameAscii), "/",
                 AsciiStrLen ("/"));
  UnicodeStrToAsciiStr (Info->Pname, PnameAscii);
  if (Info->MultiSlotBoot) {
    AsciiStrnCatS (StrPnameAscii, ARRAY_SIZE (StrPnameAscii), PnameAscii,
                   AsciiStrLen (PnameAscii) - (MAX_SLOT_SUFFIX_SZ - 1));
  } else {
    AsciiStrnCatS (StrPnameAscii, ARRAY_SIZE (StrPnameAscii), PnameAscii,
                   AsciiStrLen (PnameAscii));
  }

  Status =
      Info->VbIntf->VBVerifyImage (Info->VbIntf, (UINT8 *)StrPnameAscii,
                                   (UINT8 *)Info->Images[0].ImageBuffer,
                                   Info->Images[0].ImageSize, &Info->BootState);
  if (Status != EFI_SUCCESS || Info->BootState == BOOT_STATE_MAX) {
    DEBUG ((EFI_D_ERROR, "VBVerifyImage failed with: %r\n", Status));
    return Status;
  }

  Status = Info->VbIntf->VBSendRot (Info->VbIntf);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error sending Rot : %r\n", Status));
    return Status;
  }

  if (!IsRootCmdLineUpdated (Info)) {
    SystemPathLen = GetSystemPath (&SystemPath);
    if (SystemPathLen == 0 || SystemPath == NULL) {
      DEBUG ((EFI_D_ERROR, "GetSystemPath failed!\n"));
      return EFI_LOAD_ERROR;
    }
    GUARD (AppendVBCmdLine (Info, DmVerityCmd));
    /* Copy only the portion after "root=" in the SystemPath */
    Temp = AsciiStrStr (SystemPath, "root=");
    if (Temp) {
      CopyMem (Temp, SystemPath + AsciiStrLen ("root=") + 1,
          SystemPathLen - AsciiStrLen ("root=") - 1);
      SystemPath[SystemPathLen - AsciiStrLen ("root=")] = '\0';
    }

    GUARD (AppendVBCmdLine (Info, SystemPath));
    GUARD (AppendVBCmdLine (Info, "\""));
  }
  GUARD (AppendVBCommonCmdLine (Info));
  GUARD (AppendVBCmdLine (Info, VerityMode));
  GUARD (AppendVBCmdLine (Info, VbVm[IsEnforcing ()].name));

  Info->VBData = NULL;
  return Status;
}

STATIC BOOLEAN
ResultShouldContinue (AvbSlotVerifyResult Result)
{
  switch (Result) {
  case AVB_SLOT_VERIFY_RESULT_ERROR_OOM:
  case AVB_SLOT_VERIFY_RESULT_ERROR_IO:
  case AVB_SLOT_VERIFY_RESULT_ERROR_INVALID_METADATA:
  case AVB_SLOT_VERIFY_RESULT_ERROR_UNSUPPORTED_VERSION:
  case AVB_SLOT_VERIFY_RESULT_ERROR_INVALID_ARGUMENT:
    return FALSE;

  case AVB_SLOT_VERIFY_RESULT_OK:
  case AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION:
  case AVB_SLOT_VERIFY_RESULT_ERROR_ROLLBACK_INDEX:
  case AVB_SLOT_VERIFY_RESULT_ERROR_PUBLIC_KEY_REJECTED:
    return TRUE;
  }

  return FALSE;
}

STATIC EFI_STATUS
LEGetImageHash (QcomAsn1x509Protocol *pEfiQcomASN1X509Protocol,
        VB_HASH HashAlgorithm,
        UINT8 *Img, UINTN ImgSize,
        UINT8 *ImgHash, UINTN HashSize)
{
    EFI_STATUS Status = EFI_FAILURE;
    EFI_GUID *HashAlgorithmGuid;
    UINTN DigestSize = 0;
    EFI_HASH2_OUTPUT Hash2Output;
    EFI_HASH2_PROTOCOL *pEfiHash2Protocol = NULL;

    if (pEfiQcomASN1X509Protocol == NULL ||
        Img == NULL ||
        ImgHash == NULL) {
        DEBUG ((EFI_D_ERROR,
                "LEGetRSAPublicKeyInfoFromCertificate: Invalid pointer\n"));
        return EFI_INVALID_PARAMETER;
    }

    switch (HashAlgorithm) {
    case VB_SHA256:
        HashAlgorithmGuid = &gEfiHashAlgorithmSha256Guid;
        break;
    default:
        DEBUG ((EFI_D_ERROR,
            "VB: LEGetImageHash: not supported algorithm:%d\n", HashAlgorithm));
        Status = EFI_UNSUPPORTED;
        goto exit;
    }

    Status = gBS->LocateProtocol (&gEfiHash2ProtocolGuid,
                 NULL, (VOID **)&pEfiHash2Protocol);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR,
        "VB:LEGetImageHash: LocateProtocol unsuccessful!Status: %r\n", Status));
        goto exit;
    }

    Status = pEfiHash2Protocol->GetHashSize (pEfiHash2Protocol,
                HashAlgorithmGuid, &DigestSize);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR,
         "VB: LEGetImageHash: GetHashSize unsuccessful! Status: %r\n", Status));
        goto exit;
    }
    if (HashSize != DigestSize) {
        DEBUG ((EFI_D_ERROR,
            "VB: LEGetImageHash: Invalid size! HashSize: %d, DigestSize: %d\n"
            , HashSize, DigestSize));
        Status = EFI_FAILURE;
        goto exit;
    }
    Status = pEfiHash2Protocol->HashInit (pEfiHash2Protocol, HashAlgorithmGuid);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR,
            "VB: LEGetImageHash: HashInit unsuccessful! Status: %r\n", Status));
        goto exit;
    }
    Status = pEfiHash2Protocol->HashUpdate (pEfiHash2Protocol, Img, ImgSize);
    if (EFI_SUCCESS != Status) {

        DEBUG ((EFI_D_ERROR,
            "VB: LEGetImageHash: HashUpdate unsuccessful(Img)!Status: %r\n"
            , Status));
        goto exit;
    }
    Status = pEfiHash2Protocol->HashFinal (pEfiHash2Protocol, &Hash2Output);
    if (EFI_SUCCESS != Status) {

        DEBUG ((EFI_D_ERROR,
        "VB: LEGetImageHash: HashFinal unsuccessful! Status: %r\n", Status));
        goto exit;
    }
    gBS->CopyMem ((VOID *)ImgHash, (VOID *)&Hash2Output, DigestSize);
    Status = EFI_SUCCESS;

exit:
    return Status;
}

STATIC EFI_STATUS LEGetRSAPublicKeyInfoFromCertificate (
                QcomAsn1x509Protocol *pEfiQcomASN1X509Protocol,
                CERTIFICATE *Certificate,
                secasn1_data_type *Modulus,
                secasn1_data_type *PublicExp,
                UINT32 *PaddingType)
{
    EFI_STATUS Status = EFI_FAILURE;
    RSA RsaKey = {NULL};

    if (pEfiQcomASN1X509Protocol == NULL ||
        Certificate == NULL ||
        Modulus == NULL ||
        PublicExp == NULL ||
        PaddingType == NULL) {
        DEBUG ((EFI_D_ERROR,
                "LEGetRSAPublicKeyInfoFromCertificate: Invalid pointer\n"));
        return EFI_INVALID_PARAMETER;
    }

    Status = pEfiQcomASN1X509Protocol->ASN1X509GetRSAFromCert
                    (pEfiQcomASN1X509Protocol, Certificate, &RsaKey);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR,
            "VB: ASN1X509GetRSAFromCert unsuccessful! Status : %r\n", Status));
        goto exit;
    }
    Status = pEfiQcomASN1X509Protocol->ASN1X509GetKeymaterial
            (pEfiQcomASN1X509Protocol, &RsaKey, Modulus, PublicExp);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR,
            "VB: ASN1X509GetKeymaterial unsuccessful! Status: %r\n", Status));
        goto exit;
    }
    *PaddingType = CE_RSA_PAD_PKCS1_V1_5_SIG;
exit:
    return Status;
}
STATIC EFI_STATUS LEVerifyHashWithRSASignature (
                UINT8 *ImgHash,
                VB_HASH HashAlgorithm,
                secasn1_data_type *Modulus,
                secasn1_data_type *PublicExp,
                UINT32 PaddingType,
                CONST UINT8 *SignaturePtr,
                UINT32 SignatureLen)
{
    EFI_STATUS Status = EFI_FAILURE;
    CE_RSA_KEY Key = {0};
    BigInt ModulusBi;
    BigInt PublicExpBi;
    INT32 HashIdx;
    INT32 HashLen;
    VOID *PaddingInfo = NULL;
    QcomSecRsaProtocol *pEfiQcomSecRSAProtocol = NULL;
    SetMem (&Key, sizeof (CE_RSA_KEY), 0);

    if (ImgHash == NULL ||
        Modulus == NULL ||
        PublicExp == NULL ||
        SignaturePtr == NULL) {
        DEBUG ((EFI_D_ERROR, "LEVerifyHashWithRSASignature:Invalid pointer\n"));
        return EFI_INVALID_PARAMETER;
    }

    switch (HashAlgorithm) {
    case VB_SHA256:
        HashIdx = CE_HASH_IDX_SHA256;
        HashLen = VB_SHA256_SIZE;
        break;
    default:
        DEBUG ((EFI_D_ERROR,
                "VB: LEVerifySignature: Hash algorithm not supported\n"));
        Status = EFI_UNSUPPORTED;
        goto exit;
    }

    Key.N = AllocatePool (sizeof (S_BIGINT));
    if (Key.N == NULL) {
        DEBUG ((EFI_D_ERROR,
                "VB: LEVerifySignature: mem allocation err for Key.N\n"));
        goto exit;
    }
    Key.e = AllocatePool (sizeof (S_BIGINT));
    if (Key.e == NULL) {
        DEBUG ((EFI_D_ERROR,
                "VB: LEVerifySignature: mem allocation err for Key.e\n"));
        goto exit;
    }
    Status = gBS->LocateProtocol (&gEfiQcomSecRSAProtocolGuid,
                NULL, (VOID **) &pEfiQcomSecRSAProtocol);
    if ( Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR,
        "VB: LEVerifySignature: LocateProtocol failed, Status: %r\n", Status));
        goto exit;
    }

    Status = pEfiQcomSecRSAProtocol->SecRSABigIntReadBin (
            pEfiQcomSecRSAProtocol, Modulus->data, Modulus->Len, &ModulusBi);
    if ( Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR,
                "VB: LEVerifySignature: SecRSABigIntReadBin for Modulus failed!"
                "Status: %r\n", Status));
        goto exit;
    }
    Status = pEfiQcomSecRSAProtocol->SecRSABigIntReadBin (
        pEfiQcomSecRSAProtocol, PublicExp->data, PublicExp->Len,  &PublicExpBi);
    if ( Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "VB: LEVerifySignature: SecRSABigIntReadBin for"
                        " Modulus failed! Status: %r\n", Status));
        goto exit;
    }

    Key.N->Bi = ModulusBi;
    Key.e->Bi = PublicExpBi;
    Key.e->Sign = S_BIGINT_POS;
    Key.Type = CE_RSA_KEY_PUBLIC;

    Status = pEfiQcomSecRSAProtocol->SecRSAVerifySig (pEfiQcomSecRSAProtocol,
                &Key, PaddingType,
                    PaddingInfo, HashIdx,
                    ImgHash, HashLen, (UINT8*)SignaturePtr, SignatureLen);

    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR,
        "VB: LEVerifySignature: SecRSAVerifySig failed! Status: %r\n", Status));
        goto exit;
    }

    DEBUG ((EFI_D_VERBOSE, "VB: LEVerifySignature: SecRSAVerifySig success!"
                      " Status: %r\n", Status));

    Status = EFI_SUCCESS;
exit:
    if (Key.N != NULL) {
        FreePool (Key.N);
    }
    if (Key.e != NULL) {
        FreePool (Key.e);
    }
    return Status;
}

STATIC EFI_STATUS LEVerifyHashWithSignature (
                    QcomAsn1x509Protocol *pEfiQcomASN1X509Protocol,
                    UINT8 *ImgHash, VB_HASH HashAlgorithm,
                    CERTIFICATE *Certificate,
                    CONST UINT8 *SignaturePtr,
                    UINT32 SignatureLen)
{
    EFI_STATUS Status = EFI_FAILURE;
    secasn1_data_type Modulus = {NULL};
    secasn1_data_type PublicExp = {NULL};
    UINT32 PaddingType = 0;

    if (pEfiQcomASN1X509Protocol == NULL ||
        ImgHash == NULL ||
        Certificate == NULL ||
        SignaturePtr == NULL) {
        DEBUG ((EFI_D_ERROR, "LEVerifyHashWithSignature: Invalid pointer\n"));
        return EFI_INVALID_PARAMETER;
    }

    /* TODO: get subject publick key info from certificate, implement new
                                                    algorithm in XBL*/
    /* XBL implemented by default sha256 and rsaEncryption with
                                                    PKCS1_V1_5 padding*/

    Status = LEGetRSAPublicKeyInfoFromCertificate (pEfiQcomASN1X509Protocol,
                Certificate, &Modulus, &PublicExp, &PaddingType);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "VB: LEGetRSAPublicKeyInfoFromCertificate "
                      "unsuccessful! Status: %r\n", Status));
        goto exit;
    }

    Status = LEVerifyHashWithRSASignature (ImgHash, HashAlgorithm,
                &Modulus, &PublicExp, PaddingType,
                SignaturePtr, SignatureLen);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "VB: LEVerifyHashWithSignature unsuccessful! "
                      "Status: %r\n", Status));
        goto exit;
    }
    Status = EFI_SUCCESS;
exit:
    return Status;
}


STATIC EFI_STATUS
LoadImageAndAuthVB2 (BootInfo *Info)
{
  EFI_STATUS Status = EFI_SUCCESS;
  AvbSlotVerifyResult Result;
  AvbSlotVerifyData *SlotData = NULL;
  VB2Data *VBData = NULL;
  AvbOpsUserData *UserData = NULL;
  AvbOps *Ops = NULL;
  CHAR8 PnameAscii[MAX_GPT_NAME_SIZE] = {0};
  CHAR8 *SlotSuffix = NULL;
  BOOLEAN AllowVerificationError = IsUnlocked ();
  CONST CHAR8 *RequestedPartitionMission[] = {"boot", "dtbo", NULL};
  CONST CHAR8 *RequestedPartitionRecovery[] = {"recovery", "dtbo", NULL};
  CONST CHAR8 *CONST *RequestedPartition = NULL;
  UINTN NumRequestedPartition = 0;
  UINT32 ImageHdrSize = 0;
  UINT32 PageSize = 0;
  UINT32 ImageSizeActual = 0;
  VOID *ImageBuffer = NULL;
  UINTN ImageSize = 0;
  KMRotAndBootState Data = {0};
  CONST boot_img_hdr *BootImgHdr = NULL;
  AvbSlotVerifyFlags VerifyFlags =
      AllowVerificationError ? AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR
                             : AVB_SLOT_VERIFY_FLAGS_NONE;
  AvbHashtreeErrorMode VerityFlags =
      AVB_HASHTREE_ERROR_MODE_RESTART_AND_INVALIDATE;

  Info->BootState = RED;
  GUARD (VBCommonInit (Info));
  GUARD (VBAllocateCmdLine (Info));

  UserData = avb_calloc (sizeof (AvbOpsUserData));
  if (UserData == NULL) {
    DEBUG ((EFI_D_ERROR, "ERROR: Failed to allocate AvbOpsUserData\n"));
    Status = EFI_OUT_OF_RESOURCES;
    goto out;
  }

  Ops = AvbOpsNew (UserData);
  if (Ops == NULL) {
    DEBUG ((EFI_D_ERROR, "ERROR: Failed to allocate AvbOps\n"));
    Status = EFI_OUT_OF_RESOURCES;
    goto out;
  }
  UserData->IsMultiSlot = Info->MultiSlotBoot;

  if (Info->MultiSlotBoot) {
    UnicodeStrToAsciiStr (Info->Pname, PnameAscii);
    if ((MAX_SLOT_SUFFIX_SZ + 1) > AsciiStrLen (PnameAscii)) {
      DEBUG ((EFI_D_ERROR, "ERROR: Can not determine slot suffix\n"));
      Status = EFI_INVALID_PARAMETER;
      goto out;
    }
    SlotSuffix = &PnameAscii[AsciiStrLen (PnameAscii) - MAX_SLOT_SUFFIX_SZ + 1];
  } else {
     SlotSuffix = "\0";
  }

  DEBUG ((EFI_D_VERBOSE, "Slot: %a, allow verification error: %a\n", SlotSuffix,
          BooleanString[AllowVerificationError].name));

  if ((!Info->MultiSlotBoot) &&
           Info->BootIntoRecovery) {
     RequestedPartition = RequestedPartitionRecovery;
     NumRequestedPartition = ARRAY_SIZE (RequestedPartitionRecovery) - 1;
     if (Info->NumLoadedImages) {
       /* fastboot boot option, skip Index 0, as boot image already
        * loaded */
       RequestedPartition = &RequestedPartitionRecovery[1];
     }
  } else {
     RequestedPartition = RequestedPartitionMission;
     NumRequestedPartition = ARRAY_SIZE (RequestedPartitionMission) - 1;
     if (Info->NumLoadedImages) {
       /* fastboot boot option, skip Index 0, as boot image already
        * loaded */
       RequestedPartition = &RequestedPartitionMission[1];
     }
  }

  if (Info->NumLoadedImages) {
    NumRequestedPartition--;
  }

  if (FixedPcdGetBool (AllowEio)) {
    VerityFlags = IsEnforcing () ? AVB_HASHTREE_ERROR_MODE_RESTART
                                 : AVB_HASHTREE_ERROR_MODE_EIO;
  } else {
    VerityFlags = AVB_HASHTREE_ERROR_MODE_RESTART_AND_INVALIDATE;
  }

  Result = avb_slot_verify (Ops, RequestedPartition, SlotSuffix, VerifyFlags,
                            VerityFlags, &SlotData);

  if (SlotData == NULL) {
    Status = EFI_LOAD_ERROR;
    Info->BootState = RED;
    goto out;
  }

  if (AllowVerificationError && ResultShouldContinue (Result)) {
    DEBUG ((EFI_D_ERROR, "State: Unlocked, AvbSlotVerify returned "
                         "%a, continue boot\n",
            avb_slot_verify_result_to_string (Result)));
  } else if (Result != AVB_SLOT_VERIFY_RESULT_OK) {
    DEBUG ((EFI_D_ERROR, "ERROR: Device State %a, AvbSlotVerify returned %a\n",
            AllowVerificationError ? "Unlocked" : "Locked",
            avb_slot_verify_result_to_string (Result)));
    Status = EFI_LOAD_ERROR;
    Info->BootState = RED;
    goto out;
  }

  for (UINTN ReqIndex = 0; ReqIndex < NumRequestedPartition; ReqIndex++) {
    DEBUG ((EFI_D_VERBOSE, "Requested Partition: %a\n",
            RequestedPartition[ReqIndex]));
    for (UINTN LoadedIndex = 0; LoadedIndex < SlotData->num_loaded_partitions;
         LoadedIndex++) {
      DEBUG ((EFI_D_VERBOSE, "Loaded Partition: %a\n",
              SlotData->loaded_partitions[LoadedIndex].partition_name));
      if (!AsciiStrnCmp (
              RequestedPartition[ReqIndex],
              SlotData->loaded_partitions[LoadedIndex].partition_name,
              AsciiStrLen (
                  SlotData->loaded_partitions[LoadedIndex].partition_name))) {
        if (Info->NumLoadedImages >= ARRAY_SIZE (Info->Images)) {
          DEBUG ((EFI_D_ERROR, "NumLoadedPartition"
                               "(%d) too large "
                               "max images(%d)\n",
                  Info->NumLoadedImages, ARRAY_SIZE (Info->Images)));
          Status = EFI_LOAD_ERROR;
          Info->BootState = RED;
          goto out;
        }
        Info->Images[Info->NumLoadedImages].Name =
            SlotData->loaded_partitions[LoadedIndex].partition_name;
        Info->Images[Info->NumLoadedImages].ImageBuffer =
            SlotData->loaded_partitions[LoadedIndex].data;
        Info->Images[Info->NumLoadedImages].ImageSize =
            SlotData->loaded_partitions[LoadedIndex].data_size;
        Info->NumLoadedImages++;
        break;
      }
    }
  }

  if (Info->NumLoadedImages < NumRequestedPartition) {
    DEBUG ((EFI_D_ERROR, "ERROR: AvbSlotVerify slot data error: num of "
                         "loaded partitions %d, requested %d\n",
            Info->NumLoadedImages, NumRequestedPartition));
    Status = EFI_LOAD_ERROR;
    goto out;
  }

  DEBUG ((EFI_D_VERBOSE, "Total loaded partition %d\n", Info->NumLoadedImages));

  VBData = (VB2Data *)avb_calloc (sizeof (VB2Data));
  if (VBData == NULL) {
    DEBUG ((EFI_D_ERROR, "ERROR: Failed to allocate VB2Data\n"));
    Status = EFI_OUT_OF_RESOURCES;
    goto out;
  }
  VBData->Ops = Ops;
  VBData->SlotData = SlotData;
  Info->VBData = (VOID *)VBData;

  GetPageSize (&ImageHdrSize);
  GUARD_OUT (GetImage (Info, &ImageBuffer, &ImageSize,
                    ((!Info->MultiSlotBoot) &&
                     Info->BootIntoRecovery) ?
                     "recovery" : "boot"));

  Status = CheckImageHeader (ImageBuffer, ImageHdrSize,
        &ImageSizeActual, &PageSize);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Invalid boot image header:%r\n", Status));
    goto out;
  }

  if (ImageSizeActual > ImageSize) {
    Status = EFI_BUFFER_TOO_SMALL;
    DEBUG ((EFI_D_ERROR, "Boot size in vbmeta less than actual boot image size "
                         "flash corresponding vbmeta.img\n"));
    goto out;
  }

  if (AllowVerificationError) {
    Info->BootState = ORANGE;
  } else {
    if (UserData->IsUserKey) {
      Info->BootState = YELLOW;
    } else {
      Info->BootState = GREEN;
    }
  }

  /* command line */
  GUARD_OUT (AppendVBCommonCmdLine (Info));
  GUARD_OUT (AppendVBCmdLine (Info, SlotData->cmdline));

  /* Set Rot & Boot State*/
  Data.Color = Info->BootState;
  Data. IsUnlocked = AllowVerificationError;
  Data.PublicKeyLength = UserData->PublicKeyLen;
  Data.PublicKey = UserData->PublicKey;

  BootImgHdr = (boot_img_hdr *)ImageBuffer;
  Data.SystemSecurityLevel = (BootImgHdr->os_version & 0x7FF);
  Data.SystemVersion = (BootImgHdr->os_version & 0xFFFFF800) >> 11;

  GUARD_OUT (KeyMasterSetRotAndBootState (&Data));

  DEBUG ((EFI_D_INFO, "VB2: Authenticate complete! boot state is: %a\n",
          VbSn[Info->BootState].name));

out:
  if (Status != EFI_SUCCESS) {
    if (SlotData != NULL) {
      avb_slot_verify_data_free (SlotData);
    }
    if (Ops != NULL) {
      AvbOpsFree (Ops);
    }
    if (UserData != NULL) {
      avb_free (UserData);
    }
    if (VBData != NULL) {
      avb_free (VBData);
    }
    Info->BootState = RED;
    if (Info->MultiSlotBoot) {
      HandleActiveSlotUnbootable ();
      /* HandleActiveSlotUnbootable should have swapped slots and
       * reboot the device. If no bootable slot found, enter fastboot
       */
      DEBUG ((EFI_D_WARN, "No bootable slots found enter fastboot mode\n"));
    } else {
       DEBUG ((EFI_D_WARN,
           "Non Multi-slot: Unbootable entering fastboot mode\n"));
    }
  }

  DEBUG ((EFI_D_ERROR, "VB2: boot state: %a(%d)\n", VbSn[Info->BootState].name,
          Info->BootState));
  return Status;
}

STATIC EFI_STATUS
DisplayVerifiedBootScreen (BootInfo *Info)
{
  EFI_STATUS Status = EFI_SUCCESS;
  CHAR8 FfbmStr[FFBM_MODE_BUF_SIZE] = {'\0'};

  if (GetAVBVersion () < AVB_1) {
    return EFI_SUCCESS;
  }

  if (!StrnCmp (Info->Pname, L"boot", StrLen (L"boot"))) {
    Status = GetFfbmCommand (FfbmStr, FFBM_MODE_BUF_SIZE);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_VERBOSE, "No Ffbm cookie found, ignore: %r\n", Status));
      FfbmStr[0] = '\0';
    }
  }

  DEBUG ((EFI_D_VERBOSE, "Boot State is : %d\n", Info->BootState));
  switch (Info->BootState) {
  case RED:
    Status = DisplayVerifiedBootMenu (DISPLAY_MENU_RED);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_INFO,
              "Your device is corrupt. It can't be trusted and will not boot."
              "\nYour device will shutdown in 30s\n"));
    }
    MicroSecondDelay (30000000);
    ShutdownDevice ();
    break;
  case YELLOW:
    Status = DisplayVerifiedBootMenu (DISPLAY_MENU_YELLOW);
    if (Status == EFI_SUCCESS) {
      WaitForExitKeysDetection ();
    } else {
      DEBUG ((EFI_D_INFO, "Your device has loaded a different operating system."
                          "\nWait for 5 seconds before proceeding\n"));
      MicroSecondDelay (5000000);
    }
    break;
  case ORANGE:
    if (FfbmStr[0] != '\0' && !TargetBuildVariantUser ()) {
      DEBUG ((EFI_D_VERBOSE, "Device will boot into FFBM mode\n"));
    } else {
      Status = DisplayVerifiedBootMenu (DISPLAY_MENU_ORANGE);
      if (Status == EFI_SUCCESS) {
        WaitForExitKeysDetection ();
      } else {
        DEBUG (
            (EFI_D_INFO, "Device is unlocked, Skipping boot verification\n"));
        MicroSecondDelay (5000000);
      }
    }
    break;
  default:
    break;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS LoadImageAndAuthForLE (BootInfo *Info)
{
    EFI_STATUS Status = EFI_SUCCESS;
    QcomAsn1x509Protocol *QcomAsn1X509Protocal = NULL;
    CONST UINT8 *OemCertFile = LeOemCertificate;
    UINTN OemCertFileLen = sizeof (LeOemCertificate);
    CERTIFICATE OemCert = {NULL};
    UINTN HashSize;
    UINT8 *ImgHash = NULL;
    UINTN ImgSize;
    VB_HASH HashAlgorithm;
    UINT8 *SigAddr = NULL;
    UINT32 SigSize = 0;
    CHAR8 *SystemPath = NULL;
    UINT32 SystemPathLen = 0;

    /*Load image*/
    GUARD (VBAllocateCmdLine (Info));
    GUARD (VBCommonInit (Info));
    GUARD (LoadImageNoAuth (Info));

    /* Initialize Verified Boot*/
    device_info_vb_t DevInfo_vb;
    DevInfo_vb.is_unlocked = IsUnlocked ();
    DevInfo_vb.is_unlock_critical = IsUnlockCritical ();
    Status = Info->VbIntf->VBDeviceInit (Info->VbIntf,
                                        (device_info_vb_t *)&DevInfo_vb);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "VB: Error during VBDeviceInit: %r\n", Status));
        return Status;
    }

    /* Locate QcomAsn1x509Protocol*/
    Status = gBS->LocateProtocol (&gEfiQcomASN1X509ProtocolGuid, NULL,
                                 (VOID **)&QcomAsn1X509Protocal);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "VB: Error LocateProtocol "
                      "gEfiQcomASN1X509ProtocolGuid: %r\n", Status));
        return Status;
    }

    /* Read OEM certificate from the embedded header file */
    Status = QcomAsn1X509Protocal->ASN1X509VerifyOEMCertificate
                (QcomAsn1X509Protocal, OemCertFile, OemCertFileLen, &OemCert);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "VB: Error during "
                      "ASN1X509VerifyOEMCertificate: %r\n", Status));
        return Status;
    }

    /*Calculate kernel image hash, SHA256 is used by default*/
    HashAlgorithm = VB_SHA256;
    HashSize = VB_SHA256_SIZE;
    ImgSize = Info->Images[0].ImageSize;
    ImgHash = AllocatePool (HashSize);
    if (ImgHash == NULL) {
        DEBUG ((EFI_D_ERROR, "kernel image hash buffer allocation failed!\n"));
        Status = EFI_OUT_OF_RESOURCES;
        return Status;
    }
    Status = LEGetImageHash (QcomAsn1X509Protocal, HashAlgorithm,
                (UINT8 *)Info->Images[0].ImageBuffer,
                ImgSize, ImgHash, HashSize);
    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "VB: Error during VBGetImageHash: %r\n", Status));
        return Status;
    }

    SigAddr = (UINT8 *)Info->Images[0].ImageBuffer + ImgSize;
    SigSize = LE_BOOTIMG_SIG_SIZE;
    Status = LEVerifyHashWithSignature (QcomAsn1X509Protocal, ImgHash,
    HashAlgorithm, &OemCert, SigAddr, SigSize);

    if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "VB: Error during "
                      "LEVBVerifyHashWithSignature: %r\n", Status));
        return Status;
    }
    DEBUG ((EFI_D_INFO, "VB: LoadImageAndAuthForLE complete!\n"));

    if (!IsRootCmdLineUpdated (Info)) {
        SystemPathLen = GetSystemPath (&SystemPath);
        if (SystemPathLen == 0 ||
            SystemPath == NULL) {
            return EFI_LOAD_ERROR;
        }
        GUARD (AppendVBCmdLine (Info, SystemPath));
    }
    return Status;
}

EFI_STATUS
LoadImageAndAuth (BootInfo *Info)
{
  EFI_STATUS Status = EFI_SUCCESS;
  BOOLEAN MdtpActive = FALSE;
  QCOM_MDTP_PROTOCOL *MdtpProtocol;
  UINT32 AVBVersion = NO_AVB;

  if (Info == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid parameter Info\n"));
    return EFI_INVALID_PARAMETER;
  }

  /* Get Partition Name*/
  if (!Info->MultiSlotBoot) {
    if (Info->BootIntoRecovery) {
      DEBUG ((EFI_D_INFO, "Booting Into Recovery Mode\n"));
      StrnCpyS (Info->Pname, ARRAY_SIZE (Info->Pname), L"recovery",
                StrLen (L"recovery"));
    } else {
      DEBUG ((EFI_D_INFO, "Booting Into Mission Mode\n"));
      StrnCpyS (Info->Pname, ARRAY_SIZE (Info->Pname), L"boot",
                StrLen (L"boot"));
    }
  } else {
    Slot CurrentSlot = {{0}};

    GUARD (FindBootableSlot (&CurrentSlot));
    if (IsSuffixEmpty (&CurrentSlot)) {
      DEBUG ((EFI_D_ERROR, "No bootable slot\n"));
      return EFI_LOAD_ERROR;
    }

    GUARD (StrnCpyS (Info->Pname, ARRAY_SIZE (Info->Pname), L"boot",
                     StrLen (L"boot")));
    GUARD (StrnCatS (Info->Pname, ARRAY_SIZE (Info->Pname), CurrentSlot.Suffix,
                     StrLen (CurrentSlot.Suffix)));
  }

  DEBUG ((EFI_D_VERBOSE, "MultiSlot %a, partition name %s\n",
          BooleanString[Info->MultiSlotBoot].name, Info->Pname));

  if (FixedPcdGetBool (EnableMdtpSupport)) {
    Status = IsMdtpActive (&MdtpActive);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "Failed to get activation state for MDTP, "
                           "Status=%r."
                           " Considering MDTP as active and continuing \n",
              Status));
      if (Status != EFI_NOT_FOUND)
        MdtpActive = TRUE;
    }
  }

  AVBVersion = GetAVBVersion ();
  DEBUG ((EFI_D_VERBOSE, "AVB version %d\n", AVBVersion));

  /* Load and Authenticate */
  switch (AVBVersion) {
  case NO_AVB:
    return LoadImageNoAuthWrapper (Info);
    break;
  case AVB_1:
    Status = LoadImageAndAuthVB1 (Info);
    break;
  case AVB_2:
    Status = LoadImageAndAuthVB2 (Info);
    break;
  case AVB_LE:
    Status = LoadImageAndAuthForLE (Info);
    break;
  default:
    DEBUG ((EFI_D_ERROR, "Unsupported AVB version %d\n", AVBVersion));
    Status = EFI_UNSUPPORTED;
  }

  // if MDTP is active Display Recovery UI
  if (Status != EFI_SUCCESS && MdtpActive) {
    Status = gBS->LocateProtocol (&gQcomMdtpProtocolGuid, NULL,
                                  (VOID **)&MdtpProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG (
          (EFI_D_ERROR, "Failed to locate MDTP protocol, Status=%r\n", Status));
      return Status;
    }
    /* Perform Local Deactivation of MDTP */
    Status = MdtpProtocol->MdtpDeactivate (MdtpProtocol, FALSE);
  }

  if (IsUnlocked () && Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "LoadImageAndAuth failed %r\n", Status));
    return Status;
  }

  if (AVBVersion != AVB_LE) {
    DisplayVerifiedBootScreen (Info);
    DEBUG ((EFI_D_VERBOSE, "Sending Milestone Call\n"));
    Status = Info->VbIntf->VBSendMilestone (Info->VbIntf);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "Error sending milestone call to TZ\n"));
      return Status;
    }
  }
  return Status;
}

VOID
FreeVerifiedBootResource (BootInfo *Info)
{
  DEBUG ((EFI_D_VERBOSE, "FreeVerifiedBootResource\n"));

  if (Info == NULL) {
    return;
  }

  VB2Data *VBData = Info->VBData;
  if (VBData != NULL) {
    AvbOps *Ops = VBData->Ops;
    if (Ops != NULL) {
      if (Ops->user_data != NULL) {
        avb_free (Ops->user_data);
      }
      AvbOpsFree (Ops);
    }

    AvbSlotVerifyData *SlotData = VBData->SlotData;
    if (SlotData != NULL) {
      avb_slot_verify_data_free (SlotData);
    }
    avb_free (VBData);
  }

  if (Info->VBCmdLine != NULL) {
    FreePool (Info->VBCmdLine);
  }
  return;
}

EFI_STATUS
GetCertFingerPrint (UINT8 *FingerPrint,
                    UINTN FingerPrintLen,
                    UINTN *FingerPrintLenOut)
{
  EFI_STATUS Status = EFI_SUCCESS;

  if (FingerPrint == NULL || FingerPrintLenOut == NULL ||
      FingerPrintLen < AVB_SHA256_DIGEST_SIZE) {
    DEBUG ((EFI_D_ERROR, "GetCertFingerPrint: Invalid parameters\n"));
    return EFI_INVALID_PARAMETER;
  }

  if (GetAVBVersion () == AVB_1) {
    QCOM_VERIFIEDBOOT_PROTOCOL *VbIntf = NULL;

    Status = gBS->LocateProtocol (&gEfiQcomVerifiedBootProtocolGuid, NULL,
                                  (VOID **)&VbIntf);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "Unable to locate VerifiedBoot Protocol\n"));
      return Status;
    }

    if (VbIntf->Revision < QCOM_VERIFIEDBOOT_PROTOCOL_REVISION) {
      DEBUG ((EFI_D_ERROR, "GetCertFingerPrint: VB1: not "
                           "supported for this revision\n"));
      return EFI_UNSUPPORTED;
    }

    Status = VbIntf->VBGetCertFingerPrint (VbIntf, FingerPrint, FingerPrintLen,
                                           FingerPrintLenOut);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "Failed Reading CERT FingerPrint\n"));
      return Status;
    }
  } else if (GetAVBVersion () == AVB_2) {
    CHAR8 *UserKeyBuffer = NULL;
    UINT32 UserKeyLength = 0;
    AvbSHA256Ctx UserKeyCtx = {{0}};
    CHAR8 *UserKeyDigest = NULL;

    GUARD (GetUserKey (&UserKeyBuffer, &UserKeyLength));

    avb_sha256_init (&UserKeyCtx);
    avb_sha256_update (&UserKeyCtx, (UINT8 *)UserKeyBuffer, UserKeyLength);
    UserKeyDigest = (CHAR8 *)avb_sha256_final (&UserKeyCtx);

    CopyMem (FingerPrint, UserKeyDigest, AVB_SHA256_DIGEST_SIZE);
    *FingerPrintLenOut = AVB_SHA256_DIGEST_SIZE;
  } else {
    DEBUG ((EFI_D_ERROR, "GetCertFingerPrint: not supported\n"));
    return EFI_UNSUPPORTED;
  }

  return Status;
}
