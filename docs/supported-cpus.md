# Supported CPUs

These values are accepted by `--cpu`:

- `stm32h533`
- `stm32h563` (default)
- `stm32h5f4`
- `stm32u585`
- `stm32l552`
- `stm32wba55` — STM32WBA55CG (Cortex-M33, TrustZone, 1 MB single-bank flash)
- `stm32u385` — STM32U385RG (Cortex-M33, TrustZone, 1 MB dual-bank flash, 4 KB pages)
- `stm32c5a3` — STM32C5A3ZG (Cortex-M33 without TrustZone, 1 MB dual-bank flash, 8 KB pages)
- `lpc55s69`
- `mcxw71c` — NXP MCX W71 / MCXW716C (Cortex-M33, TrustZone, FMU0 flash)
- `mcxn947`
- `rw612` — NXP RW612 (Cortex-M33, FlexSPI NOR boot, ROM FlexSPI driver, ELS / PKA crypto offload)
- `mcxa153` — NXP MCX A153 (Cortex-M33 without TrustZone, DSP, FPU or MPU; flash through the ROM API)
- `rw612` — NXP RW612 (Cortex-M33 + ELS / PKA crypto offload)
- `nrf5340`
- `nrf5340net` — Nordic nRF5340 network core (Cortex-M33 without TrustZone, DSP or FPU; flash at 0x01000000, 2 KB NVMC pages)
- `nrf54l15` — Nordic nRF54L15 (Cortex-M33, TrustZone through SPU / MPC)
- `nrf54lm20` — Nordic nRF54LM20A (Cortex-M33, TrustZone through SPU / MPC)
- `psc3m6` — Infineon PSOC Control C3M6 (Cortex-M33, boot ROM flash API)
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

### PSOC Control C3M6 notes

- **Aliases.** Flash and SRAM are visible through the code bus and the system
  bus, each Secure and Non-secure: flash at `0x02000000` / `0x12000000` (CBUS) and
  `0x22000000` / `0x32000000` (SBUS), SRAM at `0x24000000` / `0x34000000` (SBUS) and
  `0x04000000` / `0x14000000` (CBUS), boot ROM at `0x00800000` / `0x10800000`. The
  Non-secure SBUS flash view is accepted by the flash API but not mapped for reads.
- **Always Secure.** The boot ROM starts the application Secure, so an image linked
  at the Secure aliases keeps running there under `--no-tz`.
- **Flash.** Programmed only through the boot ROM API table at `0x1080FF6C`
  (`cyboot_flash_list.h`): `erase_row` (row to 0x00), `program_row` (sets bits) and
  `write_row` (erase then program) on 512-byte rows addressed through the SBUS alias;
  the CBUS address is refused. The other entries return `CYBOOT_FLASH_FAILED`; CPU
  stores into the flash bus-fault. Rows are written back with `--persist`.
- **Clocks.** `PERI_PCLK` dividers start only through `DIV_CMD`; `CLOCK_CTL` resets
  disconnected (`0x3FF`) and an SCB bus-faults until the entry of its destination
  (group 4, index `2 * n`; SCB5 group 6 index 0) selects a started divider. `PERI`
  groups 1 and up reset with `SL_CTL` clear: HSIOM and GPIO (group 1), SCB0-4
  (group 3) and SCB5 (group 5) bus-fault until their slave bit is set.
- **Pins.** 14 ports. `HSIOM.SECURE_PRT[p].NONSECURE_MASK` starts all ones and splits
  each port between the aliases: a pin's HSIOM and GPIO fields are visible through
  the Secure alias when its bit is clear and through the Non-secure alias when it is
  set; the other alias drops writes and reads zero. GPIO `IN` reads back the output
  latch.
- **SCB.** SCB0..SCB5 in UART mode (TX/RX FIFO, a PTY per instance, SCB3 first so
  `--uart-stdout` is the kit console) and SPI master mode on `--spiflash:SPIn`, with
  the select released after each frame unless `SPI_CTRL.SSEL_CONTINUOUS` is set.
  I2C mode, interrupts and a chip select driven from a GPIO are not modelled.

### STM32WBA55 notes

- **Modelled:** RCC (HSI16/HSE32/PLL1 ready bits, SYSCLK selection), PWR (`VOSR`),
  the flash controller, ICACHE, GPIO A/B/C/H, USART1/USART2/LPUART1. Secure aliases at
  `+0x10000000`; no GTZC, RNG or crypto.
