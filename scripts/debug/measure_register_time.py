#!/usr/bin/env python3
import time
import sys
from gnwmanager.ocdbackend.openocd_backend import OpenOCDBackend

# RTC Backup Register 2 on H7B0 (TAMP_BKP2R)
RTC_BKP2R = 0x58004508

def main():
    backend = OpenOCDBackend()
    backend.open()
    
    times = []
    num_runs = 20
    
    print(f"Starting {num_runs} boot time measurements...")
    print("-----------------------------------------")
    
    try:
        for i in range(1, num_runs + 1):
            # 1. Full Hardware Reset and Start
            print(f"Resetting for run {i:02d}...", end="\r")
            backend.reset_and_halt()
            backend.resume()
            
            # 2. Wait for boot and asset load
            time.sleep(3)
            
            # 3. Read measurement
            val = backend.read_uint32(RTC_BKP2R)
            times.append(val)
            print(f"Run {i:02d}: {val} ms")
            
            # 4. Clear register for next run
            backend.write_uint32(RTC_BKP2R, 0)
            
        if times:
            valid_times = [t for t in times if t > 0]
            if not valid_times:
                print("No valid measurements recorded (all zero).")
                return
                
            avg = sum(valid_times) / len(valid_times)
            min_t = min(valid_times)
            max_t = max(valid_times)
            
            print("-----------------------------------------")
            print(f"Measurements: {len(valid_times)}/{num_runs}")
            print(f"Average: {avg:.2f} ms")
            print(f"Min:     {min_t} ms")
            print(f"Max:     {max_t} ms")
            
    finally:
        backend.close()

if __name__ == "__main__":
    main()
