# Supported CPUs

These values are accepted by `--cpu`:

- `stm32h533`
- `stm32h563` (default)
- `stm32h5f4`
- `stm32u585`
- `stm32l552`
- `lpc55s69`
- `mcxw71c` — NXP MCX W71 / MCXW716C (Cortex-M33, TrustZone, FMU0 flash)
- `mcxn947`
- `rw612` — NXP RW612 (Cortex-M33, FlexSPI NOR boot, ROM FlexSPI driver, ELS / PKA crypto offload)
- `nrf5340`
- `nrf5340net` — Nordic nRF5340 network core (Cortex-M33 without TrustZone, DSP or FPU; flash at 0x01000000, 2 KB NVMC pages)
- `nrf54l15` — Nordic nRF54L15 (Cortex-M33, TrustZone through SPU / MPC)
- `nrf54lm20` — Nordic nRF54LM20A (Cortex-M33, TrustZone through SPU / MPC)
- `rp2350` — Raspberry Pi RP2350 (dual Cortex-M33, three PIO blocks)
- `pic32ck`
- `m2354` — Nuvoton NuMicro M2354 (Cortex-M23, ARMv8-M Baseline)
- `imxrt700` — NXP i.MX RT700 / MIMXRT798S (dual Cortex-M33, TrustZone, ELS / PKC / PUF)

### STM32H5 flash notes

- **Programming.** Stores with PG set collect in the 128-bit write buffer (`WBNE`
  while it waits); the flash word is programmed in one operation once all 16 bytes are
  in, or on `FW`, with the bytes never written left at 1. A store to another flash word
  before the buffer is full raises `INCERR` and loses both; an erase or option change
  started with `WBNE` set raises `PGSERR` (RM0481 7.3.5, 7.9). A flash word is never
  programmed twice: over one that is not erased the program is refused with `PGSERR`.
- **Option bytes.** `FLASH_OPTCR` is locked until `FLASH_OPTKEYR` takes its two keys.
  The `_PRG` registers (`OPTSR`, `SECWMx`, `WRPx`) change only while it is unlocked,
  and an option change (`OPTSTRT`) makes them the option bytes in force (`_CUR`).
  Write protection and the watermarks act at once; a new `SWAP_BANK` waits for the next
  reset, and `FLASH_OPTCR.SWAP_BANK` tells which mapping is in force now (RM0481 7.3.10).
  `--optbyte` and `monitor optbytes` set them as the device ships.

### MCX W71 notes

- **Always Secure.** The core resets Secure and TrustZone cannot be turned off, so
  images linked at the non-secure aliases (`0x0` flash, `0x20000000` STCM) run Secure
  with the SAU disabled, as on the part; `--no-tz` does not demote the core.
- **FMU0 flash controller.** Flash is programmed through FMU0 (`0x40020000` /
  `0x50020000`): a command launched from `FCCOB0` by writing `FSTAT.CCIF` raises
  `PEWEN`, the phrase (16 bytes, `PGMPHR` 0x24), page (128 bytes, `PGMPG` 0x23) or any
  phrase of the 8 KB sector (`ERSSCR` 0x42) is stored to the array through either
  alias, `PERDY` rises once the unit is complete, and acknowledging `PERDY` runs it
  and sets `CCIF`. `ERSALL` (0x40) and the verify-erase commands (0x00-0x04, `FAIL`
  on non-erased data) are modelled too. A store with no write phase open bus-faults; a
  sub-word store, a store outside the latched phrase or an unknown command raise
  `ACCERR`, and a launch is ignored while `ACCERR`/`PVIOL` are set. Programming only
  clears bits. The single 1 MB block has no read-while-write: the array faults on
  reads and fetches while a command is open, so flash routines must run from SRAM.
  Program and erase are written back with `--persist`.
- **Clocks and power.** SCG, CCM32K, VBAT and MRCC report their oscillators valid; SPC
  never shows `SC.BUSY` and acknowledges `SRAMCTL.REQ` at once, so the SDK
  `BOARD_BootClockRUN` completes. SMSCM (`OCMDR0` cache control) and the MCM
  (`0xE0080000`, `CPCR2.CCBC` self-clearing) are register files.
- **Console.** LPUART1 (`0x40039000`, the FRDM-MCXW71 debug UART) takes `--uart-stdout`;
  LPUART0 gets a PTY.
- **Not modelled.** The ELE S200 enclave (ELEMU mailbox) and the radio.

### RW612 notes

- **Boot.** The part is flashless: the FlexSPI AHB window (`0x08000000` NS / `0x18000000` S)
  holds a 64 MB serial NOR. Images with a FlexSPI configuration block (`FCFB` tag at
  offset `0x400`) boot like the ROM boots them, from the vector table at offset `0x1000`;
  the image header in the vector table (length at `0x20`, load address at `0x34`) makes a
  load address in SRAM a load-to-RAM image. Raw images without an FCB start at offset 0.
  Signed image types are not authenticated.
- **Always Secure.** The ROM starts the core Secure, so images linked at the non-secure
  aliases still run Secure; neither `--no-tz` nor a non-secure initial stack demotes it.
