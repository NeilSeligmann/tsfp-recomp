/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DERIVED FROM A CC0-1.0 WORK. Do not hand-edit -- regenerate instead:
 *
 *     ./.venv/bin/python -m tools.arity_oracle emit-c \
 *         --output src/xbox/kernel_arity_oracle.c
 *
 * Source: XboxDev/nxdk, lib/xboxkrnl/xboxkrnl.exe.def
 *         https://github.com/XboxDev/nxdk
 *         SPDX-License-Identifier: CC0-1.0
 *         SPDX-FileCopyrightText: 2017 Stefan Schmidt
 *
 * The input is vendored verbatim at tools/data/nxdk/xboxkrnl.exe.def, so this table
 * is reproducible from a fresh clone. CC0-1.0 is a public-domain dedication;
 * docs/provenance.md verdicts XboxDev/nxdk lib/xboxkrnl/ as USE after auditing 873
 * commits for Microsoft DDK fingerprints with a positive control. The derivation is
 * recorded here rather than left implicit because that is what the licence terms of
 * the inputs to this project require of us.
 *
 * WHAT THE NUMBERS ARE. An MSVC-decorated export name carries the argument BYTE
 * count: `Name@N` is __stdcall, `@Name@N` is __fastcall, a bare name is __cdecl or
 * a DATA export. Dividing by four gives dwords. This is a NAME DECORATION, not a
 * measurement, so it cannot be fooled by a late `_icall_esp` bracket, by a
 * callee-saved push inside an argument window, or by a `jmp [slot]` import stub --
 * the three ways the call-site scanner has actually gone wrong on this image.
 *
 * WHAT IT IS NOT. This is nxdk's export list, not XDK 5849's. Ordinal drift across
 * XDK builds is real. Nothing here overrides a hand-verified ABI_TABLE row in
 * src/host/kernel_thunk.c; see kernel_arity_oracle.h for the ordering rule.
 */

#include "kernel_arity_oracle.h"

/*
 * Rows are ordinal-ascending and the lookup below relies on that for a binary
 * search. Ordinals are NOT dense -- the gaps are real holes in the export table.
 */
