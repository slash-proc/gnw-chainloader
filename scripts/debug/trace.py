#!/usr/bin/env python3
"""
Execution control and tracing utility for the Game & Watch (STM32H7B0).
Supports stepping, run-until-address, watchpoints, and basic CPU control.
"""

import argparse
import sys
import time
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

# Capstone disassembler (optional fallback)
try:
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    capstone_available = True
except ImportError:
    capstone_available = False


def get_cpu_info(backend):
    pc = backend.read_register("pc")
    sp = backend.read_register("sp")
    lr = backend.read_register("lr")
    return pc, sp, lr


def check_hardfault(backend):
    try:
        icsr = backend.read_uint32(0xE000ED04)
        if (icsr & 0x1FF) == 3:
            return True
    except Exception:
        pass
    return False


def disassemble_inst(backend, pc):
    # Thumb instructions can be 16-bit or 32-bit.
    # Read 4 bytes at pc (clearing thumb bit)
    pc_val = pc & ~1
    try:
        instr_bytes = backend.read_memory(pc_val, 4)
        if capstone_available:
            # Try to disassemble
            disasm = list(md.disasm(instr_bytes, pc_val))
            if disasm:
                op = disasm[0]
                return f"0x{op.bytes.hex().upper():<8s} {op.mnemonic:<8s} {op.op_str}"
        # Fallback to hex
        word = int.from_bytes(instr_bytes[:2], "little")
        # Check if 32-bit Thumb-2 instruction (opcodes starting with 0b11101, 0b11110, 0b11111)
        if (word & 0xF800) in (0xE800, 0xF000, 0xF800):
            word32 = int.from_bytes(instr_bytes, "little")
            return f"0x{word32:08X} (32-bit)"
        else:
            return f"0x{word:04X} (16-bit)"
    except Exception as e:
        return f"Error reading memory at 0x{pc_val:08X}"


def do_step(backend, steps):
    print(f"Stepping {steps} instructions...")
    for i in range(steps):
        pc, sp, lr = get_cpu_info(backend)
        disasm_str = disassemble_inst(backend, pc)
        print(f"#{i+1:3d}: PC=0x{pc:08X} | SP=0x{sp:08X} | LR=0x{lr:08X} | {disasm_str}")
        
        if check_hardfault(backend):
            print("!!! HARD FAULT DETECTED !!! Tracing aborted.")
            # Dump registers and fault details
            return
            
        try:
            backend("step", decode=False)
        except Exception as e:
            print(f"Error stepping CPU: {e}")
            break


def do_until(backend, target_address, timeout):
    target = target_address & ~1
    print(f"Setting temporary hardware breakpoint at 0x{target:08X}...")
    try:
        backend(f"bp 0x{target:08X} 2 hw", decode=False)
    except Exception as e:
        print(f"Failed to set breakpoint: {e}")
        return

    print("Resuming execution...")
    try:
        backend.resume()
    except Exception as e:
        print(f"Failed to resume execution: {e}")
        # Cleanup breakpoint
        try:
            backend(f"rbp 0x{target:08X}", decode=False)
        except:
            pass
        return

    deadline = time.time() + timeout
    hit = False
    print(f"Waiting up to {timeout} seconds for target PC 0x{target:08X}...")
    try:
        while time.time() < deadline:
            time.sleep(0.1)
            try:
                backend.halt()
            except Exception:
                continue
                
            pc = backend.read_register("pc") & ~1
            if pc == target:
                print(f"\n>>> Breakpoint hit! Halted at PC = 0x{pc:08X}")
                hit = True
                break
                
            # If we didn't hit it yet and target hasn't hardfaulted, resume
            if check_hardfault(backend):
                print("\n!!! HardFault occurred while waiting!")
                break
                
            backend.resume()
    finally:
        # Cleanup breakpoint
        try:
            backend(f"rbp 0x{target:08X}", decode=False)
        except:
            pass
        
        if not hit:
            print("\nTimed out or aborted. Halting CPU...")
            try:
                backend.halt()
            except:
                pass
            pc, sp, lr = get_cpu_info(backend)
            print(f"Halted at: PC=0x{pc:08X} SP=0x{sp:08X}")


