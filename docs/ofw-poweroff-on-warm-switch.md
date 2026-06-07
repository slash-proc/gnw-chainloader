# Power-off on warm switch into the stock OFW

> **RESOLVED — hardware-verified on Mario and Zelda (2026-05-31).** See the
> **Resolution** section immediately below. The investigation history further down
> is kept for the record; the earlier "ROOT CAUSE" / Session-2/3 sections chased the
> *standby arm* of the handshake (which only ever produced "alive but headless") —
> the actual display-init gate is `PWR_CPUCR.SBF`.

## ✅ Resolution — the PWR_CPUCR.SBF power-on gate

**Confirmed root cause.** The stock OFW's pre-loop boot-mode selector calls a tiny
gate that reads `PWR_CPUCR.SBF` (bit 6, the standby-wake flag). SBF is hardware-set
*only* when the chip woke from standby (a genuine power-button power-on) and is `0`
after a warm `NVIC_SystemReset` — our bank swap into the OFW, or retro-go's
reboot-to-stock. When the gate returns 0, app `main` diverts into a headless path
that skips display init, and the in-loop **state-6** handler then enters STANDBY
because the power button isn't held — the dark-screen / "press power" behavior.

**The fix — two byte-patches per firmware**, applied in the OFW patch scripts
(`gnwmanager/.../gnw_patch/{mario,zelda}.py`, right after the `read_buttons` hook):

| firmware | SBF gate fn | NOP its `bpl` | state-6 handler | `bpl`→`b` |
|---|---|---|---|---|
| Mario | `0x08006030` | `0x08006038` — `nop(0x6038,1)` | `0x08005EF4` | `0x08005F08` — `b(0x5F08,0x5F2A)` |
| Zelda | `0x0800EBC8` | `0x0800EBD0` — `nop(0xEBD0,1)` | `0x0800EA8C` | `0x0800EAA0` — `b(0xEAA0,0xEAC2)` |

- Patch 1 NOPs the SBF gate's `bpl` so it always falls through to "return 1"
  ("woke from standby"), forcing the full cold-boot init path on a warm reset.
- Patch 2 turns the state-6 standby branch into an unconditional skip, so a warm
  entry stays alive instead of powering off.
- Both are **no-ops on a genuine cold boot** (SBF=1 *and* the power button is held),
  so they change only the warm case. They use raw stock offsets and live in
  verified-1:1 code regions — independent of the `bootloader` entry-symbol naming.

**Hardware verification** (`scripts/debug/probe_warmboot_display.py <mario|zelda>`):
driving SWITCH → BOOT ACTIVE OFW over the probe *without* a power press, both
consoles came up — core alive (reset vec `0x08018101` / `0x0801B3E1`),
`LTDC GCR.LTDCEN=1`, `GPIOD PD4` high, `PWR_CPUCR.SBF=0` (confirming a genuine warm
boot), and the OFW splash rendered (`build/warmboot_{mario,zelda}.png`).

**Still to re-verify (regression, low risk):** a genuine cold power-on still boots;
normal idle-sleep / manual power-off still work (only the *boot-time* state-6
standby was touched); and Left+Game still escapes the running OFW to the launcher.

### Tooling added
- `scripts/debug/disasm_stock_buttons.py` gained PC-relative literal-pool resolution
  (`KNOWN_ADDRS`), `--words N` (dump a jump table), and `--file PATH` (disassemble an
  arbitrary image — e.g. a patched binary, to verify applied bytes).
- `scripts/debug/probe_warmboot_display.py` — drives the warm switch and reads
  LTDC/PD4 to confirm display init on its own (per-console `mario`/`zelda`).

## Symptom

Entering the stock original firmware (OFW) via a **warm reset** powers the device
**completely off** — the screen goes black, the debug probe loses the target
entirely, and a physical **power-button press** is required to bring the OFW up.