- **FlexSPI NOR.** The XIP window is read-only to the CPU. The NOR is programmed through
  the boot ROM FlexSPI NOR driver (`fsl_romapi_flexspi.h` layout) at entry 5 of the ROM
  API tree, `0x13030000` (A1 and later) or `0x13024100` (A0), picked by `SOCCTRL
  CHIP_INFO` (A2 by default, `M33MU_RW612_CHIP_REV` overrides it). `init`, `page_program`
  (NOR semantics: bits only clear, the page wraps), `partial_program`, `erase`,
  `erase_sector`, `erase_block`, `erase_all`, `read` and `get_config` act on the NOR;
  changes are written back with `--persist`. `M33MU_ROMAPI_TRACE` and
  `M33MU_FLEXSPI_TRACE` trace the calls and the NOR operations.
- **SRAM.** 1.2 MB, visible at both the code-bus alias (`0x0` / `0x10000000`) and the
  system-bus alias (`0x20000000` / `0x30000000`).
- **Peripherals.** CLKCTL/RSTCTL, FlexComm USART/SPI (clock and reset on `CLKCTL1` /
  `RSTCTL1` like the SDK), CACHE64 (commands complete at once), FlexSPI controller
  registers, SOCCTRL, GPIO, CTIMER/MRT, ELS and PKA.

### i.MX RT700 notes

- **Boot.** The part is flashless: the XSPI0 AHB window (`0x28000000` NS / `0x38000000` S)
  is the "flash". Images with an FCB (`FCFB` tag at offset 0) boot like the ROM boots them,
  from the vector table at offset `0x4000`; a non-zero `imageExecutionAddress` (vector
  table + `0x34`) makes it a load-to-RAM image, copied to SRAM before start. Raw images
  without an FCB start at offset 0 (or `--boot-offset`).
- **Always Secure.** The ROM starts the core Secure and TrustZone cannot be fused off,
  so images linked at the non-secure aliases (as the SDK links them) still run Secure;
  neither `--no-tz` nor a non-secure initial stack demotes the core.
- **XSPI0 flash controller.** The flash is programmed the way the part programs its
  NOR: IP commands running the LUT sequences (write enable, status, page program,
  sector/block erase, reads) of the EVK's octal NOR. The SFP flash region (FRAD) and
  initiator domain (MDAD) descriptors are enforced, including their locks. Program and
  erase are written back with `--persist`.
- **SRAM.** 7.5 MB, visible at both the code-bus alias (`0x0` / `0x10000000`) and the
  system-bus alias (`0x20000000` / `0x30000000`).
- **Second core.** CPU1 (sense domain) is released with the SDK multicore-manager
  sequence: GLIKEY4 unlock, `SYSCON3 CPU1_SVTOR`, CPU1 clock and reset, then
  `SYSCON3 CPU_STATUS.CPU_WAIT` cleared. MU1 connects the cores with interrupts on both
  sides; each core's private timers raise interrupts on its own NVIC.
- **Security.** SAU attribution plus the AHBSC0 SRAM memory rules (compute domain);
  GLIKEY write-enable sequences gate the protected registers; TRNG, OCOTP fuses and the
  ROM API OTP driver are modelled. ELS / PKC / PUF use m33mu's synthetic command
  interface (documented in `cpu/imxrt700/imxrt700_secure.h`) and need wolfSSL.
- **Peripherals.** Every block of the CM33 SVDs is present as a register file with its
  reset values (generated by `cpu/imxrt700/gen_svd_regs.py`); LPUART/LPSPI
  (LP_FLEXCOMM), RGPIO, CRC, CTIMER, OS event timer, MU1 and XSPI0 have behaviour.
- **Not modelled.** The HiFi4/HiFi1 DSPs and the EZH-V core.

### nRF54L15 / nRF54LM20 notes

- **One address per memory.** RRAM (`0x0`) and RAM (`0x20000000`) have no secure alias.
  The core always starts Secure, also under `--no-tz`. Peripherals answer at `0x5xxxxxxx`
  (secure) and `0x4xxxxxxx` (non-secure).
- **Security.** The SAU decides the attribute of each transaction; `0x5xxxxxxx` and the
  ICACHE are always Secure. A peripheral answers only at the alias its SPU
  `PERIPH[n].PERM.SECATTR` selects (Secure at reset), else `EVENTS_PERIPHACCERR` and a
  bus fault; `DMASEC` sets the EasyDMA attribute and EasyDMA reaches RAM only.
  MPC00 checks every RRAM and RAM transaction: memory is Secure RWX at reset and
  `OVERRIDE[n]` regions (ORed where they overlap) set READ/WRITE/EXECUTE/SECATTR;
  a refused access is a bus fault and raises `EVENTS_MEMACCERR`.
- **RRAMC.** Stores need `CONFIG.WEN`; RRAM is overwritten in place (writing `0xFF` is
  the erase). `CONFIG.WRITEBUFSIZE` buffers stores until `TASKS_COMMITWRITEBUF` and
  `BUFSTATUS.WRITEBUFEMPTY` reports it. `ERASE.ERASEALL` and `--persist` are supported.
- **Peripherals.** CLOCK / OSCILLATORS start events and status, ICACHE (`0xE0082000`),
  GRTC, TIMER00..24, UARTE/SPIM with the nRF54 EasyDMA layout (`TASKS_DMA`, `DMA.RX`,
  `DMA.TX`), CRACEN RNG FIFO fed from the host RNG (`--rng-seed`), FICR, TAMPC.

You can also print the list directly from the executable:

```sh
build/m33mu --cpu list
```

or:

```sh
build/m33mu --cpu ?
```

Use the CPU profile that matches the target firmware's memory map and peripheral set. If you are bringing up a new firmware image and are unsure which target model to use, start from the exact MCU family the firmware was built for and then confirm flash/RAM layout and required peripherals.