def do_watch(backend, watch_address, timeout):
    addr = watch_address
    print(f"Setting write watchpoint at 0x{addr:08X}...")
    try:
        backend(f"wp 0x{addr:08X} 4 w", decode=False)
    except Exception as e:
        print(f"Failed to set watchpoint: {e}")
        return

    print("Resuming execution...")
    try:
        backend.resume()
    except Exception as e:
        print(f"Failed to resume: {e}")
        try:
            backend(f"rwp 0x{addr:08X}", decode=False)
        except:
            pass
        return

    deadline = time.time() + timeout
    hit = False
    print(f"Waiting up to {timeout} seconds for write to 0x{addr:08X}...")
    try:
        while time.time() < deadline:
            time.sleep(0.1)
            try:
                backend.halt()
            except Exception:
                continue
                
            # Check if halted due to watchpoint
            # We check if we are halted
            # If OpenOCD halts, it will report target state.
            # If we can read register PC, we can see if it changed.
            # Actually, OpenOCD halts automatically when watchpoint is triggered.
            # So if we succeeded in halting and we are not resuming, we hit it!
            # Let's verify by checking if the halt status reports watchpoint.
            # To be simple: if the processor is halted on its own, it means watchpoint hit!
            # Since we poll sleep, if we find it halted on its own:
            # we check if it is halted
            # In OpenOCDBackend, we don't have a direct "is_halted" state without trying to halt or read.
            # Let's try to query status or read PC. If it's already halted, wait_and_halt might tell us.
            # Wait, let's just let it run. If we halt and find it stopped:
            # Actually, OpenOCD's wait_halt can block until halt!
            # Let's check: backend("wait_halt 100", decode=False) or similar can wait for halt.
            # Yes! backend("wait_halt 100", decode=False) waits up to 100ms.
            try:
                backend("wait_halt 50", decode=False)
                # If wait_halt completes without exception, it is halted!
                print(f"\n>>> Watchpoint triggered! CPU halted.")
                hit = True
                break
            except Exception:
                # Still running
                pass
    finally:
        try:
            backend(f"rwp 0x{addr:08X}", decode=False)
        except:
            pass
            
        if not hit:
            print("\nTimed out or aborted. Halting CPU...")
            try:
                backend.halt()
            except:
                pass
        pc, sp, lr = get_cpu_info(backend)
        print(f"Current state: PC=0x{pc:08X} SP=0x{sp:08X}")


def main():
    parser = argparse.ArgumentParser(
        description="Execution control and tracing utility for the Game & Watch (STM32H7B0)."
    )
    subparsers = parser.add_subparsers(dest="command", required=True, help="Subcommand to execute")

    # Halt
    subparsers.add_parser("halt", help="Halt the target CPU")
    
    # Resume
    subparsers.add_parser("resume", help="Resume target CPU execution")
    
    # Reset-halt
    subparsers.add_parser("reset-halt", help="Reset and halt the target CPU")

    # Step
    step_parser = subparsers.add_parser("step", help="Single step instructions")
    step_parser.add_argument("count", type=int, nargs="?", default=1, help="Number of instructions to step")
    step_parser.add_argument("--no-halt", action="store_true", help="Do not halt before stepping (assumes already halted)")

    # Until
    until_parser = subparsers.add_parser("until", help="Run until PC hits an address")
    until_parser.add_argument("address", type=lambda x: int(x, 0), help="Hex address to run until")
    until_parser.add_argument("--timeout", type=float, default=10.0, help="Timeout in seconds")
    until_parser.add_argument("--no-halt", action="store_true", help="Do not halt before running (assumes already halted)")

    # Watch
    watch_parser = subparsers.add_parser("watch", help="Set write watchpoint at memory address")
    watch_parser.add_argument("address", type=lambda x: int(x, 0), help="Hex RAM address to watch")
    watch_parser.add_argument("--timeout", type=float, default=15.0, help="Timeout in seconds")
    watch_parser.add_argument("--no-halt", action="store_true", help="Do not halt before running (assumes already halted)")

    args = parser.parse_args()

    backend = OpenOCDBackend()
    backend.open()
    try:
        if args.command == "halt":
            print("Halting CPU...")
            backend.halt()
            pc, sp, lr = get_cpu_info(backend)
            print(f"Halted: PC=0x{pc:08X} SP=0x{sp:08X}")
            
        elif args.command == "resume":
            print("Resuming CPU...")
            backend.resume()
            
        elif args.command == "reset-halt":
            print("Resetting and halting CPU...")
            backend.reset_and_halt()
            pc, sp, lr = get_cpu_info(backend)
            print(f"Halted at Reset: PC=0x{pc:08X} SP=0x{sp:08X}")
            
        elif args.command == "step":
            if not args.no_halt:
                backend.halt()
            do_step(backend, args.count)
            
        elif args.command == "until":
            if not args.no_halt:
                backend.halt()
            do_until(backend, args.address, args.timeout)
            
        elif args.command == "watch":
            if not args.no_halt:
                backend.halt()
            do_watch(backend, args.address, args.timeout)
            
    finally:
        backend.close()


if __name__ == "__main__":
    main()