- **Cold boot → BOOT ACTIVE OFW: works** every time (the OFW starts normally).
- **Warm switch → OFW: dies.** Reproduced via:
  - our chainloader: menu → BOOT ACTIVE OFW (bank swap + `NVIC_SystemReset`), and
  - a *stock* retro-go setup: the `INTFLASH_BANK==2` "Reboot to Original system"
    option, whose `soft_reset_do()` is just
    `HAL_RTCEx_BKUPWrite(DR0, 0); NVIC_SystemReset();` — **no bank swap at all.**

**This is not specific to this project and not a regression we introduced.** It
reproduces in stock retro-go. The common factor across every repro is: *the stock
OFW begins executing from a warm `NVIC_SystemReset` rather than a genuine
power-button power-on.*

## What it is NOT (ruled out, with evidence)

| # | Theory | How it was killed |
|---|---|---|
| 1 | Stale boot-magic words misrouting | Full register trace right before the switch: `SRAM_MAGIC/TARGET/SHADOW`, `RG_MAGIC`, `BKP0R`, `BKP3R` all `0` / clean. |
| 2 | PD1/PD4 power latch released by reset | PD1/PD4 are the **LCD + external-flash rails**, not the MCU/main supply hold. Re-asserting them in the patch's `chainloader()` hook (twice, different placements) changed nothing. |
| 3 | Stub's POR→standby branch (`stub_main.c:38`) | That branch gates on `RCC_RSR_PORRSTF`; the trace shows `PORRSTF` is **not** set at the menu (`RSR = SFTRSTF|PINRSTF|D1RSTF`). |
| 4 | Stock OFW reset handler / SystemInit power gate | Disassembled stock Mario reset handler (`0x08017A44`) → 4-instruction veneer → `SystemInit` (`0x08017B20`) is **vanilla CMSIS** (FPU enable, RCC reset, VTOR). No power/standby/GPIO-latch logic. |
| 5 | Stale `PWR`/`RCC` wake/standby flags confusing the OFW | Experiment: scrubbed `RCC->RSR` (RMVF), `PWR->WKUPCR` (clear WKUPC1..6), `PWR->CPUCR` (CSSF) in the patch right before starting the stock app. **No change.** (Note: a real power-on's `SBF`/`WKUPF` are hardware-set read-only — software can clear but not *forge* a power-on, so this only tested the "stale state confuses it" direction.) |

## Hard facts (measured, trust these)

1. **It is a true full power-down**, not standby and not a hang. Across the switch,
   the debug AP cannot read **any** domain for 12+ reconnect attempts (~15 s) —
   including the always-on D3/backup registers (`PWR_*`, `RCC->RSR` at `0x5802xxxx`).
   A standby would leave D3 readable; a hang would leave the AP alive. Everything
   dark ⇒ the hardware power latch released.
   - *Caveat:* the stock OFW won't set the `DBGMCU` standby-keep bits, so a plain
     STANDBY would *also* drop the probe. The distinguishing evidence is losing the
     **backup-domain** registers, which standby would not.
2. **Baseline at the menu (reached via a real power-on), healthy:**
   `RCC->RSR = 0x01480000` (`SFTRSTF|PINRSTF|D1RSTF`, **no PORRSTF**),
   `PWR->CPUCR = 0`, `PWR->WKUPFR = 0`, `PWR->WKUPEPR = 0x1C0`, all magic/backup cells `0`,
   banks unswapped (`OPTSR_CUR = 0x1006AAF0`).
   - Note `SFTRSTF` is present on the **good** path too, so "SFTRSTF set" is *not*
     the discriminator.
3. Cold power-on → OFW: works. Warm reset → OFW: powers off.
4. A power-button press revives it (supplies the genuine WKUP1 wake).
5. The hand-off itself succeeds — after revival the OFW runs fine, so the swap /
   reset target is correct; the issue is purely the power state at OFW entry.

## Deep-dive findings (OFW disassembly session)

Disassembled the stock Mario boot chain from the backup image:
`reset 0x08017A44` → veneer → `SystemInit 0x08017B20` (plain CMSIS) →
`main 0x08017FF8` (C-runtime: FPU init, `__libc_init_array`, `.data` unpack) →
**real app main `0x0800721C`**.

In the app main there is a genuine **reset-source check** at `0x08007338`:

```
0x08007338: ldr  r0, [pc,#0x60]   ; r0 = 0x58024530  (a mirror of RCC->RSR)
0x0800733A: ldr  r1, [r0]
0x0800733C: tst  r1, #0x11000000  ; bit24 SFTRSTF | bit28 WWDG1RSTF
0x08007340: bx   lr               ; Z=1 iff neither set
```
called at `0x08007312` (`bl; beq 0x800732e`), where the not-equal branch runs
`0x8006366 / delay 0x96 via 0x8008408 / 0x8006384`. A nearby check at `0x08007300`
reads `PWR->WKUPFR` (`0x58024824`) and tests bit1 (WKUPF1 = power button).

**This looked like the smoking gun but is NOT the (sole) trigger — proven:**

- `0x58024530` and the canonical `RCC->RSR` (`0x580244D0`, RCC+0xD0) are the **same
  physical register** (verified live: writing RMVF to `0x580244D0` cleared
  `0x58024530` too — both went `0x01480000` → `0x00000000`).
- **RMVF works**: a single `RSR |= (1<<16)` live-cleared SFTRSTF and all
  reset-cause flags. So experiment #5 (which cleared `0x580244D0`) used the *right*
  register and the clear *did* take — yet the OFW still powered off.
- Therefore: clearing RSR/SFTRSTF before starting the OFW does **not** prevent the
  power-off. The actual power-down trigger is **elsewhere** (the `0x08007338` check
  is real but either off the failing path, or only one of several conditions).
- Also note: at the menu (reached via the chainloader) RSR already shows **SFTRSTF
  set** (`0x01480000` = CDRSTF|PINRSTF|SFTRSTF), so "SFTRSTF set" is not by itself
  the cold-vs-warm discriminator — another reason RSR isn't the gate.

**Still not captured (the one cheap observation that would help most):** RSR's
value immediately after a *true cold power-cycle* boot to the menu, to compare
against the warm-reset value and see what actually differs. Left RSR = 0 (cleared)
at end of session.

## ROOT CAUSE FOUND (OFW disassembly, session 2)

The stock OFW enters **STANDBY** (= "device off", wakes on the WKUP1 power-button
pin) via a **power-button-presence check** that a warm reset fails.

**The shutdown primitive** (`0x08003198`, Mario):
```
ldr r0,=PWR_CPUCR; orr [r0],#5  ; PDDS_D1|PDDS_D3 (deep power-down both domains)
ldr r0,=SCB_SCR;   orr [r0],#4  ; SLEEPDEEP
dsb; isb; wfi                   ; -> STANDBY
```
Reached via the shutdown wrappers `0x08009D7A` / `0x08009D58` (which arm WKUP via
`PWR_WKUPEPR |= 0x200`).

**The gate** — the state-machine handler at `0x08005EF4` (Mario), dispatched from
the main game loop (`0x080072EC` → jump table `0x08005E68`):
```
0x08005F00: ldr r1,=0x58020010   ; GPIOA->IDR
0x08005F04: ldr r2,[r1]
0x08005F06: lsls r0,r2,#0x1f      ; isolate GPIOA bit0  (== BTN_PWR: GPIOA pin 0, active-low)
0x08005F08: bpl 0x8005f2a         ; button HELD (bit0=0,N=0) -> branch, SKIP shutdown
            ... bl 0x8009d7a      ; button NOT held -> SHUTDOWN (standby)
```
On a genuine power-on the user is physically holding the power button → GPIOA.0
low → check passes → OFW stays on. On a **warm reset** (our swap, or RG's
`soft_reset`) nobody is holding it → GPIOA.0 high → OFW enters standby. Pressing
power afterwards satisfies the same check → it comes up. This is the
cold-works/warm-dies behavior, fully explained. The check reads **raw GPIOA->IDR**,
not `read_buttons`, so the remote-input shadow cell does NOT cover it.

(There is a sibling handler at `0x08005E98` with the same shape; the dispatch is a
6-entry jump table at `0x08005E70` indexed by a runtime state byte. Confirm which
handler(s) run on the boot-path before patching.)

### Fix options
1. **1-byte OFW patch:** at `0x08005F08` change `bpl 0x8005f2a` (`0f d5`) to an
   unconditional `b 0x8005f2a` (`0f e0`) so the power-button check always skips
   shutdown. Same offset encoding, target unchanged. Must verify it doesn't also
   disable legitimate idle/standby power-off, and cover the sibling handler +
   Zelda's equivalent addresses (re-run the scan on the Zelda backup).
2. The shadow-cell route does **not** work here (raw GPIO read, bypasses
   read_buttons).

### Tools added this session
- `scripts/debug/scan_ofw_power.py <mario|zelda>` — scans a backup for WFI/WFE and
  literal-pool refs to PWR/SCB/RCC/GPIO (finds the standby machinery).
- `scripts/debug/find_callers.py <mario|zelda> <0xADDR>` — decodes Thumb BL/B.W and
  lists every call site targeting an address (walk the call graph).

## Session 3 — live tracing: it's a HEADLESS BOOT MODE, not a power-off

Drove the switch and **halted the live OFW** (instead of inferring from the dead
probe). Findings that reframe everything:

- **The OFW is ALIVE and running its main game loop** after the warm switch — PC
  samples vary across `0x080072E0 / 0x080061DC / 0x08009E5A / 0x08003148 /
  0x0800A672` (the loop body), no HardFault (ICSR=0), **not** in standby
  (PWR_CPUCR SBF/STOPF = 0, SCB_SCR SLEEPDEEP = 0). It is **not powered off** here.
- **The display is simply never initialized:** GPIOD ODR shows PD4 (3.3V LCD) LOW,
  LTDC `GCR.LTDCEN = 0`. The OFW runs **headless**. Forcing PD4 high live → it
  sticks (OFW does not clear it), so the OFW isn't fighting the display; it just
  never ran its display-init path.
- So the warm-boot symptom is "**OFW boots into a minimal/headless background mode
  and skips display+game init**", which on the original hardware likely ends in
  standby (hence the earlier full-power-off readings). Same root family as the
  power-button handshake.

**The state machine:** app `main 0x0800721C` sets a state byte at **`0x20001034`**
to **6** at entry (`0x08007342: movs r0,#6; strb r0,[0x20001034]`), then the main
loop dispatches on it via a 6-entry jump table at `0x08005E70` (`0x08005E68`:
`adr r1,#4; ldr r0,[r1,r0,lsl#2]; bx r0`) — i.e. **valid states are 0-5; state 6
is out of range.** Live, the OFW sits in state **6**. On a *cold* boot the user
holds power (GPIOA->IDR bit0 low) and something advances state 6 → a normal state
that inits the display; on a *warm* reset the button isn't held (`IDR bit0=1`) so
it never advances → headless. This is the power-on handshake.

**My `bpl→b` patch (reverted in source) DID take effect** — confirmed live:
`0x08005F08` (low-mapped, executing) reads `0xE00F` (`b`), my patch. It neutered
the state-handler's *standby arm* (so: full-power-off → alive-headless) but did
**not** fix the *advance-out-of-state-6* / display-init path. Wrong arm of the
same handshake.

**Forcing state 0-5 live (mid-loop) does NOT bring up the display** — display init
is a one-time pre-loop boot step that's already been skipped, so late state edits
can't recover it. The real gate is the **early, one-time boot-mode decision before
the main loop**, which I could not catch live (the swap-reset clears HW
breakpoints; reconnect-after-reset is too slow to plant one before the decision).

### CRITICAL methodology notes (cost real time this session)
- **The patched OFW in bank2 is NOT byte-identical to `backup/internal_flash_
  backup_mario.bin` at all offsets.** Code regions are 1:1 (verified: `0x9E9C`,
  `0xA130`, `0xAA90`, `0x5E68`, `0x5EF4`, `0x3198`, `0x721C` all match live), but
  some regions are **relocated/compressed by the patch** (`move_to_compressed_
  memory`) — e.g. `0x5F00` reads as compressed data in the high window. Always
  **verify a region is live-1:1 before trusting backup-derived disassembly**, and
  read **low-mapped** addresses (`0x0800xxxx`, what the CPU fetches after swap) —
  the high window (`0x0810xxxx`) is NOT the executing image.
- The patch byte tooling works: `self.internal.replace(0xOFFSET, byte, size=1)`
  in `gnwmanager/.../mario.py`, OFFSET = flash-relative (`0x5F09` → `0x08005F09`).

### Next-session plan
1. Find the **early, pre-loop** code that advances state `6 → normal` (cold path),
   gated on the power button. Search the live-1:1 region for reads of `0x20001034`
   and `cmp #6`, and for the GPIOA-bit0 (`lsls #0x1f` / `bpl`) power-button tests.
2. Patch *that* gate (force-advance regardless of button) rather than the standby
   arm — then the display-init path should run.
3. Replicate for Zelda (standby entry `0x080036A4`, gate TBD).

## Leading unexplored hypothesis (superseded — see ROOT CAUSE + Session 3 above)

The stock **OFW application's early `main`** (NOT the reset vector, NOT SystemInit
— both cleared) deliberately powers the device down when it detects it was *not*
started by a genuine power-button wake. A real Game & Watch behaves exactly this
way: it only turns on via the power button, and turns itself back off if it wakes
without that condition. That check would read state that differs between a cold
power-on and a warm `NVIC_SystemReset` — candidates: `PWR_CPUCR.SBF` (standby
flag, hardware-set, *cannot be forged in software*), a wake-pin flag, an RTC
register, or a specific backup register the OFW owns.

