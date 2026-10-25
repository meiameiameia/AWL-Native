# Platform runtime for the recompiled game (P1 platform track)

Status: design, SDK identification and the runtime (milestones 1-3) implemented in `src/hle/`; `awl-host` boots the game to the main loop and the first REL load (section 9).  Target: NTSC-U `GYWEE9`, `main.dol` SHA1 `1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374`, verified with `certutil` before the analysis. Everything below was measured on the owner's local files with `awl-recomp disasm` and `awl-hle-scan` (`src/hle/hle_scan.cpp`); nothing derived from game bytes is committed except SDK names and addresses. Contract with the generator track: `tools/recomp/runtime/hle_abi.h`. Replaced functions: `tools/recomp/hle_manifest.txt`.

Evidence grades: **high** = read in the disassembly (instruction sequence, constants, call graph); **medium** = shape and call-graph position match the SDK routine of that name, name taken from the SDK's function order; **low** = inferred from register roles or from public hardware documentation without a confirming code path. Hardware register layouts come from the public GameCube register map and from what the SDK's own recompiled code does with them; an expectation that rests on neither is listed under "Open questions".

## 1. Approach in one table

The SDK in `main.dol` is the 2003-era Dolphin SDK with the CARD, THP, GBA, AX and DSP libraries linked. Its software parts (heaps, `printf`, math, queues, the thread scheduler, the alarm list, the file-system path code, the GX command writers, the AX voice code, THP decoding) stay recompiled and run unchanged. Replacement happens only where recompiled code cannot work, or where a driver is a hardware-timing state machine whose result can be produced directly.

| Family | Level | Replaced (native) | Stays recompiled | Native model |
| --- | --- | --- | --- | --- |
| CPU and OS primitives | API leaf | `OSSaveContext`, `OSLoadContext`, `PPCMtdec`, `PPCSync`, `DCFlushRange`, `DCStoreRange`, `RealMode`, `__OSInitAudioSystem` | thread queues, `SelectThread`, `OSSleepThread`/`OSWakeupThread`, alarms, message queues, `__OSDispatchInterrupt`, interrupt masks, heap, `OSLink` | fibers, event queue, interrupt delivery, time base |
| Interrupt controller | register | none | `__OSInterruptInit`, `SetInterruptMask`, `__OSDispatchInterrupt` | PI interrupt block, MI block |
| DVD | API | the command functions (read, seek, stream, inquiry, cancel, drive status) | `DVDInit`, `DVDOpen`/`Close`/`ConvertPathToEntrynum`/`ChangeDir` (read the FST in guest memory), `DVDReadPrio`/`DVDReadAsyncPrio` wrappers, `DVDGetCommandBlockStatus` | drive queue, file reads through the mount, two DI registers |
| VI | register | none | `VIInit`, `VIConfigure`, `VIFlush`, retrace ISR, `VIWaitForRetrace` | VI registers, display interrupts, beam position, frame-latch events |
| PAD / SI | API (PAD, `SIInit`) + register (SI) | `PADInit`, `PADReset`, `PADRecalibrate`, `PADRead`, `PADControlMotor`, `SIInit` (its asynchronous type probes need the SI transfer machinery nobody uses) | SI polling code (`SIRefreshSamplingRate` still runs from the VI retrace handler) | the SI polling register only; `PadAdapter` behind the PAD handlers (neutral input in P1) |
| GBA link | API | `GBAInit`, `GBAGetStatus`, `GBAReset`, `GBARead`, `GBAWrite` | none | "no GBA attached" |
| EXI, RTC, SRAM | register | none | `EXIInit`, `EXILock`, `EXISelect`, `EXIImm`, `EXISync`, `__OSInitSram`, SRAM access | EXI channels with RTC/SRAM device on channel 0; "no card" elsewhere |
| CARD | recompiled over EXI (P1), API replacement later (P3) | none in P1 | the whole CARD library | none in P1 (EXI reports no card) |
| AI, DSP, ARAM | register + protocol responder | `__OSInitAudioSystem` | `AIInit`, `DSPInit`, `ARInit`, ARQ, the DSP task code, all of AX | AI DMA engine (silent), DSP mailbox and control, ARAM array and DMA, AX-ucode responder |
| GX | register + FIFO | none | the whole GX library and the inline FIFO writers in game and HSD code | write-gather port, CP/PE/PI FIFO registers, command-stream parser, PE token/finish events |
| Debug output | optional | `OSReport` text capture (diagnostic only) | | log sink |

Rationale: the OS scheduler and interrupt dispatch contain the only non-local control flow in the program (`rfi` returning into a saved context). Two leaf primitives (`OSSaveContext`/`OSLoadContext`) plus a native interrupt-delivery step are enough to run all of that code unchanged on fibers. The DVD, PAD and GBA drivers sit on SI/DI/EXI protocols and alarm timeouts; their public command layer is small, so it is replaced. VI, PI, MI, AI, DSP, ARAM, EXI and GX are plain register files whose semantics the recompiled SDK code already defines, so modelling the registers is smaller and more faithful than replacing the libraries on top of them.

## 2. Identification

Convention: addresses are `main.dol` text addresses. "hmawl" marks a name taken from the read-only name list. Counts of hardware-facing functions come from `awl-hle-scan facts` with volatile registers forgotten at every call (the P0 census did not do this, so the numbers differ by a few): constant-address accesses by block: GX FIFO 115, DSP 24, DI 24, AI 15, VI 12, PI 11, SI 11, EXI 10, MI 5; 36 functions contain `rfi` or `sc` (dispositions in 2.1). The GX library reaches CP/PE/PI-FIFO registers through pointer variables, which the scan cannot see; those are found at run time.

### 2.1 OS and CPU

