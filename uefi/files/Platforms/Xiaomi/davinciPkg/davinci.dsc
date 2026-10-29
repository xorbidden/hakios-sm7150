##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = davinci
  PLATFORM_GUID                  = 9B70EF10-5424-4738-8142-6662F105742C
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/davinciPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = davinciPkg/davinci.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 1

  #
  # 0 = SM7150
  # 1 = SM7150-AB
  # 2 = SM7150-AC
  #
  SOC_TYPE                       = 0

!include MooreaPkg/MooreaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Mi 9T"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"davinci"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"M1903F10G"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"F10"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2340
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"WP"

[BuildOptions]
  #
  # HackiOS: the XNU loader hook (SiliciumPkg BootManagerLib/XnuLoader.c) writes nothing to flash
  #
  *_*_*_CC_FLAGS = -DXNU_NO_FLASH_LOG -DXNU_DIAG_SHUTDOWN -DXNU_DISABLE_APSS_WDT=0x17C10000

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|davinciPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|davinciPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  #davinci/AcpiTables.inf
