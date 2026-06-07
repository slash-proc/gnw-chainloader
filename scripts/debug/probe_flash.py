#!/usr/bin/env python3
import struct
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

OCTOSPI1_BASE = 0x52001400
OCTOSPI_CR = OCTOSPI1_BASE + 0x00
OCTOSPI_DCR1 = OCTOSPI1_BASE + 0x08
OCTOSPI_DCR2 = OCTOSPI1_BASE + 0x0C
OCTOSPI_DCR3 = OCTOSPI1_BASE + 0x10
OCTOSPI_DCR4 = OCTOSPI1_BASE + 0x14
OCTOSPI_SR = OCTOSPI1_BASE + 0x20
OCTOSPI_FCR = OCTOSPI1_BASE + 0x24
OCTOSPI_DLR = OCTOSPI1_BASE + 0x40
OCTOSPI_AR = OCTOSPI1_BASE + 0x48
OCTOSPI_DR = OCTOSPI1_BASE + 0x4C
OCTOSPI_CCR = OCTOSPI1_BASE + 0x100
OCTOSPI_TCR = OCTOSPI1_BASE + 0x108
OCTOSPI_IR = OCTOSPI1_BASE + 0x110
OCTOSPI_ABR = OCTOSPI1_BASE + 0x114

def main():
    backend = OpenOCDBackend()
    backend.open()
    try:
        backend.halt()
        print("--- Probing Flash via OCTOSPI registers ---")
        
        # Read JEDEC ID using raw read memory if possible, or probe registers
        # Let's see: is memory mapped mode active?
        # We can read the OCTOSPI registers to check configuration
        cr = backend.read_uint32(OCTOSPI_CR)
        sr = backend.read_uint32(OCTOSPI_SR)
        ccr = backend.read_uint32(OCTOSPI_CCR)
        print(f"OCTOSPI_CR:  0x{cr:08X}")
        print(f"OCTOSPI_SR:  0x{sr:08X}")
        print(f"OCTOSPI_CCR: 0x{ccr:08X}")

        # Let's try to perform a manual JEDEC ID read command (0x9F) via OCTOSPI register programming
        # 1. Abort any ongoing transfer
        backend.write_uint32(OCTOSPI_CR, cr | (1 << 1)) # Set ABORT bit
        while backend.read_uint32(OCTOSPI_CR) & (1 << 1):
            pass
        
        # 2. Configure CCR for 1-line Instruction (0x9F), 1-line Data, 3 bytes to read
        # Instruction mode = 1-line (1), Data mode = 1-line (1), NbData = 3 (DLR = 2)
        backend.write_uint32(OCTOSPI_DLR, 2) # 3 bytes (n-1)
        
        # CCR configuration:
        # FMODE (bits 26-27): 01 (Indirect Read)
        # DMODE (bits 24-25): 01 (Data on 1 line)
        # ADDSIZE (bits 22-23): 00 (8-bit address size) - not used
        # ADDMODE (bits 20-21): 00 (No address)
        # IMODE (bits 8-9): 01 (Instruction on 1 line)
        # SIOO (bit 12): 0
        ccr_val = (1 << 26) | (1 << 24) | (1 << 8)
        backend.write_uint32(OCTOSPI_CCR, ccr_val)
        
        # 3. Write instruction 0x9F to IR
        backend.write_uint32(OCTOSPI_IR, 0x9F)
        
        # 4. Wait for FTF (FIFO Threshold Flag) or TC (Transfer Complete)
        import time
        t0 = time.time()
        while True:
            sr_val = backend.read_uint32(OCTOSPI_SR)
            if sr_val & (1 << 1): # TC flag
                break
            if time.time() - t0 > 1.0:
                print("Timeout waiting for Transfer Complete.")
                break
        
        # Read data from DR
        dr_val = backend.read_uint32(OCTOSPI_DR)
        print(f"Raw JEDEC ID bytes: 0x{dr_val:08X}")
        # Extract 3 bytes
        b1 = dr_val & 0xFF
        b2 = (dr_val >> 8) & 0xFF
        b3 = (dr_val >> 16) & 0xFF
        print(f"Parsed JEDEC ID: Manufacturer=0x{b1:02X}, Type=0x{b2:02X}, Capacity=0x{b3:02X}")

        # Let's restore the CR
        backend.write_uint32(OCTOSPI_CR, cr)
        backend.resume()
    finally:
        backend.close()

if __name__ == "__main__":
    main()