If true, the fix options are constrained: software can't fake `SBF`, so we'd need
to either (a) find a different gate the OFW uses that *is* satisfiable, or (b)
route the OFW entry through an actual standby+WKUP cycle (enter standby, let the
power button wake straight into the OFW), or (c) accept "press power after
switching to OFW" as documented behavior (what upstream effectively does).

## Tooling / addresses for the next session

- `scripts/debug/probe_ofw_switch.py` — drives menu → BOOT ACTIVE OFW over the
  remote-input shadow cell and traces PWR/RCC/magic/bank state across the reset
  (reconnects the probe each attempt). Requires a `REMOTE_INPUT` build.
- `scripts/debug/disasm_stock_buttons.py <mario|zelda> <read_buttons|reset|0xADDR>`
  — disassembles any function from the stock backup images
  (`backup/internal_flash_backup_{mario,zelda}.bin`, raw flash from `0x08000000`).
- Stock entry points: Mario reset `0x08017A44` → SystemInit `0x08017B20`; Zelda
  reset `0x0801AD48`. `STOCK_READ_BUTTONS`: Mario `0x08010D48`, Zelda `0x08016808`.
- Live PWR/RCC map (STM32H7B0): `RCC->RSR 0x580244D0` (RMVF bit16);
  `PWR_CR1 0x58024800`, `CSR1 04`, `CR2 08`, `CR3 0C`, `CPUCR 10` (SBF bit6,
  STOPF bit5, CSSF bit9), `SRDCR 18`, `WKUPCR 20`, `WKUPFR 24`, `WKUPEPR 28`.

## Status

**Resolved** for the warm-switch display-init failure on both Mario and Zelda —
hardware-verified 2026-05-31 (see the Resolution section at the top). The fix lives
in the OFW patch scripts (gnwmanager commit `750f935`); our patch entry symbol was
renamed `chainloader`→`bootloader` to build against unmodified gnwmanager (reset
vector unchanged). The former workaround (press power after switching to the OFW) is
no longer needed.

Remaining: re-verify no regression on cold boot, normal power-off, and the
Left+Game escape from the running OFW.