- **Flash.** 128 pages of 8 KB programmed by 128-bit flash words: stores with `PG` set
  fill one word (`NSSR.WDW` while it is incomplete), a store to another word first is
  `PGSERR` and loses both. A word that is not erased takes only all-zero data; anything
  else is `PROGERR` with nothing written (RM0493 7.3.7). `NSCR1` is read-only while

### STM32U385 notes

- **Modelled:** RCC at `0x40030C00` (MSIS/MSIK/HSI/HSE ready bits, MSIS range from
  `ICSCR1`, SYSCLK selection), PWR at `0x40030800` (`CR3.REGSEL` -> `SVMSR.REGS`,
  `VOSR` range and booster ready bits), the flash controller, ICACHE, GPIO A-E/G/H,
  USART1/USART3/UART4/UART5/LPUART1. Secure aliases at `+0x10000000`; no GTZC, RNG or
  crypto.
- **Flash.** Two banks of 128 pages of 4 KB programmed by 64-bit double words: stores
  with `PG` set fill one double word (`NSSR.WDW` while it is incomplete), a store to
  another word first is `PGSERR` and loses both. A double word that is not erased takes
  only all-zero data; anything else is `PROGERR` with nothing written (RM0487 7.3.7).
  `PNB` and `MER1`/`MER2` address the bank `BKER` selects. `NSCR` is read-only while
  `LOCK` is set; `OPTKEYR` unlocks `OPTLOCK`, `OBL_LAUNCH` resets the device.

### STM32C5A3 notes

- **No TrustZone.** The part has no SAU and no secure aliases: one view of the flash,
  SRAM and peripherals. `--no-tz` is accepted and changes nothing.
- **Modelled:** RCC at `0x44020C00` (HSIS/HSIDIV3/PSIS/HSE ready bits, SYSCLK selection;
  only the 144 MHz PSI output is known), PWR as a register file, the flash controller,
  ICACHE, GPIO A-H, USART1-3/UART4-5/LPUART1.
- **Flash.** The STM32H5 controller with two banks of 64 pages of 8 KB: the 128-bit write
  buffer (`WBNE`, `INCERR`, `FW`), `PGSERR` for a flash word programmed twice or an erase
  with `WBNE` set, `BKSEL`/`PNB`/`BER` naming physical banks whatever the mapping, `CCR`
  clearing the status flags. With `--dualbank`, an option change (`OPTKEYR`, `OPTSR_PRG`,
  `OPTCR.OPTSTRT`) programs `SWAP_BANK`, shown in `OPTSR_CUR` and applied at the next
  reset, when `OPTCR.SWAP_BANK` tells which mapping is in force.

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

### MCXN947 notes

- **SRAM security.** The AHB secure controller (`AHBSC`, `0x40120000` / `0x50120000`)
  attributes SRAM: `RAMA..RAMH_MEM_RULE` fields (one 2-bit rule per 4 KB) apply once
  `MISC_CTRL_REG`/`MISC_CTRL_DP_REG` enable secure checking (off at reset, so the SAU alone
  decides until then), and strict mode refuses the Secure alias of a Non-secure rule unless
  both copies disable it, as the SDK `tzm_config.c` does. The TRDC MBC registers are a
  register file only.
- **ELS (S50).** The real SFR interface at `0x40054000` / `0x50054000` with the commands
  wolfBoot drives through `mcuxClEls`: `RND_REQ` (DMA into SRAM), `KDELETE`, `HKDF` and,
  with wolfSSL, P-256 `KEYGEN` and `ECSIGN` over a 20-slot keystore; slot 7 holds the ROM
  DICE CDI at reset. Commands complete synchronously; `SYSCON AHBCLKCTRL2/PRESETCTRL2` bit
  18 gate the block and reset enabled. The synthetic PUF/OTP/ELS-command/PKC blocks at
  `0x40024000..0x4002C000` used by the ROM API are unchanged.
- **IFR UUID.** The 128-bit device UUID is readable at `0x01100000` (the bytes
  `ffr_get_uuid` returns).

You can also print the list directly from the executable:

```sh
build/m33mu --cpu list
```

or:

```sh
build/m33mu --cpu ?
```

Use the CPU profile that matches the target firmware's memory map and peripheral set. If you are bringing up a new firmware image and are unsure which target model to use, start from the exact MCU family the firmware was built for and then confirm flash/RAM layout and required peripherals.
