#!/usr/bin/env python3
"""
🌸 Sakura Audio Player - 1-Click Firmware Flasher for MAX32655
Flashes pre-compiled max32655.bin using OpenOCD and fast ROM FLC RAM flasher in ~12 seconds.
Author: Shivashankar P (Spark Shiva 007)
"""

import os
import sys
import time
import socket
import struct
import subprocess
import shutil

# Locate root directory
TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.abspath(os.path.join(TOOL_DIR, ".."))
BIN_PATH = os.path.join(REPO_DIR, "bin", "max32655.bin")
FLASHER_PATH = os.path.join(REPO_DIR, "bin", "scratch_flasher.bin")

# Candidate paths for OpenOCD
OPENOCD_CANDIDATES = [
    r'C:\Users\Gaming\tools\zephyr-sdk-1.0.1\hosttools\openocd\bin\openocd.exe',
    r'C:\MaximSDK\Tools\OpenOCD\bin\openocd.exe',
    r'C:\Program Files\OpenOCD\bin\openocd.exe',
]
SCRIPTS_CANDIDATES = [
    r'C:\Users\Gaming\tools\zephyr-sdk-1.0.1\hosttools\openocd\share\openocd\scripts',
    r'C:\MaximSDK\Tools\OpenOCD\scripts',
    r'C:\Program Files\OpenOCD\share\openocd\scripts',
]

def find_openocd():
    # Check PATH first
    path_ocd = shutil.which("openocd")
    if path_ocd:
        return path_ocd, None
    for ocd in OPENOCD_CANDIDATES:
        if os.path.exists(ocd):
            for sc in SCRIPTS_CANDIDATES:
                if os.path.exists(sc):
                    return ocd, sc
    return None, None

class OpenOcdTcl:
    def __init__(self, host='127.0.0.1', port=6666):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((host, port))

    def cmd(self, command):
        cmd_str = command + '\x1a'
        self.sock.sendall(cmd_str.encode('utf-8'))
        resp = b''
        while True:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            resp += chunk
            if b'\x1a' in resp:
                resp = resp[:resp.index(b'\x1a')]
                break
        return resp.decode('latin1', errors='replace')

    def get_pc(self):
        res = self.cmd("reg pc")
        for line in res.splitlines():
            if "0x" in line:
                val = line.split("0x")[-1].strip()
                try:
                    return int(val, 16)
                except ValueError:
                    pass
        return None

    def close(self):
        self.sock.close()

def main():
    print("=" * 65)
    print("🌸 SAKURA AUDIO PLAYER — 1-CLICK FIRMWARE FLASHER (MAX32655)")
    print("=" * 65)

    if not os.path.exists(BIN_PATH):
        print(f"[ERROR] Firmware binary not found at: {BIN_PATH}")
        sys.exit(1)
    if not os.path.exists(FLASHER_PATH):
        print(f"[ERROR] Scratch flasher binary not found at: {FLASHER_PATH}")
        sys.exit(1)

    with open(BIN_PATH, "rb") as f:
        bin_data = f.read()

    print(f"Target Firmware: {BIN_PATH} ({len(bin_data):,} bytes)")

    ocd_exe, ocd_scripts = find_openocd()
    if not ocd_exe:
        print("[ERROR] OpenOCD executable not found!")
        print("Please ensure OpenOCD (Maxim SDK or Zephyr SDK) is installed.")
        sys.exit(1)

    print(f"Using OpenOCD: {ocd_exe}")

    # Terminate any existing OpenOCD process
    if sys.platform == "win32":
        subprocess.run('taskkill /F /IM openocd.exe', shell=True, capture_output=True)
    time.sleep(0.5)

    # Launch OpenOCD background daemon
    cmd = [ocd_exe]
    if ocd_scripts:
        cmd.extend(['-s', ocd_scripts])
    cmd.extend([
        '-f', 'interface/cmsis-dap.cfg',
        '-f', 'target/max32655.cfg',
        '-c', 'adapter speed 5000'
    ])

    print("Connecting to CMSIS-DAP debugger on MAX32655...")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.8)

    try:
        tcl = OpenOcdTcl()
        print("[1/5] Connected to OpenOCD.")

        print("[2/5] Halting CPU...")
        tcl.cmd("reset halt")

        print("[3/5] Mass erasing internal flash...")
        t0 = time.time()
        res = tcl.cmd("max32xxx mass_erase 0")
        print(f"      Mass erase finished in {time.time()-t0:.2f}s ({res.strip()})")

        print("[4/5] Loading fast ROM flasher to SRAM...")
        flasher_posix = FLASHER_PATH.replace('\\', '/')
        tcl.cmd(f"load_image {flasher_posix} 0x20000000 bin")

        chunk_size = 16384  # 16 KB
        num_chunks = (len(bin_data) + chunk_size - 1) // chunk_size
        print(f"[5/5] Burning {len(bin_data):,} bytes in {num_chunks} blocks of 16 KB...")

        ram_buf = 0x20001000
        sp_val = 0x2001F000
        flasher_pc = 0x20000001
        bkpt_success = 0x20000066

        temp_chunk_path = os.path.join(TOOL_DIR, "_flash_chunk.bin")
        temp_chunk_posix = temp_chunk_path.replace('\\', '/')

        t_start = time.time()
        for idx in range(num_chunks):
            c = bin_data[idx*chunk_size : (idx+1)*chunk_size]
            if len(c) % 16 != 0:
                c += b'\xff' * (16 - (len(c) % 16))

            with open(temp_chunk_path, 'wb') as f:
                f.write(c)

            dest_flash = 0x10000000 + idx * chunk_size
            num_128 = len(c) // 16

            tcl.cmd(f"load_image {temp_chunk_posix} {hex(ram_buf)} bin")
            tcl.cmd(f"reg r0 {hex(dest_flash)}")
            tcl.cmd(f"reg r1 {hex(ram_buf)}")
            tcl.cmd(f"reg r2 {hex(num_128)}")
            tcl.cmd(f"reg sp {hex(sp_val)}")
            tcl.cmd(f"reg lr {hex(flasher_pc)}")
            tcl.cmd(f"reg pc {hex(flasher_pc)}")

            tcl.cmd("resume")
            tcl.cmd("wait_halt 35000")

            pc = tcl.get_pc()
            if pc != bkpt_success:
                print(f"\n[FAIL] Block {idx+1}/{num_chunks} failed at {hex(dest_flash)}! PC={hex(pc or 0)}")
                sys.exit(1)

            pct = int(((idx + 1) * 100) / num_chunks)
            bar = "█" * (pct // 4) + "░" * (25 - (pct // 4))
            print(f"\r      [{bar}] {pct}% ({idx+1}/{num_chunks} blocks)", end='', flush=True)

        if os.path.exists(temp_chunk_path):
            os.remove(temp_chunk_path)

        elapsed = time.time() - t_start
        print(f"\n\n✓ Burning completed successfully in {elapsed:.2f}s ({(len(bin_data)/1024)/elapsed:.1f} KB/s)!")

        # Trigger hardware GCR reset
        print("Resetting and launching Sakura Audio Player firmware...")
        tcl.cmd("mww 0x40000004 0x80000000")
        time.sleep(0.3)
        tcl.cmd("resume")
        tcl.close()

        print("\n" + "=" * 65)
        print("🌸 SUCCESS: SAKURA AUDIO PLAYER FIRMWARE IS NOW RUNNING!")
        print("=" * 65)

    finally:
        proc.terminate()

if __name__ == "__main__":
    main()

