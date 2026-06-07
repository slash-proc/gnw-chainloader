#!/usr/bin/env python3
"""Read the battery ADC state directly over SWD (STM32H7B0, ADC1 channel 4 / PC4).

Diagnostic for the battery-level readout: dumps the ADC peripheral state the
chainloader configured (CR/CFGR/CCR/SMPR/SQR), triggers a fresh conversion, and
prints the raw count plus the same 11000..13000 -> 3.2..4.2V / 0..100% mapping
board.c uses. Also reads the power-good (PA2) and charging (PE7) GPIOs.

  python3 scripts/debug/read_battery.py            # passive: read the firmware's DR
  python3 scripts/debug/read_battery.py --halt -n 32  # halt CPU, take 32 clean samples

Passive mode only reads the firmware's last conversion (DR) so it never disturbs
the running ADC. --halt stops the CPU and drives the ADC from the probe to
characterise the raw signal + noise (min/max/mean), then resumes.
"""
import argparse
import sys
import time

from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

# STM32H7B0 ADC1 / ADC12 common
ADC1 = 0x40022000
ADC12_COMMON = 0x40022300
R_ISR = ADC1 + 0x00
R_CR = ADC1 + 0x08
R_CFGR = ADC1 + 0x0C
R_SMPR1 = ADC1 + 0x14
R_SQR1 = ADC1 + 0x30
R_DR = ADC1 + 0x40
R_CCR = ADC12_COMMON + 0x08

# GPIO IDRs (AHB4): GPIOA 0x58020000, GPIOE 0x58021000; IDR at +0x10
GPIOA_IDR = 0x58020000 + 0x10
GPIOE_IDR = 0x58021000 + 0x10

CR_ADSTART = 1 << 2
CR_ADEN = 1 << 0
ISR_ADRDY = 1 << 0
ISR_EOC = 1 << 2

BATT_LOW = 11000
BATT_FULL = 13000


def rd(be, addr):
    return int.from_bytes(be.read_memory(addr, 4), "little")


def wr(be, addr, val):
    be.write_memory(addr, int(val).to_bytes(4, "little"))


def main():
    ap = argparse.ArgumentParser(description="Read the battery ADC over SWD.")
    ap.add_argument("--halt", action="store_true",
                    help="halt the CPU and take clean probe-driven samples")
    ap.add_argument("-n", "--samples", type=int, default=32,
                    help="number of samples in --halt mode (default 32)")
    args = ap.parse_args()

    be = OpenOCDBackend()
    be.open()
    try:
        cr = rd(be, R_CR)
        isr = rd(be, R_ISR)
        print("--- ADC peripheral state (as configured by the chainloader) ---")
        print(f"  CR    = 0x{cr:08X}  (ADEN={cr & 1}, ADSTART={(cr >> 2) & 1}, "
              f"ADVREGEN={(cr >> 28) & 1}, DEEPPWD={(cr >> 29) & 1}, "
              f"BOOST={(cr >> 8) & 0x3})")
        print(f"  ISR   = 0x{isr:08X}  (ADRDY={isr & 1}, EOC={(isr >> 2) & 1})")
        print(f"  CFGR  = 0x{rd(be, R_CFGR):08X}")
        print(f"  CCR   = 0x{rd(be, R_CCR):08X}  (CKMODE={(rd(be, R_CCR) >> 16) & 0x3})")
        print(f"  SMPR1 = 0x{rd(be, R_SMPR1):08X}")
        print(f"  SQR1  = 0x{rd(be, R_SQR1):08X}")

        a = rd(be, GPIOA_IDR)
        e = rd(be, GPIOE_IDR)
        pgood = (a & (1 << 2)) == 0      # PA2 active-low
        charging = (e & (1 << 7)) == 0   # PE7 active-low

        def pct_of(raw):
            return 0 if raw <= BATT_LOW else min(100, (raw - BATT_LOW) * 100 // (BATT_FULL - BATT_LOW))

        if not args.halt:
            # Passive: just report the firmware's last conversion (DR). Reading DR
            # does NOT disturb the running ADC, so this is safe while the menu runs.
            last = rd(be, R_DR)
            print(f"\n  Firmware reading (DR) = {last}  -> ~{pct_of(last)}%")
            print(f"  power-good (PA2) = {pgood}   charging (PE7) = {charging}")
            print("  (use --halt for a clean noise characterisation)")
            return 0

        if not (cr & CR_ADEN):
            print("\n!! ADC is NOT enabled (ADEN=0) — firmware never initialised it.")
            return 1

        # Clean characterisation: halt the CPU so the firmware can't drive the ADC,
        # then take N back-to-back conversions and report the spread.
        be.halt()
        try:
            samples = []
            for _ in range(args.samples):
                wr(be, R_ISR, ISR_EOC)
                wr(be, R_CR, rd(be, R_CR) | CR_ADSTART)
                for _ in range(1000):
                    if rd(be, R_ISR) & ISR_EOC:
                        break
                    time.sleep(0.0005)
                samples.append(rd(be, R_DR))
        finally:
            be.resume()

        smin, smax = min(samples), max(samples)
        mean = sum(samples) // len(samples)
        print(f"\n--- {len(samples)} probe-driven samples (CPU halted) ---")
        print(f"  min  = {smin}  -> {pct_of(smin)}%")
        print(f"  max  = {smax}  -> {pct_of(smax)}%")
        print(f"  mean = {mean}  -> {pct_of(mean)}%")
        print(f"  spread = {smax - smin} counts ({(smax - smin) * 100 // (BATT_FULL - BATT_LOW)}%)")
        print(f"  power-good (PA2) = {pgood}   charging (PE7) = {charging}")
        return 0
    finally:
        be.close()


if __name__ == "__main__":
    sys.exit(main())