| Address | Name | Evidence | Grade | Disposition |
| --- | --- | --- | --- | --- |
| `0x80003154` | `__start` | DOL entry; calls `__init_hardware`, `OSInit`, `DBInit`, `main` | high | recompiled |
| `0x801B0DE0` | `OSInit` | hmawl; structure read in full (see 3.1); calls the init chain `__OSInterruptInit` ... `DVDInit`, `DVDInquiryAsync` | high | recompiled |
| `0x801B11B8` | `OSExceptionInit` | hmawl; `memcpy` of the exception-vector template from DOL text into low memory, then `DCFlushRangeNoSync`/`ICInvalidateRange` | high | recompiled (copies bytes that are never executed) |
| `0x801B1490`, `0x800036AC-0x800053AC` (24 slots) | exception vector template and slots | `rfi` bodies; only reachable through a hardware exception | high | unreachable by design; stay generator platform stubs and stop loudly |
| `0x801B3274` | `OSSwitchFPUContext` | `rfi`; handler for FP-unavailable | high | unreachable (the runtime never clears the FP-available state) |
| `0x801B4F54`, `0x801B4FD4` | `Config24MB`, `Config48MB` | BAT setup ending in `rfi`; only reached as arguments of `RealMode` | high | unreachable once `RealMode` is replaced |
| `0x801B6968`, `0x802333E0`, `0x80233610`, `0x80234C50-0x8023514C` | system-call vector body, TRK/debug state routines | `rfi`, DABR/IABR, SPRG use; `OSInit` calls `0x80234D70` only when the BI2 debug flag is set (it is 0 in the disc's `bi2.bin`) | high | unreachable |
| `0x801B2D8C` | `OSSaveContext` | hmawl; stores r1, r2, r13-r31, GQR1-7, CR, LR, CTR, XER, MSR (as SRR1), LR (as SRR0), puts 1 in the r3 slot, returns 0 | high | **replaced** |
| `0x801B2E0C` | `OSLoadContext` | hmawl; reloads the same fields and executes `rfi` | high | **replaced** |
| `0x801B6E7C` | `SelectThread` | hmawl; read in full: run-queue bit scan, `OSSaveContext` unless the context carries the exception flag (`+0x1A2` bit 1), idle spin, `OSLoadContext` of the next thread. No caller uses its return value (checked at all 7 call sites) | high | recompiled |
| `0x801B70A4` | `__OSReschedule` | hmawl; calls `SelectThread(0)` when the reschedule flag is set | high | recompiled |
| `0x801B70D4` | `OSCreateThread` | calls `OSInitContext` between `OSDisableInterrupts`/`OSRestoreInterrupts`; 3 DOL callers + 5 `wlp1` references | medium | recompiled |
| `0x801B72BC` | `OSExitThread` | `OSClearContext`, `__OSUnlockAllMutex`, `OSWakeupThread`, `SelectThread`; the link-register value planted by thread creation | medium | recompiled |
| `0x801B755C` `0x801B77E4` `0x801B73A0` `0x801B7954` `0x801B7A40` | `OSResumeThread`, `OSSuspendThread`, `OSCancelThread`, `OSSleepThread`, `OSWakeupThread` | hmawl | high | recompiled |
| `0x801B4CA8` `0x801B4D08` `0x801B4DD0` | `OSInitMessageQueue`, `OSSendMessage`, `OSReceiveMessage` | hmawl; `wlp1` references (9, 9, 10) | high | recompiled |
| `0x801B1618` `0x801B1670` `0x801B1680` `0x801B18D0` `0x801B1938` `0x801B1A54` | `OSInitAlarm`, `OSCreateAlarm`, `InsertAlarm`, `OSSetAlarm`, `OSCancelAlarm`, `DecrementerExceptionCallback` | hmawl; the only writers of the decrementer are `PPCMtdec` callers inside these | high | recompiled |
| `0x801B0A48` | `PPCMtdec` | hmawl; `mtspr DEC,r3; blr` | high | **replaced** (arms the native decrementer) |
| `0x801B3A44` `0x801B3A58` `0x801B3A6C` | `OSDisableInterrupts`, `OSEnableInterrupts`, `OSRestoreInterrupts` | hmawl; `mfmsr`/`mtmsr` on the EE bit; 128 + 3 + 125 DOL callers | high | recompiled (MSR is guest state in `Ctx.msr`) |
| `0x801B3AC0` `0x801B3B34` `0x801B3E0C` `0x801B3E94` `0x801B3F1C` | `__OSInterruptInit`, `SetInterruptMask`, `__OSMaskInterrupts`, `__OSUnmaskInterrupts`, `__OSDispatchInterrupt` | hmawl; the dispatcher calls `OSLoadContext`, `OSDisableScheduler`, `OSEnableScheduler`, `__OSReschedule` and `OSGetTime` and reads the PI/MI/DSP/EXI/AI cause registers; `__OSInterruptInit` is the first hardware boundary of the census (`0x801B3AC0`) | high | recompiled |
| `0x801B4260` | external-interrupt handler tail | saves GQR1-7 and jumps to `__OSDispatchInterrupt`; installed as exception handler by `__OSInterruptInit` | high | recompiled, entered by the native delivery step |
| `0x801B1C84` | `DecrementerExceptionHandler` | tail of the decrementer vector; calls `DecrementerExceptionCallback` | high | recompiled, entered by the native delivery step |
| `0x801B506C` | `__OSInitMemoryProtection` | hmawl; reads 24 MB sizes at `0x800000F0`, writes MI `0xCC004010/4020/4028`, sets handlers for interrupts 0-4, calls `RealMode` with `Config24MB` (or `Config48MB`) | high | recompiled |
| `0x801B5054` | `RealMode` | hmawl; SRR0 = r3, clears IR/DR, `rfi`; only caller above | high | **replaced** (no-op) |
| `0x801B20F4` | `__OSInitAudioSystem` | hmawl; copies 128 bytes from the arena top, runs a DSP boot program through DSP_CONTROL `0xCC00500A`, DMA registers `0xCC005020-28`, mailbox polling, then restores the bytes; called from `OSInit` | high | **replaced** (net effect: DSP left reset, memory unchanged) |
| `0x801B5E98` | `__OSInitSram` | hmawl; `EXILock`/`EXISelect`/`EXIImm`/`EXISync` sequences; caches the SRAM block | high | recompiled over the EXI model |
| `0x801B0A50` `0x801B23C8` `0x801B23F8` | `PPCSync`, `DCFlushRange`, `DCStoreRange` | hmawl; end in `sc`, whose SDK handler only synchronises caches | high | **replaced** (no-op) |
| `0x801B2630` `0x801B2654` | locked-cache block DMA | `mtspr DMA_U`/`DMA_L`; flag `0x12` (DMA_LD) in the first, `0x02` in the second. hmawl labels the first `LCStoreBlocks`; by the direction flag and operand roles it loads memory into the cache, which matches the SDK order (`LCLoadBlocks` first) | medium | no handler: the generator's `mtspr DMA_L` must perform the transfer (request in section 8) |
| `0x801B2678` `0x801B2724` `0x801B27D0` | `LCLoadData`/`LCStoreData` wrappers (hmawl labels the first `LCStoreData`), `LCQueueWait` | split a request into 128-block chunks; `LCQueueWait` spins on HID2 | medium | recompiled |
| `0x801B7BF0` `0x801B7C08` `0x801B7C10` | `OSGetTime`, `OSGetTick`, `__OSGetSystemTime` | hmawl; `mftb` pairs, system time adds the 64-bit word at `0x800030D8` | high | recompiled, time source via `timeBaseInstall` |
| `0x801B346C` | `OSReport` | hmawl; `vprintf` into the MSL console, whose `__write_console` stubs (`0x801FAF00/08`) return constants, so retail output is discarded | high | recompiled; native text capture is an optional diagnostic |
| `0x801770A8` | `OSPanic` | hmawl; 20 instructions that store the arguments and return (the retail build neutered it) | high | recompiled (faithful: a panic does not stop the original either) |
| `0x801B568C` `0x801B5210` `0x801B55D4` | `OSResetSystem`, `__OSReboot`, `Reset` | hmawl; hardware reset paths; `OSResetSystem` is called from `__check_pad3` and the game at `0x8000CF1C` | high | recompiled; reaching them is a stop (reset requested) |
| `0x801B4860` `0x801B4ABC` | `OSLink`, `OSUnlink` | hmawl; called from `0x801777C4`; REL relocation in guest memory | high | recompiled; REL dispatch registration is a generator-track item (section 8) |

### 2.2 DVD

The file-system code (`dvdfs`) is software over the FST that the apploader leaves in memory. `DVDInit` takes the retail branch (boot magic `0x0D15EA5E` at `0x80000020`; checked at `0x801BAE74`) and does not call `__fstLoad` (`0x801BDCFC`, reachable only for the development-hardware magic). The command layer is replaced.

| Address | Name | Evidence | Grade | Disposition |
| --- | --- | --- | --- | --- |
| `0x801BADF0` | `DVDInit` | hmawl; `__DVDFSInit`, `OSInitAlarm` helper, handler for interrupt 21, DI_STATUS/DI_COVER writes (`0xCC006000 = 0x2A`, `0xCC006004 = 0`) | high | recompiled; the DI model accepts exactly those two writes |
| `0x801BA340` | `__DVDFSInit` | copies `OSBootInfo.FSTLocation` (`0x80000038`) and computes the entry table from the root entry | high | recompiled |
| `0x801BA378` `0x801BA66C` `0x801BA734` `0x801BA97C` | `DVDConvertPathToEntrynum`, `DVDOpen`, `DVDClose`, `DVDChangeDir` | hmawl | high | recompiled |
| `0x801BA9DC` `0x801BAACC` `0x801BABE4` | `DVDReadAsyncPrio`, `DVDReadPrio`, sync callback | hmawl; the read wrappers add the file's start address, call `0x801BC9D8`; the sync variant sleeps on a thread queue until the callback wakes it | high | recompiled |
| `0x801BAC08` `0x801BACD0` | seek and prepare-stream wrappers on a `DVDFileInfo` | call `0x801BCAB4` / `0x801BCD24`; `0x801BACD0` is called from game code `0x80129928` next to `DVDOpen` | medium | recompiled |
| `0x801BC9D8` | `DVDReadAbsAsyncPrio` | command id 1 stored at block+8 | high (id) / medium (name) | **replaced** |
| `0x801BCAB4` | `DVDSeekAbsAsyncPrio` | id 2 | medium | **replaced** |
| `0x801BCB80` | `DVDReadAbsAsyncForBS` | id 4; only callers are the reboot path | medium | not needed (unreachable); would stop loudly |
| `0x801BCC50` | `DVDReadDiskID` | id 5; callers `__fstLoad` and `0x801BDC24` only | medium | not needed (unreachable) |
| `0x801BCD24` | `DVDPrepareStreamAbsAsync` | id 6 | medium | **replaced** |
| `0x801BCDE8` | `DVDCancelStreamAsync` | id 7; 4 game callers (`0x80129AAC`, `0x8012A0C0`, `0x8012A8E4`) | medium | **replaced** |
| `0x801BCEA4` | `DVDGetStreamErrorStatusAsync` | id 9; 5 game callers (`0x80129B1C`, `0x80129D28`, `0x8012A0C0`, `0x8012A3C8`, `0x8012A8E4`) | medium | **replaced** |
| `0x801BCF60` | `DVDInquiryAsync` | id 14; called by `OSInit` with `InquiryCallback` (`0x801B0DA4`, hmawl) | high | **replaced** |
| `0x801BD0C0` | `DVDGetDriveStatus` | returns -1 / 8 / 0 / 1 / `executing->state` from the SDK's globals | high | **replaced** (reports busy while a native request is pending) |
| `0x801BD1CC` | `DVDCancelAsync` | switch over `block->state` through a 12-entry table; invokes the cancel callback | high | **replaced** |
| `0x801BD074` | `DVDGetCommandBlockStatus` | `block->state` (3 maps to 1) under disabled interrupts; 13 game callers | high | recompiled; correct as long as the native drive keeps `block->state` |
| `0x801BD448` | `DVDCancel` | sync wrapper over `0x801BD1CC` plus `OSSleepThread` | high | recompiled |
| `0x801BBE50` and the `0x801B97E4-0x801BA2A8`, `0x801BB0BC-0x801BC3A0` state machine, `0x801B9504` DI handler | `stateReady` ... `DVDLow*` | driven only by the replaced commands | high | unreachable |

Command ids follow the SDK's `DVD_COMMAND_*` order (read 1, seek 2, boot-sector read 4, read ID 5, init stream 6, cancel stream 7, request audio error 9, inquiry 14); the order of the public functions in the address space is consistent with it. The disc has audio streaming enabled in its header (`boot.bin` offset 8) and the game streams through commands 6/7/9, so P1 must complete those commands even though no audio is produced.

### 2.3 VI

| Address | Name | Evidence | Grade |
| --- | --- | --- | --- |
| `0x801BE3BC` | `VIInit` | hmawl; reads `0x800000CC` (TV format), registers handler for interrupt 24, reads VTR `0xCC002002`, writes 19 VI registers | high |
| `0x801BED34` `0x801BF55C` `0x801BF68C` `0x801BF6F8` `0x801BE86C` | `VIConfigure`, `VIFlush`, `VISetNextFrameBuffer`, `VISetBlack`, `VIWaitForRetrace` | hmawl; game video setup `0x8017A3E0` calls all of them; `VIWaitForRetrace` sleeps on a thread queue | high |
| `0x801BDE64` | retrace ISR | installed for interrupt 24; reads and acknowledges the four display-interrupt registers `0xCC002030/34/38/3C` (status = bit 15 of the high half, cleared by writing the half back with it cleared), runs the pre/post retrace callbacks, wakes `VIWaitForRetrace` sleepers | high |
| `0x801BE1BC` | VI register write-out | 20 stores into `0xCC002000-0xCC00206C` | high |
| `0x801BF774` | `VIGetRetraceCount` | `lwz r3,-21092(r13); blr`; the game's main loop calls it twice per iteration | high |
| `0x801BF820` `0x801BF954` `0x801BF77C` `0x801BF8BC` | `VIGetNextField`, `VIGetTvFormat`, current-line helpers | hmawl / read of `0xCC00202C/2E` | high / medium |

### 2.4 PAD, SI, GBA

| Address | Name | Evidence | Grade | Disposition |
| --- | --- | --- | --- | --- |
| `0x801F52F4` | `SIInit` | hmawl; registers handler for interrupt 20, then starts SI type probes on 4 channels through `SITransfer` (`0x801F5DF0`); called by `OSInit` | high | **replaced** (no-op) |
| `0x801F4D58` | SI interrupt handler | installed for interrupt 20 | high | recompiled, never raised |
| `0x801C06D4` | `PADInit` | calls `OSRegisterVersion`, `OSRegisterResetFunction`; only caller is `HSD_PadInit` (`0x80213DA4`, hmawl) | high | **replaced** |
| `0x801C04B0` `0x801C05C0` | `PADReset`, `PADRecalibrate` | same shape; callers `0x80212720` and `0x80213D0C` (HSD pad code) | medium | **replaced** |
| `0x801C0824` | `PADRead` | 192 instructions, calls `PADReset`; only caller `0x80212720` | medium | **replaced** |
| `0x801C0B24` | `PADControlMotor` | only caller `0x802143A0` | medium | **replaced** |
| `0x801C1504` | `__PADDisableRecalibration` | hmawl; read-modify-write of `0x800030E3`; called by `OSResetSystem` | high | recompiled |
| `0x801FB9E4` `0x801FBA88` `0x801FBB18` `0x801FBC18` `0x801FBCDC` | `GBAInit`, `GBAGetStatus`, `GBAReset` (inferred from SDK order), `GBARead`, `GBAWrite` | hmawl; callers `0x801AA4D8`, `0x801AAB8C` (game code) | high (names) | **replaced**; semantics of "no GBA" taken from the callers' handling at M2 |

### 2.5 AI, ARAM, DSP, AX

All interrupt wiring below is read from the `__OSSetInterruptHandler` call sites (see 3.2).

| Address | Name | Evidence | Grade |
| --- | --- | --- | --- |
| `0x801C15C4` `0x801C164C` `0x801C1664` `0x801C167C` | AI DMA: init (address+length), start, stop, get start address | DSP-block registers `0xCC005030/32/36` (AI_DMA_START hi/lo, length with enable bit); callers in AX | medium |
| `0x801C19D8` `0x801C1B44` `0x801C1BC0` | `AIInit`, AI sample-counter handler (interrupt 8), AI DMA-done handler (interrupt 5) | `OSRegisterVersion`, handler registration, `0xCC006C00/04/0C` | medium |
| `0x801C1698` `0x801C1780` `0x801C189C` `0x801C1CC4` `0x801C1980-0x801C19C8` | AI stream control (play state, sample rate), SRC init, stream volume get/set (4 functions) | `0xCC006C00` read-modify-write, `0xCC006C04`, `0xCC006C08` + `OSGetTime`; the game calls the volume functions (`0x80129B1C` and others) | medium |
| `0x801C1F28` `0x801C1EEC` `0x801C2018` `0x801C20EC` `0x801C2164` | `ARStartDMA`, `ARGetDMAStatus`, `ARInit`, `__ARHandler` (interrupt 6), `__ARChecksize` | ARAM DMA registers `0xCC005020-2A`; `ARInit` registers interrupt 6 and calls the 1,533-instruction size probe | high / medium |
| `0x801C3B28` `0x801C3B98` | `ARQInit`, `ARQPostRequest` | `OSRegisterVersion`; the game posts requests directly (`0x80198D30`, `0x80198DE0`, `0x80198E80`) | medium |
| `0x801C3CFC` `0x801C3D0C` `0x801C3D1C` `0x801C3D34` | DSP mailbox primitives: check to-DSP, check from-DSP, read from-DSP, send to-DSP | registers `0xCC005000/02/04/06` | medium |
| `0x801C3D48` `0x801C3E0C` `0x801C3E54` `0x801C3E94` `0x801C3EA4` `0x801C3F64` | `DSPInit` (interrupt 7), reset, halt, status, `DSPAddTask`, `__DSPHandler` | `0xCC00500A` control register; handler registration; task list | medium |
| `0x801D...-0x801F3...` | AX, MIX and the game's audio engine | no hardware access; use the DSP/AI/AR layer above | high |

### 2.6 EXI, memory card

| Address | Name | Evidence | Grade |
| --- | --- | --- | --- |
| `0x801F7440` `0x801F62EC` `0x801F6548` `0x801F66D4` `0x801F6E54` `0x801F6F80` `0x801F7614` `0x801F7708` | `EXIInit`, `EXIImm`, `EXIImmEx`, `EXISync`, `EXISelect`, `EXIDeselect`, `EXILock`, `EXIUnlock` | hmawl; the library reaches the channel registers through a per-channel table; `EXIInit` writes `0xCC006800/14/28` | high |
| `0x801B5D80` `0x801B608C` and `0x801B5FD4-0x801B6544` | SRAM write, `UnlockSram`, `__OSLockSram*`, `OSGetSoundMode`, `OSGetWirelessID`/`OSSetWirelessID` | `EXILock`/`EXISelect`/`EXIImm` sequences; callers `0x80189BF4` (game audio setup), `0x801BDA80` | high |
| `0x801C565C` and the 17 functions `0x801C5864-0x801CAFE4` | CARD library (`CARDInit`, mount, open, read, write, delete, check, ...) | only references come from `wlp1.rel` (one each, two for `0x801C85EC` and `0x801C9C04`); built on `EXILock`/`EXISelect`/`EXIImmEx` and alarms; `CARDInit` registers the version string and an alarm | medium (membership) / low (individual names) |
| `0x801FAF18-0x801FB6F8` | MetroTRK EXI2 transport | EXI channel 2 `0xCC006828/34/38`; reached only from debugger code | medium |

### 2.7 GX

The GX library (`GXInit` `0x801CB378` through `0x801D2BA8`, hmawl names for 60+ entries) writes commands through the write-gather port `0xCC008000`; the same port is written inline by game code (`0x80091F58-0x8019AE34`) and by HSD code (`0x8020CAC4-0x8022CB40`). 115 functions contain constant-address FIFO stores. The SDK reaches CP, PE, PI-FIFO and MI registers through pointer variables initialised in `GXInit` (two `lis 0xCC00` sites in `0x801CB000-0x801CD000` and PI `0xCC003018` writes in `0x801CDAC0/0x801CDC2C`), so the register set is discovered at run time. Interrupt handlers: CP `0x801CC448` (interrupt 17), PE token `0x801CE08C` (18), PE finish `0x801CE158` (19). Draw sync: `GXSetDrawSync` `0x801CDD9C`, `GXReadDrawSync` `0x801CDE50`. Display copy: `GXCopyDisp` `0x801CED04`. The game's frame setup is `0x8017A3E0`.

## 3. Boot environment

The host plays the part of the IPL and apploader: it produces the state the DOL entry expects, without running their code. The disc extraction provides the inputs: `disc/sys/boot.bin` (disc header), `disc/sys/bi2.bin` (8 KB boot info 2), `disc/sys/fst.bin` (file-system table), and `disc/files/`.

### 3.1 Low memory and boot data

Each value below is either read by the recompiled code before it is written (evidence = the reader), or is a hardware fact; unread fields are zero.

| Address | Content | Source and justification |
| --- | --- | --- |
| `0x80000000-0x8000001F` | the 32-byte disc header (game ID `GYWEE9`, disc number, version, streaming flag and buffer size, magic) | copy of `boot.bin[0..0x20)`; `__fstLoad` copies the same 32 bytes (`memcpy` to `0x80000000`) and prints the ID and streaming flag |
| `0x80000020` | `0x0D15EA5E` | tested by `DVDInit` at `0x801BAE74`; this value selects the retail path |
| `0x80000024` | 1 | boot-info version; same value as the P0 runner (unverified; not read) |
| `0x80000028` | `0x01800000` (24 MB) | `__OSInitMemoryProtection` and `OSInit` (`0x801B10F4` prints it) read it |
| `0x8000002C` | console type, retail: top nibble 0 | `OSGetConsoleType` (`0x801B0C54`) returns it; `OSInit` prints from the top nibble (0 = retail text). Value `0x00000001` proposed; only the log text and `EXISync` depend on it. Dolphin question 1 |
| `0x80000030` | arena low, 0 | `OSInit` substitutes its own default `0x8035EFA0` when this is 0 (`0x801B0EF0`), derived from the DOL layout |
| `0x80000034` | arena high | `__start` rewrites it with the aligned address of the argument vector when `argc != 0`, which it is (below); `OSInit` otherwise defaults to `0x81700000`. Final value must come from a Dolphin snapshot (question 1) because the game's allocator (`0x80178558`) sizes its heaps from it |
| `0x80000038`, `0x8000003C` | FST location and maximum length | `__DVDFSInit` reads the first and takes the entry count from the root entry; the apploader places the FST; location proposed just below the BI2 block, length = `boot.bin+0x42C` |
| `0x800000CC` | TV mode 0 (NTSC) | `VIInit`, `VIConfigure` and `VIGetTvFormat` read it; the NTSC-U title |
| `0x800000D0` | ARAM size `0x01000000` | carried over from the P0 runner; no constant-address reader found. Dolphin question 1 |
| `0x800000F0` | simulated memory size `0x01800000` | `__OSInitMemoryProtection` reads it to pick `Config24MB` |
| `0x800000F4` | pointer to BI2 | read by `__start` and `OSInit`; BI2 block copied from `bi2.bin` |
| `0x800000F8` | bus clock 162,000,000 | 54 functions read it (tick conversions); public nominal value |
| `0x800000FC` | CPU clock 486,000,000 | same source; core clock = 3 x bus clock |
| `0x800030D8` (64-bit) | system-time offset, 0 | `__OSGetSystemTime` adds it to the time base; nothing writes it by constant address |
| `0x800030E2`, `0x800030E3`, `0x800030E4` | 0 | reset code, PAD recalibration disable bits, PAD 3 held-button word at boot (`__check_pad3` compares it with a reset combination) |
| `0x800030E6` | written by `InquiryCallback`; read by `__start` | `__start` calls `__check_pad3` unless the word is `0x8000 | x` with `x != 1`. Harmless in either case because `0x800030E4` is 0 |
| `0x800030E8`, `0x800030E9` | written by `OSInit` from BI2 bytes `+0x0C` and `+0x24` | no preset |
| BI2 block | `bi2.bin` | `__start` reads `+0xC` (debug flag, 0) and `+0x8` (argument offset); the arguments region has `argc = 2` and two short strings; the vector is rebased relative to the BI2 address and the arena high word is set from it |

Machine state at entry: all of MEM1 zero-filled first (deterministic; the DOL clears its own BSS), r1/r2/r13 set by the DOL itself in `__init_registers`, MSR with FP and ME set and EE clear (as the P0 runner), FPSCR 0, HID/BAT registers plain storage.

### 3.2 DOL loading

Sections are copied to their load addresses in MEM1 (`0x80003100-0x8034D020`, text and data); BSS is not cleared by the host (the game's `__start` fills it with its own `memset` loop, which the P0 trace shows executing). The DOL entry is the guest entry function. The BI2 block and the FST are placed above the arena in the top of MEM1 and referenced from `0x800000F4` and `0x80000038`. Exact top-of-memory layout follows the Dolphin snapshot (question 1); until then it follows the SDK's own accounting (the arena words above).

Interrupt numbers, from the call sites of `__OSSetInterruptHandler` (`0x801B3A90`):

| Interrupt | Source | Handler | Registered at |
| --- | --- | --- | --- |
| 0-4 | memory protection (MI) | `0x801B4EE8` | `0x801B50BC-EC` |
| 5 | AI DMA done (DSP block) | `0x801C1BC0` | `0x801C1B00` |
| 6 | ARAM DMA | `0x801C20EC` | `0x801C2070` |
| 7 | DSP mailbox | `0x801C3F64` | `0x801C3D9C` |
| 8 | AI sample counter | `0x801C1B44` | `0x801C1B18` |
| 9-16 | EXI channels 0-2 (EXI, TC, EXT) | installed by `EXIInit` through a table | `0x801F74CC-0x801F7530` |
| 17 | CP (FIFO overflow/underflow/breakpoint) | `0x801CC448` | `0x801CCA5C` |
| 18 / 19 | PE token / finish | `0x801CE08C` / `0x801CE158` | `0x801CE1F0/E200` |
| 20 | SI | `0x801F4D58` | `0x801F5368` |
| 21 | DI | `0x801B9504` | `0x801BAE40` |
| 22 | reset button | `0x801B5974` | `0x801B0F7C` |
| 24 | VI | `0x801BDE64` | `0x801BE840` |
| 25 | debugger EXI2 | `0x801FB36C` | `0x801FB2D8` |

## 4. Time

| Quantity | Value | Justification |
| --- | --- | --- |
| Bus clock | 162,000,000 Hz | public nominal value; stored at `0x800000F8` |
| CPU clock | 486,000,000 Hz | stored at `0x800000FC` |
| Time-base tick rate | 40,500,000 Hz | the SDK defines a tick as bus clock / 4; the DOL derives millisecond and microsecond conversions from `0x800000F8` (54 readers) |
| Decrementer | counts at the time-base rate, interrupts when it goes from 0 to negative | PowerPC architecture; `PPCMtdec` is the only writer |
| NTSC field period | 675,675 ticks (59.94 Hz) | field rate 60000/1001 Hz; 40.5e6 x 1001 / 60000. A 525-line frame is 858 x 525 pixel clocks at 13.5 MHz |
| Scan line | 2,574 ticks | 858 pixel clocks x 3 ticks per pixel clock (40.5 MHz / 13.5 MHz); 262.5 lines per field |
| Instruction to tick ratio | 4 instructions per tick | same convention as the P0 runner (about 3 cycles per instruction, 12 cycles per tick); a model, not a measurement |

The guest clock in deterministic mode is `tb = (instructions retired by all threads) / 4 + idleSkipTicks`; the real-time mode (later) scales host time instead. Both go through `timeBaseInstall`. Progressive 480p uses the same field period, so the retrace period does not depend on the mode the game picks.

## 5. Runtime design

All state lives in `src/hle/` behind one `Platform` object created by the host, registered with the generator's runtime through `hleRegister`, `mmioInstall`, `timeBaseInstall` and the preemption hook (section 8).

### 5.1 Threads on fibers

- The host's main thread becomes a supervisor fiber (`ConvertThreadToFiber`). The boot guest thread (the DOL entry) runs on its own fiber, created with `CreateFiberEx` and a 4 MB reserve (the shadow stack allows 8,192 frames).
- One guest thread = one fiber + one `rt::Ctx` (from `createThreadContext`) + one host stack. Thread stacks in guest memory belong to the game.
- A registry maps an `OSContext` guest address to its fiber. It is filled lazily: `OSSaveContext(ctx)` binds `ctx` to the calling fiber; the native interrupt step binds the context stored at `0x800000D4` (current context) to the interrupted fiber; the first `OSLoadContext(ctx)` of an unknown context starts a new thread.
- **`OSSaveContext(ctx)`**: write the same fields the original stores (r1, r2, r13-r31, GQR1-7, CR, LR as both LR and SRR0, MSR as SRR1, CTR, XER, the 1 in the r3 slot) into the guest `OSContext` so that any reader sees the original image; return 0.
- **`OSLoadContext(ctx)`**:
  - ctx belongs to a suspended fiber: switch to it. The target resumes inside its own earlier `OSLoadContext` call, which returns into `SelectThread`'s epilogue (the return value is unused by all callers) or into `__OSDispatchInterrupt`'s tail; both unwind exactly as the `rfi` into the saved context would.
  - ctx is the context of the interrupt activation currently running: return (the activation unwinds back to the delivery step).
  - ctx is unknown: build a `Ctx` from the guest image (GPRs, LR, CR, CTR, XER, GQRs, SRR1 as MSR), create a fiber and `callGuest(entry = SRR0)`; when the entry returns, run the function at the image's LR (the SDK plants `OSExitThread`) with the entry's return value in r3. Only this path trusts the guest image.
  - any other target stops loudly with the context address.
- An uncaught `rt::Stop` inside a fiber is caught at the fiber's entry, recorded, and control returns to the supervisor fiber, which reports and exits.
- Exited threads are retired and their fibers freed from the supervisor.
- Idle: `SelectThread` already spins with interrupts enabled when no thread is runnable (`0x801B6FC8`); the spin retires instructions, which advances the clock and delivers the event that wakes a thread. A later optimisation can skip the clock to the next event.

### 5.2 Events, interrupts and callbacks

- **Event queue**: ordered by (tick, sequence number), deterministic. Kinds: decrementer, VI display interrupt n, DVD request complete, AI DMA buffer end, AI sample-counter, ARAM DMA complete, DSP mail, PE token, PE finish, CP breakpoint, EXI transfer complete, SI (idle).
- **Interrupt state**: device models set cause bits; the PI model derives `INTSR & INTMR` for the recompiled `SetInterruptMask`/`__OSDispatchInterrupt`, which read and write those registers like hardware.
- **Delivery points**: the generator's `rt::at()` calls a platform hook once `c.icount` exceeds `c.limit`. The platform sets `c.limit` of the running context to the next event deadline (in that context's instruction count). The hook advances the queue to the current clock; it delivers when `Ctx.msr` bit EE (`0x8000`) is set. While an event is due but interrupts are masked, the limit is re-armed 256 instructions ahead (maximum latency after unmasking: 256 instructions, deterministic).
- **Delivery step** (the replacement for the exception vector):
  1. External interrupt pending (PI cause and mask, EE set): save the interrupted state into the context at `0x800000D4` (registers, SRR0 = pc, SRR1 = MSR, state flag bit 1 = exception) exactly as the vector does, create or reuse an interrupt `Ctx` (not the interrupted thread's), give it the interrupted r1, MSR with EE clear, and `callGuest(0x801B4260)`-equivalent: set r3 = exception number (external), r4 = context, call `__OSDispatchInterrupt` (`0x801B3F1C`). Its tail `OSLoadContext(context)` returns to the step (see 5.1).
  2. Decrementer expired: same, entering `DecrementerExceptionHandler` (`0x801B1C84`).
  3. Priority: external before decrementer, as on the CPU.
- **API-level callbacks** (DVD completion, GBA): run the same envelope the dispatcher gives handlers: `OSDisableScheduler`, the callback via `callGuest(cb)` with its arguments in r3/r4, `OSEnableScheduler`, `__OSReschedule`, all as recompiled code.
- **Nesting**: one interrupt `Ctx` per nesting level per fiber, at most two; deeper nesting stops loudly.
- **Thread switch inside an interrupt**: handled by 5.1 (the interrupted fiber suspends inside the delivery step).

### 5.3 Time source

`timeBaseInstall` provides `tb(c) = (instruction base + c.icount) / 4 + idle ticks`. On a fiber switch the platform folds the outgoing context's retired instructions into the base so that the clock is global and monotonic. All event deadlines are in ticks. Two runs execute the same instruction stream and therefore produce the same trace.

### 5.4 Device models

One `mmioInstall` pair routes by block:

| Block | Offsets | Model |
| --- | --- | --- |
| CP / PE | `0xCC000000`, `0xCC001000` | FIFO base/top/read/write pointers, status; PE token, finish and interrupt-enable registers fed by the FIFO parser |
| VI | `0xCC002000` | register file with write-through; 4 display-interrupt registers (enable, vertical/horizontal position, status cleared by write); beam position from the clock (field period 675,675 ticks, line 2,574 ticks); frame latch on top/bottom framebuffer writes; field alternation |
| PI | `0xCC003000` | `INTSR`/`INTMR` (aggregate of device causes), reset switch state (not pressed), reset register, CPU FIFO base/end/write pointer and reset |
| MI | `0xCC004000` | protection and interrupt registers accept the writes seen in 2.1; never raises |
| DSP / AI / ARAM | `0xCC005000`, `0xCC006C00` | DSP control and mailbox, AI DMA address/length/enable, AI control and sample counter, ARAM DMA; ARAM is a 16 MB array |
| DI | `0xCC006000` | accepts the two `DVDInit` writes; any other access stops |
| SI | `0xCC006400` | only the polling register (0x30) is modelled; every other access stops |
| EXI | `0xCC006800` | three channels with the SDK's control/DMA/immediate registers; channel 0 device 1 = RTC and SRAM; every other device absent (EXT clear, ID reads 0) |
| GX FIFO | `0xCC008000` | write-gather port: accepts 1/2/4/8-byte writes, packs them big-endian into the stream; reads stop |

Unsupported registers and sizes stop with the guest pc and address (`rt::stop`), never ignore. Reads of write-only registers stop too.

### 5.5 GX in P1 (no drawing)

The model captures everything the guest writes to the write-gather port into a command stream, parses it at command granularity, and completes the services the SDK waits on:

- **Parser**: opcodes `0x00` NOP, `0x08` CP register load, `0x10` XF load, `0x20-0x38` indexed XF load, `0x40` call display list (read from guest memory and parse recursively), `0x48` invalidate vertex cache, `0x61` BP register load, `0x80-0xB8` draws. The vertex size of a draw depends on the vertex descriptor and attribute formats set by earlier CP loads, so the parser tracks VCD/VAT/array-stride registers to stay in sync.
- **Completion**: BP `PE_DONE` (finish) raises the PE finish interrupt after a small latency; BP token writes set the PE token register and raise the token interrupt when requested; EFB copies are counted and exposed to the frame criterion; `GXReadDrawSync` reads the token. The FIFO read pointer follows the write pointer immediately, so watermark interrupts never fire.
- **Fail-loud**: an unknown opcode, an unsupported register or a draw with an undefined VAT stops with the offset.

Everything else about GX is P2. The captured stream is kept in memory (ring) for the P2 renderer.

### 5.6 DVD

- Mount: `disc/` relative to the working directory, files under `disc/files/` addressed by FST path; `src/filesystem.cpp` supplies `filesystem_resolve_path` (traversal-safe). It supports one mount and no directory listing, so the first version builds its own path-to-file index from `fst.bin` (names) and the mount root; a mod overlay needs a second mount and size-adjusted FST entries (not P1).
- Memory: `fst.bin` is placed in guest memory at the location published in `0x80000038`; the recompiled path functions walk it.
- Reads: a request carries `(disc offset, length, buffer)`; the offset is resolved to the FST entry whose range contains it, the host file is read at `offset - entry.offset`, and bytes are written to guest memory. A request that spans entries, or lies outside every entry, completes with the SDK failure result (-1) and logs.
- Queue: native, priority-ordered (the original semantics for the `prio` argument), one request at a time, completion after a modelled latency (a few thousand ticks), then the callback through the API-level envelope with `(length, block)`; `block->state` is 2 while waiting, 1 while executing, 0 when done, 10 when cancelled; `transferredSize` is updated.
- Streaming commands (prepare, cancel, error status): accepted and completed without audio; results in the structure the callers expect (to be read from the callers at M2).
- `DVDInquiryAsync`: fills the drive-info buffer (revision, device code, date) and completes; the device code word flows into `0x800030E6`.

### 5.7 PAD

`PADInit`/`PADReset`/`PADRecalibrate` keep a small native channel state and report channel 0 connected (neutral stick, no buttons) and channels 1-3 absent; `PADRead` fills the `PADStatus` array (the layout is the SDK's: buttons, stick, substick, triggers, analog, error). The values come from the existing `PadAdapter` (`include/awl/input.h`) when a host provides frames, and are neutral otherwise (P1). The exact error codes and field offsets are read from the HSD caller at M2.

## 6. Main-loop criterion

The game's loop is `main` (`0x8000AA54`) at `0x8000AC34-0x8000AD3C`: read `VIGetRetraceCount`, run the frame update and draw functions, spin on `VIGetRetraceCount` until the configured number of retraces has passed, call the frame-end function `0x8017A7F8` (`VISetNextFrameBuffer`, `VIFlush`, `VIWaitForRetrace`), the pad renewal `0x80213CE4`, repeat.

Reached when, after the boot sequence, **N consecutive VI retraces (N = 120 by default, configurable) each have** a retrace callback delivered and, within every window of 3 retraces, at least one framebuffer latch (a write of a new framebuffer address into the VI) preceded by an EFB copy command in the GX stream. Bring-up counters in the host report retraces, latches, copies, tokens and finishes separately so a stall shows which condition failed. The deterministic trace is the sequence of (tick, event) pairs and the guest calls of the manifested functions; two runs must be byte-identical.

## 7. Source layout and tests

`src/hle/`: `platform.{h,cpp}` (clock, event queue, interrupt delivery, fibers, `OSSaveContext`/`OSLoadContext`), `os_handlers.cpp` (MMIO routing, the native SDK replacements, main-loop bookkeeping), `devices.h` with `dev_core.cpp` (PI, MI, DI stub, SI polling register), `dev_vi.cpp`, `dev_exi.cpp` (EXI with the RTC/SRAM device), `dev_audio.cpp` (DSP responder, ARAM, AI), `dev_gx.cpp` (write-gather port, CP, PE, command-stream parser), `dvd.{h,cpp}`, `boot.{h,cpp}` (boot environment), `hle_common.{h,cpp}` (guest memory access, boot trace), `host_main.cpp` (`awl-host`), `hle_scan.cpp` (`awl-hle-scan`). `tools/recomp/platform.cmake` builds the library `awl_hle`, the host (only when `build/recomp-gen` exists), the scanner and the tests.

Tests (`hle_core_tests`, 82 checks; `hle_sched_tests`, 28 checks) need no game files. Expectations come from the SDK code read above (OSContext offsets, register bit tests, the call order of the callback envelope), public register maps, the tick arithmetic in section 4 and the GX command formats. They cover the event queue and clock, PI, the VI display interrupts and beam position, EXI with RTC/SRAM (checksum loop of `UnlockSram`), the GX parser (vertex sizes, finish, token, EFB copy, ring mirror), the DVD queue/cancel/file reads, thread start and switch through `OSSaveContext`/`OSLoadContext`, interrupt delivery and the callback envelope.

Run (from the repository root, with the disc extracted under `disc/`; the generated output comes from `awl-recomp gen --full --dol rom/main.dol --rel-dir disc/files --out build/recomp-gen`):

```powershell
cmake -S . -B build
cmake --build build --config Release --target awl-host
build/Release/awl-host.exe --dol rom/main.dol --disc disc --trace build/boot-trace.log [--limit N] [--retraces N]
```

`awl-host` is deterministic: the trace of two runs is byte-identical (checked).

## 8. Requests and open questions

### 8.1 `hle_abi.h` and cross-track requests

1. **Preemption hook (required)**: `using PreemptHook = void (*)(Ctx&); void preemptInstall(PreemptHook);`. When `c.icount > c.limit` and a hook is installed, `rt::at()` calls the hook instead of `limitReached`; the hook delivers events, may switch fibers, and must set a new `c.limit` (if it leaves `limit` below `icount` the existing `InstructionLimit` stop applies). Without it, asynchronous interrupts and the idle spin cannot work.
2. `createThreadContext(parent)` must give a context that shares `mem`/`lc`, has `skipPlatform` false, an empty shadow stack (`sp == 0`, so the first return is unchecked) and `limit` = unlimited; and `destroyThreadContext` must free it. Please state that `callGuest` sets `LR` to a value the top-level `ret` accepts and returns with r3/f1 intact.
3. **Locked-cache DMA (generator semantics)**: `mtspr DMA_L` (923) must perform the transfer described by `DMA_U`/`DMA_L` (Gekko User Manual: memory address in `DMA_U`, cache address and direction in `DMA_L`, length in blocks of 32 bytes, 0 = 128) against `c.mem` and `c.lc`; completion is instantaneous. Only the SDK's `LC*` functions use it (checked: the SPRs appear in 3 functions). Otherwise the platform must replace the `0x801B2630`/`0x801B2654` pair.
4. **REL dispatch**: after the recompiled `OSLink` (`0x801B4860`) returns, the REL's functions must be registered at `base + offset`, and removed after `OSUnlink` (`0x801B4ABC`). Please decide whether the generator does this inside the generated `OSLink`/`OSUnlink` bodies (the platform leaves both recompiled) or exposes a hook for the platform to call.
5. `hleCall` should set `c.pc = guestAddr` before the handler runs so one handler function can serve several manifest entries and report the address.
6. Optional: a `void* host` field in `Ctx` for the platform's per-thread data.

### 8.2 Dolphin black-box questions

Values to capture from a fresh boot of the NTSC-U disc in Dolphin (breakpoints in its debugger; no code is copied from it):

1. **Memory at DOL entry** (breakpoint at `0x80003154`, before the first instruction): the 64 words at `0x80000000-0x800000FF`; the 16 words at `0x800030C0-0x800030FF`; the value of `0x800000F4` and the first `0x20` and last `0x20` bytes of the 8 KB block it points to; the first `0x20` bytes at `*(0x80000038)`; the time base (`TBL`/`TBU`) at that point. Then at `0x80003268` (after `OSInit`): `0x80000030`, `0x80000034`, `0x800030E6`, `0x800030E8`, and the registers r3/r4 at the call to `0x80178558` (the game's arena setup).
2. **Retrace timing**: at every call of `0x801BF774` made from `0x8000AC34` and `0x8000AD14` for 150 consecutive frames, log r3 and the time base. This gives the retrace period in ticks, the 30/60 fps pacing the game requests, and the phase of the first retrace after `VIConfigure`.
3. **VI mode**: at the call of `VIConfigure` (`0x801BED34`) the 0x40 bytes at the `GXRenderModeObj` pointer in r3 (interlaced or progressive, framebuffer and EFB sizes); and, after the first `VIFlush` returns, the values of the 4 display-interrupt registers (`0xCC002030/34/38/3C`).
4. **DSP traffic**: every mailbox value in both directions (`0xCC005000-0xCC005006`) from reset to the first AX frame callback, with the time base, and whether Dolphin runs the HLE or LLE DSP for this game.
5. **Drive information**: after `DVDInquiryAsync` completes (`InquiryCallback`, `0x801B0DA4`, entry), the 32-byte buffer at the first argument's `+0x20` and the resulting `0x800030E6`.
6. **Call trace**: with a breakpoint-log at the addresses listed in `tools/recomp/hle_manifest.txt` plus `OSInit` (`0x801B0DE0`), `OSCreateThread` (`0x801B70D4`), `DVDOpen` (`0x801BA66C`), `VIInit`, `GXInit` (`0x801CB378`), `AIInit` (`0x801C19D8`), `DSPInit` (`0x801C3D48`), `ARInit` (`0x801C2018`): the first 300 hits of each with the time base and r3-r6. This is the reference for the boot trace the P1 exit test compares.
7. Low priority: the SRAM block as Dolphin presents it by default, and the results of `OSGetSoundMode` (`0x801B6420`) and `OSGetProgressiveMode` (`0x801B64A0`) at the call from `0x80189BF4`.

### 8.3 Unverified assumptions

- Instruction-to-tick ratio and the latencies of DVD, EFB copies and token events are model parameters (section 4, 5.2); the original's exact timing is not reproduced.
- Register layouts for VI display interrupts, AI, ARAM DMA and the PI cause bits are taken from the public register map and confirmed only where the SDK's code reads them (retrace ISR, `__OSDispatchInterrupt`, `ARStartDMA`); bits the code does not touch are not modelled.
- Names marked medium or low in section 2 may change when the code is read at M2; the manifest's addresses stay valid.

## 9. State after milestones 2 and 3

**What runs.** `awl-host` runs the recompiled entry through `OSInit` (interrupt controller, exception table, SRAM load over EXI, thread init, DSP boot hack skipped, `DVDInit`, `DVDInquiryAsync`), `DBInit`, the game's `main`: video init (`VIInit`, `VIConfigure`, `VIFlush`, `VIWaitForRetrace` with the retrace ISR delivered through the platform's interrupt step), `GXInit` and the first frame setup (4 framebuffer register changes, 1 EFB copy, 1 draw-sync token), audio init (AI, ARAM size probe and DMA, DSP boot ROM handshake, AX task INIT and per-frame mail exchange), `DVDReadAsyncPrio` of the first REL file (about 433 KB), and enters the main loop at `0x8000AC34`. The first REL's `OSLink` succeeds in the recompiled SDK; the run then stops at the call into the REL's prolog (`0x803A2A8C`): the RELs are not part of the generated output yet.

**Main-loop criterion.** Implemented as in section 6 (N consecutive qualifying retraces, default 120). It cannot be met until the REL code runs, because the frame update and draw calls of the loop live there; the boot reaches the loop and the REL load is the furthest point observable now.

**Unmodelled by design (stop loudly).** Any DI register other than DVDInit's two writes and the DI_CONFIG read; SI registers other than the polling register; EXI DMA read-write and RTC writes; MI registers other than those seen; CP/PE registers outside the modelled ranges; any DSP microcode mail other than the boot sequence and the AX frame pair; unknown GX command opcodes; execution on a context the platform does not know.

**Stub register (everything temporary the runtime does; each use is logged in the boot trace unless noted).**

| Stub | Where | Behaviour | Removed by |
| --- | --- | --- | --- |
| GBA absent | `GBAInit`, `GBAGetStatus`, `GBAReset`, `GBARead`, `GBAWrite` | return GBA_NOT_READY (1) | GBA link support, if ever |
| PAD neutral input | `PADInit`, `PADReset`, `PADRecalibrate`, `PADRead`, `PADControlMotor` | channel 0 connected with no input, channels 1-3 absent; first 8 calls logged | P3 input (`PadAdapter`) |
| DVD streaming | commands 6, 7, 9 | accepted and completed with result 0 and no data | P4 audio streaming |
| DVD timing | all commands | fixed latency (1 ms + 4 ticks per byte) | measured drive model, if wanted |
| DVD drive info | `DVDInquiryAsync` | placeholder revision and device code | Dolphin question 5 |
| DSP silence | DSP responder, AI DMA | boot ROM and AX frame handshake only; no mixing, no samples consumed; DMA replays the buffer | P4 audio |
| `__OSInitAudioSystem`, `SIInit` | manifest | no-op | none needed (hardware bring-up with no native analogue) |
| EXI cards | channels 0-1 | no card (EXT clear); CS2 and the UART read zero | P3 saves |
| RTC/SRAM defaults | EXI device | fixed time (2026-01-01), stereo, English, no progressive scan | settings |
| GX | CP, PE, FIFO | idle command processor; finish/token/copy completed after 2000 ticks; the stream is parsed and not drawn | P2 |
| Boot environment | `boot.cpp` | arena, FST/BI2 placement, console type, ARAM size, DI_CONFIG are assumptions printed in the boot-trace header | Dolphin questions 1 and 5 |
| Timing | everything | 4 instructions per tick; event latencies are model parameters | measurement |

**Contract points the runtime relies on.** `awl-recomp gen` must recompile functions whose only platform evidence is constant-address MMIO (`platform.privileged == 0`) and stub only `rfi`/`sc` functions; with the committed generator every such function is a `platformBoundary` stub and the host stops at the first PI write. The host was verified with that one-line change (`codegen.cpp`, `f.platform.any()` replaced by `f.platform.privileged != 0`) applied locally; it is requested from the generator track.