static const kernel_arity_oracle_entry ORACLE_TABLE[] = {
    {1u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* AvGetSavedDataAddress */
    {2u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* AvSendTVEncoderOption */
    {3u, KERNEL_ARITY_ORACLE_STDCALL, 6u, 0u}, /* AvSetDisplayMode */
    {4u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* AvSetSavedDataAddress */
    {5u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* DbgBreakPoint */
    {6u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* DbgBreakPointWithStatus */
    {7u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* DbgLoadImageSymbols */
    {8u, KERNEL_ARITY_ORACLE_CDECL, 0u, 0u}, /* DbgPrint */
    {9u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* HalReadSMCTrayState */
    {10u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* DbgPrompt */
    {11u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* DbgUnLoadImageSymbols */
    {12u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExAcquireReadWriteLockExclusive */
    {13u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExAcquireReadWriteLockShared */
    {14u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExAllocatePool */
    {15u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* ExAllocatePoolWithTag */
    {16u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* ExEventObjectType */
    {17u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExFreePool */
    {18u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExInitializeReadWriteLock */
    {19u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* ExInterlockedAddLargeInteger */
    {20u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 2u}, /* ExInterlockedAddLargeStatistic */
    {21u, KERNEL_ARITY_ORACLE_FASTCALL, 1u, 2u}, /* ExInterlockedCompareExchange64 */
    {22u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* ExMutantObjectType */
    {23u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExQueryPoolBlockSize */
    {24u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* ExQueryNonVolatileSetting */
    {25u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* ExReadWriteRefurbInfo */
    {26u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExRaiseException */
    {27u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExRaiseStatus */
    {28u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ExReleaseReadWriteLock */
    {29u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* ExSaveNonVolatileSetting */
    {30u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* ExSemaphoreObjectType */
    {31u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* ExTimerObjectType */
    {32u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 2u}, /* ExfInterlockedInsertHeadList */
    {33u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 2u}, /* ExfInterlockedInsertTailList */
    {34u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* ExfInterlockedRemoveHeadList */
    {35u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* FscGetCacheSize */
    {36u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* FscInvalidateIdleBlocks */
    {37u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* FscSetCacheSize */
    {38u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* HalClearSoftwareInterrupt */
    {39u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* HalDisableSystemInterrupt */
    {40u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* HalDiskCachePartitionCount */
    {41u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* HalDiskModelNumber */
    {42u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* HalDiskSerialNumber */
    {43u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* HalEnableSystemInterrupt */
    {44u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* HalGetInterruptVector */
    {45u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* HalReadSMBusValue */
    {46u, KERNEL_ARITY_ORACLE_STDCALL, 6u, 0u}, /* HalReadWritePCISpace */
    {47u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* HalRegisterShutdownNotification */
    {48u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* HalRequestSoftwareInterrupt */
    {49u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* HalReturnToFirmware */
    {50u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* HalWriteSMBusValue */
    {51u, KERNEL_ARITY_ORACLE_FASTCALL, 1u, 2u}, /* InterlockedCompareExchange */
    {52u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* InterlockedDecrement */
    {53u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* InterlockedIncrement */
    {54u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 2u}, /* InterlockedExchange */
    {55u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 2u}, /* InterlockedExchangeAdd */
    {56u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* InterlockedFlushSList */
    {57u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* InterlockedPopEntrySList */
    {58u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 2u}, /* InterlockedPushEntrySList */
    {59u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* IoAllocateIrp */
    {60u, KERNEL_ARITY_ORACLE_STDCALL, 6u, 0u}, /* IoBuildAsynchronousFsdRequest */
    {61u, KERNEL_ARITY_ORACLE_STDCALL, 9u, 0u}, /* IoBuildDeviceIoControlRequest */
    {62u, KERNEL_ARITY_ORACLE_STDCALL, 7u, 0u}, /* IoBuildSynchronousFsdRequest */
    {63u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* IoCheckShareAccess */
    {64u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* IoCompletionObjectType */
    {65u, KERNEL_ARITY_ORACLE_STDCALL, 6u, 0u}, /* IoCreateDevice */
    {66u, KERNEL_ARITY_ORACLE_STDCALL, 10u, 0u}, /* IoCreateFile */
    {67u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* IoCreateSymbolicLink */
    {68u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* IoDeleteDevice */
    {69u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* IoDeleteSymbolicLink */
    {70u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* IoDeviceObjectType */
    {71u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* IoFileObjectType */
    {72u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* IoFreeIrp */
    {73u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* IoInitializeIrp */
    {74u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* IoInvalidDeviceRequest */
    {75u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* IoQueryFileInformation */
    {76u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* IoQueryVolumeInformation */
    {77u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* IoQueueThreadIrp */
    {78u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* IoRemoveShareAccess */
    {79u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* IoSetIoCompletion */
    {80u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* IoSetShareAccess */
    {81u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* IoStartNextPacket */
    {82u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* IoStartNextPacketByKey */
    {83u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* IoStartPacket */
    {84u, KERNEL_ARITY_ORACLE_STDCALL, 8u, 0u}, /* IoSynchronousDeviceIoControlRequest */
    {85u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* IoSynchronousFsdRequest */
    {86u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 2u}, /* IofCallDriver */
    {87u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 2u}, /* IofCompleteRequest */
    {88u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* KdDebuggerEnabled */
    {89u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* KdDebuggerNotPresent */
    {90u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* IoDismountVolume */
    {91u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* IoDismountVolumeByName */
    {92u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeAlertResumeThread */
    {93u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeAlertThread */
    {94u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeBoostPriorityThread */
    {95u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeBugCheck */
    {96u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* KeBugCheckEx */
    {97u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeCancelTimer */
    {98u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeConnectInterrupt */
    {99u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KeDelayExecutionThread */
    {100u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeDisconnectInterrupt */
    {101u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeEnterCriticalRegion */
    {102u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* MmGlobalData */
    {103u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeGetCurrentIrql */
    {104u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeGetCurrentThread */
    {105u, KERNEL_ARITY_ORACLE_STDCALL, 7u, 0u}, /* KeInitializeApc */
    {106u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeInitializeDeviceQueue */
    {107u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KeInitializeDpc */
    {108u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KeInitializeEvent */
    {109u, KERNEL_ARITY_ORACLE_STDCALL, 7u, 0u}, /* KeInitializeInterrupt */
    {110u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeInitializeMutant */
    {111u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeInitializeQueue */
    {112u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KeInitializeSemaphore */
    {113u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeInitializeTimerEx */
    {114u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KeInsertByKeyDeviceQueue */
    {115u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeInsertDeviceQueue */
    {116u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeInsertHeadQueue */
    {117u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeInsertQueue */
    {118u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* KeInsertQueueApc */
    {119u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KeInsertQueueDpc */
    {120u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* KeInterruptTime */
    {121u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeIsExecutingDpc */
    {122u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeLeaveCriticalRegion */
    {123u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KePulseEvent */
    {124u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeQueryBasePriorityThread */
    {125u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeQueryInterruptTime */
    {126u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeQueryPerformanceCounter */
    {127u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeQueryPerformanceFrequency */
    {128u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeQuerySystemTime */
    {129u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeRaiseIrqlToDpcLevel */
    {130u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* KeRaiseIrqlToSynchLevel */
    {131u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* KeReleaseMutant */
    {132u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* KeReleaseSemaphore */
    {133u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeRemoveByKeyDeviceQueue */
    {134u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeRemoveDeviceQueue */
    {135u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeRemoveEntryDeviceQueue */
    {136u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KeRemoveQueue */
    {137u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeRemoveQueueDpc */
    {138u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeResetEvent */
    {139u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeRestoreFloatingPointState */
    {140u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeResumeThread */
    {141u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeRundownQueue */
    {142u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeSaveFloatingPointState */
    {143u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeSetBasePriorityThread */
    {144u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeSetDisableBoostThread */
    {145u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KeSetEvent */
    {146u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeSetEventBoostPriority */
    {147u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeSetPriorityProcess */
    {148u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* KeSetPriorityThread */
    {149u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* KeSetTimer */
    {150u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* KeSetTimerEx */
    {151u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeStallExecutionProcessor */
    {152u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeSuspendThread */
    {153u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* KeSynchronizeExecution */
    {154u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* KeSystemTime */
    {155u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* KeTestAlertThread */
    {156u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* KeTickCount */
    {157u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* KeTimeIncrement */
    {158u, KERNEL_ARITY_ORACLE_STDCALL, 8u, 0u}, /* KeWaitForMultipleObjects */
    {159u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* KeWaitForSingleObject */
    {160u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* KfRaiseIrql */
    {161u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* KfLowerIrql */
    {162u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* KiBugCheckData */
    {163u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* KiUnlockDispatcherDatabase */
    {164u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* LaunchDataPage */
    {165u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* MmAllocateContiguousMemory */
    {166u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* MmAllocateContiguousMemoryEx */
    {167u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmAllocateSystemMemory */
    {168u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmClaimGpuInstanceMemory */
    {169u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmCreateKernelStack */
    {170u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmDeleteKernelStack */
    {171u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* MmFreeContiguousMemory */
    {172u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmFreeSystemMemory */
    {173u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* MmGetPhysicalAddress */
    {174u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* MmIsAddressValid */
    {175u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* MmLockUnlockBufferPages */
    {176u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmLockUnlockPhysicalPage */
    {177u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* MmMapIoSpace */
    {178u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* MmPersistContiguousMemory */
    {179u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* MmQueryAddressProtect */
    {180u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* MmQueryAllocationSize */
    {181u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* MmQueryStatistics */
    {182u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* MmSetAddressProtect */
    {183u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmUnmapIoSpace */
    {184u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* NtAllocateVirtualMemory */
    {185u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtCancelTimer */
    {186u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* NtClearEvent */
    {187u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* NtClose */
    {188u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtCreateDirectoryObject */
    {189u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* NtCreateEvent */
    {190u, KERNEL_ARITY_ORACLE_STDCALL, 9u, 0u}, /* NtCreateFile */
    {191u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* NtCreateIoCompletion */
    {192u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* NtCreateMutant */
    {193u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* NtCreateSemaphore */
    {194u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* NtCreateTimer */
    {195u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* NtDeleteFile */
    {196u, KERNEL_ARITY_ORACLE_STDCALL, 10u, 0u}, /* NtDeviceIoControlFile */
    {197u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* NtDuplicateObject */
    {198u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtFlushBuffersFile */
    {199u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* NtFreeVirtualMemory */
    {200u, KERNEL_ARITY_ORACLE_STDCALL, 10u, 0u}, /* NtFsControlFile */
    {201u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtOpenDirectoryObject */
    {202u, KERNEL_ARITY_ORACLE_STDCALL, 6u, 0u}, /* NtOpenFile */
    {203u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtOpenSymbolicLinkObject */
    {204u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* NtProtectVirtualMemory */
    {205u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtPulseEvent */
    {206u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* NtQueueApcThread */
    {207u, KERNEL_ARITY_ORACLE_STDCALL, 10u, 0u}, /* NtQueryDirectoryFile */
    {208u, KERNEL_ARITY_ORACLE_STDCALL, 6u, 0u}, /* NtQueryDirectoryObject */
    {209u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtQueryEvent */
    {210u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtQueryFullAttributesFile */
    {211u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* NtQueryInformationFile */
    {212u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtQueryIoCompletion */
    {213u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtQueryMutant */
    {214u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtQuerySemaphore */
    {215u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* NtQuerySymbolicLinkObject */
    {216u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtQueryTimer */
    {217u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtQueryVirtualMemory */
    {218u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* NtQueryVolumeInformationFile */
    {219u, KERNEL_ARITY_ORACLE_STDCALL, 8u, 0u}, /* NtReadFile */
    {220u, KERNEL_ARITY_ORACLE_STDCALL, 8u, 0u}, /* NtReadFileScatter */
    {221u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtReleaseMutant */
    {222u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* NtReleaseSemaphore */
    {223u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* NtRemoveIoCompletion */
    {224u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtResumeThread */
    {225u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtSetEvent */
    {226u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* NtSetInformationFile */
    {227u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* NtSetIoCompletion */
    {228u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtSetSystemTime */
    {229u, KERNEL_ARITY_ORACLE_STDCALL, 8u, 0u}, /* NtSetTimerEx */
    {230u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* NtSignalAndWaitForSingleObjectEx */
    {231u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* NtSuspendThread */
    {232u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* NtUserIoApcDispatcher */
    {233u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* NtWaitForSingleObject */
    {234u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* NtWaitForSingleObjectEx */
    {235u, KERNEL_ARITY_ORACLE_STDCALL, 6u, 0u}, /* NtWaitForMultipleObjectsEx */
    {236u, KERNEL_ARITY_ORACLE_STDCALL, 8u, 0u}, /* NtWriteFile */
    {237u, KERNEL_ARITY_ORACLE_STDCALL, 8u, 0u}, /* NtWriteFileGather */
    {238u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* NtYieldExecution */
    {239u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* ObCreateObject */
    {240u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* ObDirectoryObjectType */
    {241u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* ObInsertObject */
    {242u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* ObMakeTemporaryObject */
    {243u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* ObOpenObjectByName */
    {244u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* ObOpenObjectByPointer */
    {245u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* ObpObjectHandleTable */
    {246u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* ObReferenceObjectByHandle */
    {247u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* ObReferenceObjectByName */
    {248u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* ObReferenceObjectByPointer */
    {249u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* ObSymbolicLinkObjectType */
    {250u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* ObfDereferenceObject */
    {251u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* ObfReferenceObject */
    {252u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* PhyGetLinkState */
    {253u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* PhyInitialize */
    {254u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* PsCreateSystemThread */
    {255u, KERNEL_ARITY_ORACLE_STDCALL, 10u, 0u}, /* PsCreateSystemThreadEx */
    {256u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* PsQueryStatistics */
    {257u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* PsSetCreateThreadNotifyRoutine */
    {258u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* PsTerminateSystemThread */
    {259u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* PsThreadObjectType */
    {260u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlAnsiStringToUnicodeString */
    {261u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlAppendStringToString */
    {262u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlAppendUnicodeStringToString */
    {263u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlAppendUnicodeToString */
    {264u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* RtlAssert */
    {265u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlCaptureContext */
    {266u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* RtlCaptureStackBackTrace */
    {267u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlCharToInteger */
    {268u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlCompareMemory */
    {269u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlCompareMemoryUlong */
    {270u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlCompareString */
    {271u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlCompareUnicodeString */
    {272u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlCopyString */
    {273u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlCopyUnicodeString */
    {274u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlCreateUnicodeString */
    {275u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlDowncaseUnicodeChar */
    {276u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlDowncaseUnicodeString */
    {277u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlEnterCriticalSection */
    {278u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlEnterCriticalSectionAndRegion */
    {279u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlEqualString */
    {280u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlEqualUnicodeString */
    {281u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlExtendedIntegerMultiply */
    {282u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* RtlExtendedLargeIntegerDivide */
    {283u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* RtlExtendedMagicDivide */
    {284u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlFillMemory */
    {285u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlFillMemoryUlong */
    {286u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlFreeAnsiString */
    {287u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlFreeUnicodeString */
    {288u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlGetCallersAddress */
    {289u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlInitAnsiString */
    {290u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlInitUnicodeString */
    {291u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlInitializeCriticalSection */
    {292u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* RtlIntegerToChar */
    {293u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlIntegerToUnicodeString */
    {294u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlLeaveCriticalSection */
    {295u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlLeaveCriticalSectionAndRegion */
    {296u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlLowerChar */
    {297u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlMapGenericMask */
    {298u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlMoveMemory */
    {299u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* RtlMultiByteToUnicodeN */
    {300u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlMultiByteToUnicodeSize */
    {301u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlNtStatusToDosError */
    {302u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlRaiseException */
    {303u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlRaiseStatus */
    {304u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlTimeFieldsToTime */
    {305u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlTimeToTimeFields */
    {306u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlTryEnterCriticalSection */
    {307u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* RtlUlongByteSwap */
    {308u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlUnicodeStringToAnsiString */
    {309u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlUnicodeStringToInteger */
    {310u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* RtlUnicodeToMultiByteN */
    {311u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlUnicodeToMultiByteSize */
    {312u, KERNEL_ARITY_ORACLE_STDCALL, 4u, 0u}, /* RtlUnwind */
    {313u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlUpcaseUnicodeChar */
    {314u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlUpcaseUnicodeString */
    {315u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* RtlUpcaseUnicodeToMultiByteN */
    {316u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* RtlUpperChar */
    {317u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlUpperString */
    {318u, KERNEL_ARITY_ORACLE_FASTCALL, 0u, 1u}, /* RtlUshortByteSwap */
    {319u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlWalkFrameChain */
    {320u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* RtlZeroMemory */
    {321u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* XboxEEPROMKey */
    {322u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* XboxHardwareInfo */
    {323u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* XboxHDKey */
    {324u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* XboxKrnlVersion */
    {325u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* XboxSignatureKey */
    {326u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* XeImageFileName */
    {327u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* XeLoadSection */
    {328u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* XeUnloadSection */
    {329u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* READ_PORT_BUFFER_UCHAR */
    {330u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* READ_PORT_BUFFER_USHORT */
    {331u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* READ_PORT_BUFFER_ULONG */
    {332u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* WRITE_PORT_BUFFER_UCHAR */
    {333u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* WRITE_PORT_BUFFER_USHORT */
    {334u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* WRITE_PORT_BUFFER_ULONG */
    {335u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* XcSHAInit */
    {336u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* XcSHAUpdate */
    {337u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* XcSHAFinal */
    {338u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* XcRC4Key */
    {339u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* XcRC4Crypt */
    {340u, KERNEL_ARITY_ORACLE_STDCALL, 7u, 0u}, /* XcHMAC */
    {341u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* XcPKEncPublic */
    {342u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* XcPKDecPrivate */
    {343u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* XcPKGetKeyLen */
    {344u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* XcVerifyPKCS1Signature */
    {345u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* XcModExp */
    {346u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* XcDESKeyParity */
    {347u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* XcKeyTable */
    {348u, KERNEL_ARITY_ORACLE_STDCALL, 5u, 0u}, /* XcBlockCrypt */
    {349u, KERNEL_ARITY_ORACLE_STDCALL, 7u, 0u}, /* XcBlockCryptCBC */
    {350u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* XcCryptService */
    {351u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* XcUpdateCrypto */
    {352u, KERNEL_ARITY_ORACLE_STDCALL, 3u, 0u}, /* RtlRip */
    {353u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* XboxLANKey */
    {354u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* XboxAlternateSignatureKeys */
    {355u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* XePublicKeyData */
    {356u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* HalBootSMCVideoMode */
    {357u, KERNEL_ARITY_ORACLE_DATA, 0u, 0u}, /* IdexChannelObject */
    {358u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* HalIsResetOrShutdownPending */
    {359u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* IoMarkIrpMustComplete */
    {360u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* HalInitiateShutdown */
    {361u, KERNEL_ARITY_ORACLE_CDECL, 0u, 0u}, /* RtlSnprintf */
    {362u, KERNEL_ARITY_ORACLE_CDECL, 0u, 0u}, /* RtlSprintf */
    {363u, KERNEL_ARITY_ORACLE_CDECL, 0u, 0u}, /* RtlVsnprintf */
    {364u, KERNEL_ARITY_ORACLE_CDECL, 0u, 0u}, /* RtlVsprintf */
    {365u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* HalEnableSecureTrayEject */
    {366u, KERNEL_ARITY_ORACLE_STDCALL, 1u, 0u}, /* HalWriteSMCScratchRegister */
    {374u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmDbgAllocateMemory */
    {375u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmDbgFreeMemory */
    {376u, KERNEL_ARITY_ORACLE_STDCALL, 0u, 0u}, /* MmDbgQueryAvailablePages */
    {377u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmDbgReleaseAddress */
    {378u, KERNEL_ARITY_ORACLE_STDCALL, 2u, 0u}, /* MmDbgWriteCheck */
};

#define ORACLE_TABLE_COUNT (sizeof(ORACLE_TABLE) / sizeof(ORACLE_TABLE[0]))

unsigned kernel_arity_oracle_count(void)
{
    return (unsigned)ORACLE_TABLE_COUNT;
}

const kernel_arity_oracle_entry *kernel_arity_oracle_at(unsigned index)
{
    if (index >= (unsigned)ORACLE_TABLE_COUNT) {
        return NULL;
    }
    return &ORACLE_TABLE[index];
}

const kernel_arity_oracle_entry *kernel_arity_oracle_lookup(unsigned ordinal)
{
    size_t low = 0;
    size_t high = ORACLE_TABLE_COUNT;
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        unsigned probe = ORACLE_TABLE[mid].ordinal;
        if (probe == ordinal) {
            return &ORACLE_TABLE[mid];
        }
        if (probe < ordinal) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    return NULL;
}

bool kernel_arity_oracle_callee_pop(unsigned ordinal, unsigned *out_dwords)
{
    const kernel_arity_oracle_entry *entry = kernel_arity_oracle_lookup(ordinal);
    if (entry == NULL) {
        return false;
    }
    /* A DATA export is not a function and has no arity. Handing one a pop count is
     * the mutation this file's test suite exists to catch: the thunk would pop bytes
     * nobody pushed and desync esp for every later call. */
    if (entry->convention == KERNEL_ARITY_ORACLE_DATA) {
        return false;
    }
    /* __cdecl is a POSITIVE zero, not a refusal: the caller cleans up, so the callee
     * pops nothing however many arguments were passed. Every cdecl row in this table
     * is a variadic printf-family export, whose true arity varies per call site and
     * is unknowable from a decoration -- which is exactly why callee cleanup cannot
     * apply to it. */
    *out_dwords = entry->stack_args;
    return true;
}
