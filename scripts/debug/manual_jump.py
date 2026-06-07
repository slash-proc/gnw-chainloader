import sys
from pathlib import Path

# Add project root to sys.path to find gnwmanager
sys.path.append(str(Path(__file__).resolve().parent.parent.parent / "gnwmanager"))

from gnwmanager.ocdbackend import OpenOCDBackend

def manual_jump(address):
    backend = OpenOCDBackend()
    
    print(f"Reading vectors from {hex(address)}...")
    sp = backend.read_uint32(address)
    pc = backend.read_uint32(address + 4)
    
    print(f"SP: {hex(sp)}")
    print(f"PC: {hex(pc)}")
    
    print("Resetting and halting...")
    backend.reset_halt()
    
    print("Setting registers...")
    backend.set_reg("msp", sp)
    backend.set_reg("pc", pc)
    
    print("Resuming...")
    backend.resume()
    print("Done.")

if __name__ == "__main__":
    manual_jump(0x08008000)
