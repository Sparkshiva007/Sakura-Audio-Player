#!/usr/bin/env python3
"""
🌸 Sakura Music Manager - MAX32655 Multi-Mode Audio Management Suite
Theme: Premium Sakura Dark Obsidian Edition
Capabilities:
  1. ⚡ Direct MicroSD Mode: Blazing-fast UHS direct transfer (~30-80 MB/s, 1-2 sec/song)
  2. 🔌 USB Cable Mode: On-device serial transfer via MAX32655 USB Cable (115200 Baud)
Author: Google DeepMind Antigravity Pair Programmer
"""

import sys
import os
import time
import shutil
import string
import ctypes
import serial
import serial.tools.list_ports
from PyQt6 import QtCore, QtGui, QtWidgets
from PyQt6.QtCore import Qt, QThread, pyqtSignal, QSize, QUrl
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QComboBox, QTableWidget, QTableWidgetItem,
    QProgressBar, QFileDialog, QMessageBox, QFrame, QHeaderView,
    QAbstractItemView, QButtonGroup, QRadioButton
)

# =============================================================================
# CRC16-CCITT Calculation (Polynomial 0x1021, Initial 0xFFFF)
# =============================================================================
def calc_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

# =============================================================================
# Windows Drive Detection Helper
# =============================================================================
def detect_windows_drives():
    """
    Returns list of tuples: (drive_root, drive_label, drive_type_str, total_gb, free_gb, is_removable)
    """
    drives = []
    try:
        bitmask = ctypes.windll.kernel32.GetLogicalDrives()
        for letter in string.ascii_uppercase:
            if bitmask & 1:
                drive_path = f"{letter}:\\"
                dtype = ctypes.windll.kernel32.GetDriveTypeW(drive_path)
                # 2 = DRIVE_REMOVABLE, 3 = DRIVE_FIXED, 4 = DRIVE_REMOTE, 5 = DRIVE_CDROM, 6 = DRIVE_RAMDISK
                vol_buf = ctypes.create_unicode_buffer(261)
                fs_buf = ctypes.create_unicode_buffer(261)
                label = ""
                fs = ""
                if ctypes.windll.kernel32.GetVolumeInformationW(
                    drive_path, vol_buf, ctypes.sizeof(vol_buf),
                    None, None, None, fs_buf, ctypes.sizeof(fs_buf)
                ):
                    label = vol_buf.value
                    fs = fs_buf.value

                tot_gb = 0.0
                fre_gb = 0.0
                try:
                    tot, usd, fre = shutil.disk_usage(drive_path)
                    tot_gb = tot / (1024 ** 3)
                    fre_gb = fre / (1024 ** 3)
                except Exception:
                    pass

                type_desc = "Removable" if dtype == 2 else "Drive"
                is_removable = (dtype == 2)
                drives.append((drive_path, label, fs, type_desc, tot_gb, fre_gb, is_removable))
            bitmask >>= 1
    except Exception as e:
        print("[DRIVES] Error detecting drives:", e)
    return drives

# =============================================================================
# Direct MicroSD Transfer Worker Thread (High-Speed Local I/O)
# =============================================================================
class DirectSdWorker(QThread):
    sig_log = pyqtSignal(str)
    sig_storage = pyqtSignal(int, int) # total_mb, free_mb
    sig_file_list = pyqtSignal(list)   # list of (name, size_bytes)
    sig_delete_done = pyqtSignal(bool, str)
    sig_file_progress = pyqtSignal(int, str, float) # pct, filename, speed_mbps
    sig_batch_progress = pyqtSignal(int, int, int)  # current_file_idx, total_files, total_pct
    sig_upload_finished = pyqtSignal(bool, str)

    def __init__(self):
        super().__init__()
        self.target_dir = ""
        self.action = None
        self.action_args = ()
        self.abort_requested = False

    def set_target_dir(self, directory: str):
        self.target_dir = directory

    def queue_action(self, action, *args):
        self.action = action
        self.action_args = args
        self.abort_requested = False
        self.start()

    def request_abort(self):
        self.abort_requested = True

    def run(self):
        if not self.target_dir or not os.path.exists(self.target_dir):
            self.sig_upload_finished.emit(False, "MicroSD folder path does not exist!")
            return

        action = self.action
        args = self.action_args

        if action == "STORAGE":
            self.handle_storage()
        elif action == "LIST":
            self.handle_list()
        elif action == "DELETE":
            self.handle_delete(args[0])
        elif action == "UPLOAD_BATCH":
            self.handle_upload_batch(args[0])

    def handle_storage(self):
        try:
            tot, usd, fre = shutil.disk_usage(self.target_dir)
            total_mb = int(tot / (1024 * 1024))
            free_mb = int(fre / (1024 * 1024))
            self.sig_storage.emit(total_mb, free_mb)
        except Exception as e:
            self.sig_log.emit(f"MicroSD storage query error: {e}")

    def handle_list(self):
        try:
            files = []
            for entry in os.scandir(self.target_dir):
                if entry.is_file():
                    lower = entry.name.lower()
                    if lower.endswith(".mp3") or lower.endswith(".wav"):
                        files.append((entry.name, entry.stat().st_size))
            files.sort(key=lambda x: x[0].lower())
            self.sig_file_list.emit(files)
        except Exception as e:
            self.sig_log.emit(f"MicroSD list error: {e}")

    def handle_delete(self, filename):
        try:
            file_path = os.path.join(self.target_dir, filename)
            if os.path.exists(file_path):
                os.remove(file_path)
                self.sig_delete_done.emit(True, filename)
                self.handle_storage()
                self.handle_list()
            else:
                self.sig_delete_done.emit(False, f"File '{filename}' not found.")
        except Exception as e:
            self.sig_delete_done.emit(False, str(e))

    def handle_upload_batch(self, file_paths):
        total_files = len(file_paths)
        total_bytes_all = sum(os.path.getsize(fp) for fp in file_paths)
        uploaded_bytes_all = 0
        BUFFER_SIZE = 1024 * 1024  # 1 MB high-speed streaming chunk

        t_batch_start = time.time()
        for idx, fp in enumerate(file_paths):
            if self.abort_requested:
                self.sig_upload_finished.emit(False, "Transfer cancelled by user.")
                return

            fname = os.path.basename(fp)
            fsize = os.path.getsize(fp)
            dest_path = os.path.join(self.target_dir, fname)

            self.sig_log.emit(f"⚡ [DIRECT SD] [{idx+1}/{total_files}] Blazing copy: {fname} ({fsize:,} bytes)")
            
            t_file_start = time.time()
            copied_file_bytes = 0

            try:
                with open(fp, "rb") as f_src, open(dest_path, "wb") as f_dst:
                    while True:
                        if self.abort_requested:
                            f_dst.close()
                            if os.path.exists(dest_path):
                                os.remove(dest_path)
                            self.sig_upload_finished.emit(False, "Transfer cancelled by user.")
                            return

                        chunk = f_src.read(BUFFER_SIZE)
                        if not chunk:
                            break
                        f_dst.write(chunk)
                        copied_file_bytes += len(chunk)
                        uploaded_bytes_all += len(chunk)

                        # Calculate instantaneous speed in MB/s
                        dt_file = max(0.001, time.time() - t_file_start)
                        speed_mbps = (copied_file_bytes / (1024 * 1024)) / dt_file

                        file_pct = int((copied_file_bytes * 100) / max(1, fsize))
                        batch_pct = int((uploaded_bytes_all * 100) / max(1, total_bytes_all))

                        self.sig_file_progress.emit(file_pct, fname, speed_mbps)
                        self.sig_batch_progress.emit(idx + 1, total_files, batch_pct)

                    f_dst.flush()
                    os.fsync(f_dst.fileno())

                dt_total = max(0.001, time.time() - t_file_start)
                avg_speed = (fsize / (1024 * 1024)) / dt_total
                self.sig_log.emit(f"✓ Copied {fname} in {dt_total:.2f}s ({avg_speed:.1f} MB/s)")

            except Exception as e:
                self.sig_upload_finished.emit(False, f"Transfer error for {fname}: {e}")
                return

        total_elapsed = max(0.001, time.time() - t_batch_start)
        overall_mbps = (total_bytes_all / (1024 * 1024)) / total_elapsed
        self.sig_upload_finished.emit(
            True,
            f"Successfully transferred {total_files} song(s) in {total_elapsed:.2f}s ({overall_mbps:.1f} MB/s)!"
        )
        self.handle_storage()
        self.handle_list()

# =============================================================================
# Serial Communication Engine (USB Cable / MAX32655 UART)
# =============================================================================
class SerialWorker(QThread):
    sig_log = pyqtSignal(str)
    sig_connected = pyqtSignal(bool, str)
    sig_storage = pyqtSignal(int, int) # total_mb, free_mb
    sig_file_list = pyqtSignal(list)   # list of (name, size_bytes)
    sig_delete_done = pyqtSignal(bool, str)
    sig_file_progress = pyqtSignal(int, str, float) # pct, filename, speed_kbps
    sig_batch_progress = pyqtSignal(int, int, int)  # current_file_idx, total_files, total_pct
    sig_upload_finished = pyqtSignal(bool, str)
    sig_disconnected_ack = pyqtSignal()

    def __init__(self):
        super().__init__()
        self.ser = None
        self.port = "COM21"
        self.baud = 115200
        self.action = None
        self.action_args = ()
        self.abort_requested = False

    def open_port(self, port, baud=115200):
        try:
            if self.ser and self.ser.is_open:
                self.ser.close()
            self.port = port
            self.baud = baud
            self.ser = serial.Serial(port, baud, timeout=1.5, write_timeout=1.5)
            self.ser.reset_input_buffer()
            self.ser.reset_output_buffer()
            return True
        except Exception as e:
            self.sig_log.emit(f"Failed to open port {port}: {e}")
            return False

    def close_port(self):
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def queue_action(self, action, *args):
        self.action = action
        self.action_args = args
        self.abort_requested = False
        self.start()

    def request_abort(self):
        self.abort_requested = True

    def read_line(self, timeout=2.0) -> str:
        if not self.ser or not self.ser.is_open:
            return ""
        t0 = time.time()
        line = bytearray()
        while time.time() - t0 < timeout:
            if self.ser.in_waiting > 0:
                c = self.ser.read(1)
                if not c:
                    continue
                if c == b'\n':
                    return line.decode('latin1', errors='replace').strip()
                if c != b'\r':
                    line.extend(c)
            else:
                time.sleep(0.005)
        return line.decode('latin1', errors='replace').strip()

    def run(self):
        if not self.ser or not self.ser.is_open:
            self.sig_connected.emit(False, "Port not opened")
            return

        action = self.action
        args = self.action_args

        if action == "PING":
            self.handle_ping()
        elif action == "STORAGE":
            self.handle_storage()
        elif action == "LIST":
            self.handle_list()
        elif action == "DELETE":
            self.handle_delete(args[0])
        elif action == "UPLOAD_BATCH":
            self.handle_upload_batch(args[0])
        elif action == "DISCONNECT":
            self.handle_disconnect()

    def handle_ping(self):
        try:
            self.ser.reset_input_buffer()
            self.ser.write(b"PING\n")
            self.ser.flush()
            t0 = time.time()
            found_pong = False
            while time.time() - t0 < 3.5:
                resp = self.read_line(timeout=1.0)
                if not resp:
                    continue
                if "PONG" in resp:
                    found_pong = True
                    break
            if found_pong:
                self.sig_connected.emit(True, f"Connected to {self.port}")
                self.handle_storage()
                self.handle_list()
            else:
                self.sig_connected.emit(False, "Device did not respond to PING")
        except Exception as e:
            self.sig_connected.emit(False, f"Connection error: {e}")

    def handle_storage(self):
        try:
            self.ser.write(b"STORAGE\n")
            self.ser.flush()
            t0 = time.time()
            while time.time() - t0 < 3.5:
                resp = self.read_line(timeout=1.0)
                if not resp:
                    continue
                if resp.startswith("STORAGE:"):
                    parts = resp.split(":")
                    tot = int(parts[1])
                    fre = int(parts[2])
                    self.sig_storage.emit(tot, fre)
                    break
        except Exception as e:
            self.sig_log.emit(f"Storage query error: {e}")

    def handle_list(self):
        try:
            self.ser.write(b"LIST\n")
            self.ser.flush()
            files = []
            t0 = time.time()
            while time.time() - t0 < 6.0:
                line = self.read_line(timeout=1.0)
                if not line:
                    continue
                if line == "LIST_END":
                    break
                if line.startswith("FILE:"):
                    parts = line.split(":")
                    if len(parts) >= 3:
                        fname = parts[1]
                        fsize = int(parts[2])
                        files.append((fname, fsize))
            self.sig_file_list.emit(files)
        except Exception as e:
            self.sig_log.emit(f"List files error: {e}")

    def handle_delete(self, filename):
        try:
            cmd = f"DELETE {filename}\n".encode('latin1')
            self.ser.write(cmd)
            self.ser.flush()
            t0 = time.time()
            while time.time() - t0 < 3.5:
                resp = self.read_line(timeout=1.0)
                if not resp:
                    continue
                if "DELETE_OK" in resp:
                    self.sig_delete_done.emit(True, filename)
                    self.handle_storage()
                    self.handle_list()
                    return
                elif "DELETE_ERR" in resp:
                    self.sig_delete_done.emit(False, f"Delete failed: {resp}")
                    return
            self.sig_delete_done.emit(False, "Delete timed out")
        except Exception as e:
            self.sig_delete_done.emit(False, str(e))

    def handle_upload_batch(self, file_paths):
        total_files = len(file_paths)
        total_bytes_all = sum(os.path.getsize(fp) for fp in file_paths)
        uploaded_bytes_all = 0
        CHUNK_SIZE = 4096 # Enhanced 4KB chunk size

        for idx, fp in enumerate(file_paths):
            if self.abort_requested:
                self.ser.write(b"UPLOAD_ABORT\n")
                self.ser.flush()
                self.sig_upload_finished.emit(False, "Upload cancelled by user.")
                return

            fname = os.path.basename(fp)
            fsize = os.path.getsize(fp)

            self.sig_log.emit(f"[{idx+1}/{total_files}] Preparing upload: {fname} ({fsize:,} bytes)")
            
            # Send UPLOAD_START <filename> <filesize>
            cmd = f"UPLOAD_START {fname} {fsize}\n".encode('latin1')
            self.ser.write(cmd)
            self.ser.flush()
            t0 = time.time()
            resp = ""
            while time.time() - t0 < 4.0:
                r = self.read_line(timeout=1.0)
                if not r:
                    continue
                if "UPLOAD_READY" in r:
                    resp = "UPLOAD_READY"
                    break
                elif "UPLOAD_ERR" in r:
                    resp = r
                    break
            if resp != "UPLOAD_READY":
                self.sig_upload_finished.emit(False, f"Device rejected start for {fname}: {resp}")
                return

            # Upload chunks with CRC16
            with open(fp, "rb") as f:
                offset = 0
                t_start = time.time()
                while offset < fsize:
                    if self.abort_requested:
                        self.ser.write(b"UPLOAD_ABORT\n")
                        self.ser.flush()
                        self.sig_upload_finished.emit(False, "Upload cancelled by user.")
                        return

                    chunk_data = f.read(CHUNK_SIZE)
                    chunk_len = len(chunk_data)
                    chunk_crc = calc_crc16(chunk_data)

                    header = f"CHUNK {offset} {chunk_len} {chunk_crc:04X}\n".encode('latin1')
                    self.ser.write(header)
                    self.ser.flush()
                    self.ser.write(chunk_data)
                    self.ser.flush()

                    ack_line = ""
                    t_ack = time.time()
                    while time.time() - t_ack < 4.0:
                        r = self.read_line(timeout=1.0)
                        if not r:
                            continue
                        if r.startswith("ACK ") or r.startswith("NACK ") or "WRITE_ERR" in r:
                            ack_line = r
                            break
                    if ack_line != f"ACK {offset}":
                        self.sig_log.emit(f"Chunk at {offset} failed ({ack_line}). Retrying...")
                        self.ser.write(header)
                        self.ser.flush()
                        self.ser.write(chunk_data)
                        self.ser.flush()
                        ack_line = ""
                        t_ack = time.time()
                        while time.time() - t_ack < 4.0:
                            r = self.read_line(timeout=1.0)
                            if not r:
                                continue
                            if r.startswith("ACK ") or r.startswith("NACK ") or "WRITE_ERR" in r:
                                ack_line = r
                                break
                        if ack_line != f"ACK {offset}":
                            self.sig_upload_finished.emit(False, f"Device failed to acknowledge chunk at {offset} ({ack_line})")
                            return

                    offset += chunk_len
                    uploaded_bytes_all += chunk_len

                    dt = max(0.001, time.time() - t_start)
                    speed_kbps = (offset / 1024.0) / dt

                    file_pct = int((offset * 100) / max(1, fsize))
                    batch_pct = int((uploaded_bytes_all * 100) / max(1, total_bytes_all))

                    self.sig_file_progress.emit(file_pct, fname, speed_kbps)
                    self.sig_batch_progress.emit(idx + 1, total_files, batch_pct)

            # Send UPLOAD_FINISH
            self.ser.write(b"UPLOAD_FINISH\n")
            self.ser.flush()
            t0 = time.time()
            finish_ok = False
            while time.time() - t0 < 4.0:
                r = self.read_line(timeout=1.0)
                if "UPLOAD_OK" in r:
                    finish_ok = True
                    break
            if not finish_ok:
                self.sig_upload_finished.emit(False, f"Device failed to complete upload for {fname}")
                return

        self.sig_upload_finished.emit(True, f"All {total_files} file(s) uploaded successfully!")
        self.handle_storage()
        self.handle_list()

    def handle_disconnect(self):
        try:
            self.ser.write(b"DISCONNECT\n")
            self.ser.flush()
            time.sleep(0.1)
        except Exception:
            pass
        self.close_port()
        self.sig_disconnected_ack.emit()

# =============================================================================
# Drag-and-Drop Area Widget
# =============================================================================
class DropZone(QFrame):
    files_dropped = pyqtSignal(list)

    def __init__(self):
        super().__init__()
        self.setObjectName("dropZone")
        self.setAcceptDrops(True)
        self.setFixedHeight(95)

        layout = QVBoxLayout(self)
        layout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.setSpacing(4)

        icon_lbl = QLabel("🌸")
        icon_lbl.setStyleSheet("font-size: 22px;")
        icon_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(icon_lbl)

        txt_lbl = QLabel("Drag & Drop MP3 or WAV Files Here\n— or click [+ Add Files...] below —")
        txt_lbl.setStyleSheet("color: #FFAEC4; font-weight: bold; font-size: 12px; line-height: 1.3;")
        txt_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(txt_lbl)

    def dragEnterEvent(self, event):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event):
        files = []
        for url in event.mimeData().urls():
            p = url.toLocalFile()
            if os.path.isfile(p):
                ext = os.path.splitext(p)[1].lower()
                if ext in [".mp3", ".wav"]:
                    files.append(p)
        if files:
            self.files_dropped.emit(files)

# =============================================================================
# Main Window UI
# =============================================================================
class SakuraManagerWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("🌸 Sakura Music Manager - MAX32655")
        self.resize(1080, 750)
        self.setMinimumSize(980, 680)

        # Workers
        self.worker_serial = SerialWorker()
        self.worker_direct = DirectSdWorker()

        # Connect serial worker signals
        self.worker_serial.sig_log.connect(self.log_msg)
        self.worker_serial.sig_connected.connect(self.on_serial_connected_state)
        self.worker_serial.sig_storage.connect(self.on_storage_update)
        self.worker_serial.sig_file_list.connect(self.on_file_list_update)
        self.worker_serial.sig_delete_done.connect(self.on_delete_done)
        self.worker_serial.sig_file_progress.connect(self.on_file_progress_serial)
        self.worker_serial.sig_batch_progress.connect(self.on_batch_progress)
        self.worker_serial.sig_upload_finished.connect(self.on_upload_finished)
        self.worker_serial.sig_disconnected_ack.connect(self.on_serial_disconnected_ack)

        # Connect direct SD worker signals
        self.worker_direct.sig_log.connect(self.log_msg)
        self.worker_direct.sig_storage.connect(self.on_storage_update)
        self.worker_direct.sig_file_list.connect(self.on_file_list_update)
        self.worker_direct.sig_delete_done.connect(self.on_delete_done)
        self.worker_direct.sig_file_progress.connect(self.on_file_progress_direct)
        self.worker_direct.sig_batch_progress.connect(self.on_batch_progress)
        self.worker_direct.sig_upload_finished.connect(self.on_upload_finished)

        self.current_mode = "DIRECT_SD" # "DIRECT_SD" or "USB_SERIAL"
        self.upload_queue = [] # list of local file paths
        self.is_serial_connected = False
        self.selected_sd_path = ""

        self.init_ui()
        self.refresh_drives()
        self.refresh_ports()
        self.apply_mode_ui()

    def init_ui(self):
        # Premium Dark Obsidian Sakura Stylesheet
        self.setStyleSheet("""
            QMainWindow {
                background-color: #0C0914;
            }
            QWidget {
                color: #FFFFFF;
                font-family: 'Segoe UI', system-ui, sans-serif;
            }
            QFrame#card {
                background-color: #150F22;
                border: 1px solid #352445;
                border-radius: 12px;
            }
            QFrame#dropZone {
                background-color: #180F26;
                border: 2px dashed #5D3770;
                border-radius: 12px;
            }
            QLabel {
                font-size: 13px;
            }
            QLabel#headerTitle {
                color: #FFFFFF;
                font-size: 20px;
                font-weight: 800;
                letter-spacing: 0.5px;
            }
            QLabel#headerSub {
                color: #FFAEC4;
                font-size: 12px;
                font-weight: 500;
            }
            QLabel#statusBadgeDisconnected {
                background-color: #38121E;
                color: #FF4D6D;
                border: 1px solid #FF4D6D;
                border-radius: 12px;
                padding: 4px 12px;
                font-weight: bold;
                font-size: 12px;
            }
            QLabel#statusBadgeConnected {
                background-color: #0F3320;
                color: #38EF7D;
                border: 1px solid #38EF7D;
                border-radius: 12px;
                padding: 4px 12px;
                font-weight: bold;
                font-size: 12px;
            }
            QLabel#statusBadgeSdActive {
                background-color: #0F2E33;
                color: #38EFE0;
                border: 1px solid #38EFE0;
                border-radius: 12px;
                padding: 4px 12px;
                font-weight: bold;
                font-size: 12px;
            }
            QPushButton {
                background-color: #231633;
                color: #FFFFFF;
                border: 1px solid #4D2D68;
                border-radius: 8px;
                padding: 7px 16px;
                font-weight: 600;
                font-size: 12px;
            }
            QPushButton:hover {
                background-color: #321F4A;
                border: 1px solid #FF6B9D;
            }
            QPushButton:pressed {
                background-color: #1A1026;
            }
            QPushButton#btnPrimary {
                background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #FF6B9D, stop:1 #FF4D80);
                color: #FFFFFF;
                border: none;
                font-weight: bold;
            }
            QPushButton#btnPrimary:hover {
                background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #FF85B0, stop:1 #FF6694);
            }
            QPushButton#btnFastTransfer {
                background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00B4D8, stop:1 #FF6B9D);
                color: #FFFFFF;
                border: none;
                font-weight: bold;
                font-size: 13px;
                padding: 8px 18px;
            }
            QPushButton#btnFastTransfer:hover {
                background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00C6EB, stop:1 #FF85B0);
            }
            QPushButton#btnDanger {
                background-color: #3D1220;
                color: #FF6B8B;
                border: 1px solid #6E1B32;
            }
            QPushButton#btnDanger:hover {
                background-color: #5E1A2F;
                border: 1px solid #FF4D6D;
                color: #FFFFFF;
            }
            QPushButton#modeBtnActive {
                background-color: #3C1E4F;
                color: #FF85B0;
                border: 1px solid #FF6B9D;
                border-radius: 8px;
                font-weight: bold;
            }
            QPushButton#modeBtnInactive {
                background-color: #1A1226;
                color: #A396B2;
                border: 1px solid #352445;
                border-radius: 8px;
            }
            QComboBox {
                background-color: #1E142B;
                color: #FFFFFF;
                border: 1px solid #4D2D68;
                border-radius: 8px;
                padding: 6px 12px;
                min-width: 140px;
            }
            QComboBox QAbstractItemView {
                background-color: #1E142B;
                color: #FFFFFF;
                selection-background-color: #FF6B9D;
                selection-color: #FFFFFF;
                border: 1px solid #4D2D68;
            }
            QTableWidget {
                background-color: #120D1C;
                border: 1px solid #2F1E3D;
                border-radius: 8px;
                gridline-color: #241730;
                color: #E8E4F0;
                selection-background-color: #361F4D;
                selection-color: #FFAEC4;
                font-size: 12px;
            }
            QHeaderView::section {
                background-color: #1A1128;
                color: #FFAEC4;
                padding: 6px;
                border: none;
                border-bottom: 2px solid #FF6B9D;
                font-weight: bold;
                font-size: 12px;
            }
            QProgressBar {
                background-color: #180F24;
                border: 1px solid #38224D;
                border-radius: 6px;
                text-align: center;
                color: #FFFFFF;
                font-weight: bold;
                font-size: 11px;
            }
            QProgressBar::chunk {
                background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #FF6B9D, stop:1 #FFAEC4);
                border-radius: 5px;
            }
        """)

        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)
        main_layout.setContentsMargins(18, 14, 18, 14)
        main_layout.setSpacing(10)

        # ---------------------------------------------------------------------
        # TOP BAR: Mode Selector, Title, Drive / COM Controls
        # ---------------------------------------------------------------------
        top_card = QFrame()
        top_card.setObjectName("card")
        top_layout = QVBoxLayout(top_card)
        top_layout.setContentsMargins(16, 12, 16, 12)
        top_layout.setSpacing(10)

        row1 = QHBoxLayout()
        title_box = QVBoxLayout()
        title_box.setSpacing(2)
        title_lbl = QLabel("🌸 SAKURA MUSIC MANAGER")
        title_lbl.setObjectName("headerTitle")
        self.sub_lbl = QLabel("Ultra-Fast MicroSD Management Suite & USB Sync (MAX32655)")
        self.sub_lbl.setObjectName("headerSub")
        title_box.addWidget(title_lbl)
        title_box.addWidget(self.sub_lbl)
        row1.addLayout(title_box)

        row1.addStretch()

        # Mode Selector Buttons
        mode_box = QHBoxLayout()
        mode_box.setSpacing(6)
        self.btn_mode_direct = QPushButton("⚡ Ultra-Fast SD Mode (1-2s)")
        self.btn_mode_direct.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_mode_direct.setToolTip("Direct high-speed transfer (30-80 MB/s). Connect your MicroSD card to PC via SD slot or USB adapter!")
        self.btn_mode_direct.clicked.connect(lambda: self.switch_mode("DIRECT_SD"))

        self.btn_mode_serial = QPushButton("🔌 USB Cable Mode")
        self.btn_mode_serial.setCursor(Qt.CursorShape.PointingHandCursor)
        self.btn_mode_serial.setToolTip("Direct on-device transfer via USB cable without removing SD card (115,200 baud).")
        self.btn_mode_serial.clicked.connect(lambda: self.switch_mode("USB_SERIAL"))

        mode_box.addWidget(self.btn_mode_direct)
        mode_box.addWidget(self.btn_mode_serial)
        row1.addLayout(mode_box)
        top_layout.addLayout(row1)

        # Row 2: Connection / Drive Controls
        self.row2 = QHBoxLayout()
        self.badge = QLabel("● DETECTING...")
        self.badge.setObjectName("statusBadgeSdActive")
        self.row2.addWidget(self.badge)

        # Direct SD Mode Widgets
        self.cb_drives = QComboBox()
        self.cb_drives.currentIndexChanged.connect(self.on_drive_selected)
        self.btn_refresh_drives = QPushButton("⟳ Scan Drives")
        self.btn_refresh_drives.clicked.connect(self.refresh_drives)
        self.btn_browse_folder = QPushButton("📁 Browse SD Folder...")
        self.btn_browse_folder.clicked.connect(self.browse_sd_folder)

        # USB Serial Mode Widgets
        self.cb_ports = QComboBox()
        self.btn_refresh_ports = QPushButton("⟳ Scan Ports")
        self.btn_refresh_ports.clicked.connect(self.refresh_ports)
        self.btn_connect = QPushButton("Connect Device")
        self.btn_connect.setObjectName("btnPrimary")
        self.btn_connect.clicked.connect(self.toggle_serial_connection)

        self.row2.addWidget(self.cb_drives)
        self.row2.addWidget(self.btn_refresh_drives)
        self.row2.addWidget(self.btn_browse_folder)

        self.row2.addWidget(self.cb_ports)
        self.row2.addWidget(self.btn_refresh_ports)
        self.row2.addWidget(self.btn_connect)

        self.row2.addStretch()
        top_layout.addLayout(self.row2)

        main_layout.addWidget(top_card)

        # ---------------------------------------------------------------------
        # STORAGE GAUGE CARD
        # ---------------------------------------------------------------------
        storage_card = QFrame()
        storage_card.setObjectName("card")
        storage_layout = QVBoxLayout(storage_card)
        storage_layout.setContentsMargins(16, 10, 16, 12)
        storage_layout.setSpacing(6)

        st_header_layout = QHBoxLayout()
        self.st_title = QLabel("MicroSD Storage Capacity")
        self.st_title.setStyleSheet("font-weight: bold; color: #FFAEC4; font-size: 13px;")
        self.lbl_storage_details = QLabel("Detecting MicroSD card...")
        self.lbl_storage_details.setStyleSheet("color: #D8D4E2; font-size: 12px;")
        st_header_layout.addWidget(self.st_title)
        st_header_layout.addStretch()
        st_header_layout.addWidget(self.lbl_storage_details)
        storage_layout.addLayout(st_header_layout)

        self.storage_bar = QProgressBar()
        self.storage_bar.setValue(0)
        self.storage_bar.setFixedHeight(16)
        storage_layout.addWidget(self.storage_bar)

        main_layout.addWidget(storage_card)

        # ---------------------------------------------------------------------
        # MAIN BODY: Split Panes (File Manager | Uploader & Queue)
        # ---------------------------------------------------------------------
        body_layout = QHBoxLayout()
        body_layout.setSpacing(12)

        # --- LEFT PANE: MicroSD File Manager ---
        mgr_card = QFrame()
        mgr_card.setObjectName("card")
        mgr_layout = QVBoxLayout(mgr_card)
        mgr_layout.setContentsMargins(14, 12, 14, 12)
        mgr_layout.setSpacing(8)

        mgr_header = QHBoxLayout()
        self.lbl_mgr_title = QLabel("Songs on MicroSD Card (0)")
        self.lbl_mgr_title.setStyleSheet("font-weight: bold; color: #FFAEC4; font-size: 14px;")
        self.btn_refresh_files = QPushButton("⟳ Refresh Songs")
        self.btn_refresh_files.clicked.connect(self.refresh_active_songs)
        mgr_header.addWidget(self.lbl_mgr_title)
        mgr_header.addStretch()
        mgr_header.addWidget(self.btn_refresh_files)
        mgr_layout.addLayout(mgr_header)

        self.tbl_files = QTableWidget(0, 4)
        self.tbl_files.setHorizontalHeaderLabels(["#", "Track Name", "File Size", "Action"])
        self.tbl_files.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self.tbl_files.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        self.tbl_files.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        self.tbl_files.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.ResizeToContents)
        self.tbl_files.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.tbl_files.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        mgr_layout.addWidget(self.tbl_files)

        body_layout.addWidget(mgr_card, 55)

        # --- RIGHT PANE: Uploader & Queue ---
        up_card = QFrame()
        up_card.setObjectName("card")
        up_layout = QVBoxLayout(up_card)
        up_layout.setContentsMargins(14, 12, 14, 12)
        up_layout.setSpacing(8)

        up_header = QHBoxLayout()
        self.up_title = QLabel("Upload Music to Player")
        self.up_title.setStyleSheet("font-weight: bold; color: #FFAEC4; font-size: 14px;")
        up_header.addWidget(self.up_title)
        up_header.addStretch()
        up_layout.addLayout(up_header)

        # Drag and drop zone
        self.drop_zone = DropZone()
        self.drop_zone.files_dropped.connect(self.add_files_to_queue)
        up_layout.addWidget(self.drop_zone)

        # Queue table
        self.tbl_queue = QTableWidget(0, 4)
        self.tbl_queue.setHorizontalHeaderLabels(["#", "File to Upload", "Size", "Status"])
        self.tbl_queue.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self.tbl_queue.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        self.tbl_queue.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        self.tbl_queue.horizontalHeader().setSectionResizeMode(3, QHeaderView.ResizeMode.ResizeToContents)
        self.tbl_queue.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        up_layout.addWidget(self.tbl_queue)

        # Queue buttons
        q_btn_layout = QHBoxLayout()
        self.btn_browse = QPushButton("＋ Add Files...")
        self.btn_browse.clicked.connect(self.browse_files)
        self.btn_clear_queue = QPushButton("Clear Queue")
        self.btn_clear_queue.clicked.connect(self.clear_queue)
        self.btn_upload = QPushButton("⚡ Transfer to MicroSD")
        self.btn_upload.setObjectName("btnFastTransfer")
        self.btn_upload.clicked.connect(self.start_upload)
        self.btn_cancel = QPushButton("⏹ Cancel")
        self.btn_cancel.setObjectName("btnDanger")
        self.btn_cancel.setEnabled(False)
        self.btn_cancel.clicked.connect(self.cancel_upload)

        q_btn_layout.addWidget(self.btn_browse)
        q_btn_layout.addWidget(self.btn_clear_queue)
        q_btn_layout.addWidget(self.btn_upload)
        q_btn_layout.addWidget(self.btn_cancel)
        up_layout.addLayout(q_btn_layout)

        body_layout.addWidget(up_card, 45)

        main_layout.addLayout(body_layout)

        # ---------------------------------------------------------------------
        # BOTTOM BAR: Progress Gauges & Speed Stats
        # ---------------------------------------------------------------------
        btm_card = QFrame()
        btm_card.setObjectName("card")
        btm_layout = QVBoxLayout(btm_card)
        btm_layout.setContentsMargins(16, 10, 16, 10)
        btm_layout.setSpacing(6)

        stats_layout = QHBoxLayout()
        self.lbl_progress_status = QLabel("Ready. Select songs to upload.")
        self.lbl_progress_status.setStyleSheet("color: #FFAEC4; font-weight: bold; font-size: 12px;")
        self.lbl_speed_stats = QLabel("Speed: 0.0 MB/s")
        self.lbl_speed_stats.setStyleSheet("color: #D8D4E2; font-size: 12px;")
        stats_layout.addWidget(self.lbl_progress_status)
        stats_layout.addStretch()
        stats_layout.addWidget(self.lbl_speed_stats)
        btm_layout.addLayout(stats_layout)

        # Progress bars
        self.bar_file = QProgressBar()
        self.bar_file.setValue(0)
        self.bar_file.setFixedHeight(14)
        btm_layout.addWidget(self.bar_file)

        self.bar_batch = QProgressBar()
        self.bar_batch.setValue(0)
        self.bar_batch.setFixedHeight(14)
        btm_layout.addWidget(self.bar_batch)

        main_layout.addWidget(btm_card)

    # -------------------------------------------------------------------------
    # Mode Switching (Direct MicroSD vs USB Serial Cable)
    # -------------------------------------------------------------------------
    def switch_mode(self, mode: str):
        self.current_mode = mode
        self.apply_mode_ui()

    def apply_mode_ui(self):
        if self.current_mode == "DIRECT_SD":
            self.btn_mode_direct.setObjectName("modeBtnActive")
            self.btn_mode_serial.setObjectName("modeBtnInactive")
            self.btn_upload.setObjectName("btnFastTransfer")
            self.btn_upload.setText("⚡ Transfer to MicroSD (1-2s)")
            self.sub_lbl.setText("⚡ Ultra-Fast Direct MicroSD Mode (30-80 MB/s instantaneous transfers)")

            # Show direct SD widgets, hide serial widgets
            self.cb_drives.setVisible(True)
            self.btn_refresh_drives.setVisible(True)
            self.btn_browse_folder.setVisible(True)
            self.cb_ports.setVisible(False)
            self.btn_refresh_ports.setVisible(False)
            self.btn_connect.setVisible(False)

            self.on_drive_selected()
        else:
            self.btn_mode_direct.setObjectName("modeBtnInactive")
            self.btn_mode_serial.setObjectName("modeBtnActive")
            self.btn_upload.setObjectName("btnPrimary")
            self.btn_upload.setText("▶ Upload to Player (USB Cable)")
            self.sub_lbl.setText("🔌 USB Cable Mode (MAX32655 on-device transfer via COM port)")

            # Hide direct SD widgets, show serial widgets
            self.cb_drives.setVisible(False)
            self.btn_refresh_drives.setVisible(False)
            self.btn_browse_folder.setVisible(False)
            self.cb_ports.setVisible(True)
            self.btn_refresh_ports.setVisible(True)
            self.btn_connect.setVisible(True)

            self.update_serial_ui_state()

        self.btn_mode_direct.style().unpolish(self.btn_mode_direct)
        self.btn_mode_direct.style().polish(self.btn_mode_direct)
        self.btn_mode_serial.style().unpolish(self.btn_mode_serial)
        self.btn_mode_serial.style().polish(self.btn_mode_serial)
        self.btn_upload.style().unpolish(self.btn_upload)
        self.btn_upload.style().polish(self.btn_upload)

    # -------------------------------------------------------------------------
    # Direct MicroSD Drive Management
    # -------------------------------------------------------------------------
    def refresh_drives(self):
        self.cb_drives.clear()
        drives = detect_windows_drives()
        preferred_idx = 0
        removable_found = False

        for idx, (path, label, fs, type_desc, tot_gb, fre_gb, is_remov) in enumerate(drives):
            disp = f"{path} [{label or type_desc}] ({fre_gb:.1f} GB free of {tot_gb:.1f} GB)"
            self.cb_drives.addItem(disp, path)
            if is_remov and not removable_found:
                preferred_idx = idx
                removable_found = True

        if drives:
            self.cb_drives.setCurrentIndex(preferred_idx)
            self.on_drive_selected()

    def browse_sd_folder(self):
        chosen = QFileDialog.getExistingDirectory(self, "Select MicroSD Card Root / Music Directory")
        if chosen:
            # Check if already in combobox
            idx = self.cb_drives.findData(chosen)
            if idx >= 0:
                self.cb_drives.setCurrentIndex(idx)
            else:
                self.cb_drives.addItem(f"Custom: {chosen}", chosen)
                self.cb_drives.setCurrentIndex(self.cb_drives.count() - 1)
            self.on_drive_selected()

    def on_drive_selected(self):
        if self.current_mode != "DIRECT_SD":
            return
        sd_path = self.cb_drives.currentData()
        if not sd_path or not os.path.exists(sd_path):
            self.badge.setText("● NO DRIVE")
            self.badge.setObjectName("statusBadgeDisconnected")
            self.badge.style().unpolish(self.badge)
            self.badge.style().polish(self.badge)
            return

        self.selected_sd_path = sd_path
        self.worker_direct.set_target_dir(sd_path)
        self.badge.setText(f"● SD ACTIVE ({sd_path})")
        self.badge.setObjectName("statusBadgeSdActive")
        self.badge.style().unpolish(self.badge)
        self.badge.style().polish(self.badge)

        self.lbl_progress_status.setText(f"⚡ Ready in Ultra-Fast MicroSD Mode ({sd_path})")
        self.worker_direct.queue_action("STORAGE")
        self.worker_direct.queue_action("LIST")

    # -------------------------------------------------------------------------
    # USB Serial Cable Management
    # -------------------------------------------------------------------------
    def refresh_ports(self):
        self.cb_ports.clear()
        ports = serial.tools.list_ports.comports()
        preferred_idx = 0
        for idx, p in enumerate(ports):
            self.cb_ports.addItem(f"{p.device} ({p.description})", p.device)
            if "COM21" in p.device or "PICO" in p.description or "MAX" in p.description:
                preferred_idx = idx
        if ports:
            self.cb_ports.setCurrentIndex(preferred_idx)

    def toggle_serial_connection(self):
        if not self.is_serial_connected:
            port = self.cb_ports.currentData()
            if not port:
                QMessageBox.warning(self, "No Port", "Please select a valid COM port!")
                return
            self.lbl_progress_status.setText(f"Connecting to {port}...")
            if self.worker_serial.open_port(port, 115200):
                self.worker_serial.queue_action("PING")
        else:
            self.lbl_progress_status.setText("Disconnecting from device...")
            self.worker_serial.queue_action("DISCONNECT")

    def on_serial_connected_state(self, connected: bool, msg: str):
        self.is_serial_connected = connected
        self.update_serial_ui_state(msg)

    def update_serial_ui_state(self, msg: str = ""):
        if self.current_mode != "USB_SERIAL":
            return
        if self.is_serial_connected:
            self.badge.setText("● CONNECTED")
            self.badge.setObjectName("statusBadgeConnected")
            self.btn_connect.setText("Disconnect Device")
            self.btn_connect.setObjectName("btnDanger")
            self.lbl_progress_status.setText(f"Connected to Sakura Music Player on {self.worker_serial.port}!")
        else:
            self.badge.setText("● DISCONNECTED")
            self.badge.setObjectName("statusBadgeDisconnected")
            self.btn_connect.setText("Connect Device")
            self.btn_connect.setObjectName("btnPrimary")
            if msg:
                self.lbl_progress_status.setText(f"Disconnected: {msg}")
            else:
                self.lbl_progress_status.setText("Connect player via USB cable to manage songs.")
            self.worker_serial.close_port()
        self.badge.style().unpolish(self.badge)
        self.badge.style().polish(self.badge)
        self.btn_connect.style().unpolish(self.btn_connect)
        self.btn_connect.style().polish(self.btn_connect)

    def on_serial_disconnected_ack(self):
        self.on_serial_connected_state(False, "Disconnected by user")

    # -------------------------------------------------------------------------
    # Unified Storage & File Listing
    # -------------------------------------------------------------------------
    def refresh_active_songs(self):
        if self.current_mode == "DIRECT_SD":
            self.worker_direct.queue_action("STORAGE")
            self.worker_direct.queue_action("LIST")
        else:
            if not self.is_serial_connected:
                QMessageBox.information(self, "Offline", "Connect to player first to list songs.")
                return
            self.worker_serial.queue_action("STORAGE")
            self.worker_serial.queue_action("LIST")

    def on_storage_update(self, total_mb: int, free_mb: int):
        used_mb = max(0, total_mb - free_mb)
        pct_used = int((used_mb * 100) / max(1, total_mb))
        self.storage_bar.setValue(pct_used)
        
        tot_str = f"{total_mb / 1024.0:.2f} GB" if total_mb >= 1024 else f"{total_mb} MB"
        fre_str = f"{free_mb / 1024.0:.2f} GB" if free_mb >= 1024 else f"{free_mb} MB"
        usd_str = f"{used_mb / 1024.0:.2f} GB" if used_mb >= 1024 else f"{used_mb} MB"
        pct_free = 100.0 - (used_mb * 100.0 / max(1, total_mb))
        
        self.lbl_storage_details.setText(f"Used: {usd_str} | Free: {fre_str} of {tot_str} ({pct_free:.1f}% Free)")

    def on_file_list_update(self, files: list):
        self.lbl_mgr_title.setText(f"Songs on MicroSD Card ({len(files)})")
        self.tbl_files.setRowCount(len(files))
        for row, (name, size) in enumerate(files):
            # Column 0: Index
            item_idx = QTableWidgetItem(str(row + 1))
            item_idx.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            self.tbl_files.setItem(row, 0, item_idx)

            # Column 1: Filename
            item_name = QTableWidgetItem(name)
            self.tbl_files.setItem(row, 1, item_name)

            # Column 2: Size
            sz_str = f"{size / (1024*1024):.2f} MB" if size >= 1024*1024 else f"{size / 1024:.1f} KB"
            item_sz = QTableWidgetItem(sz_str)
            item_sz.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            self.tbl_files.setItem(row, 2, item_sz)

            # Column 3: Delete Action Button
            btn_del = QPushButton("✕ Delete")
            btn_del.setObjectName("btnDanger")
            btn_del.setProperty("filename", name)
            btn_del.clicked.connect(self.on_delete_clicked)
            self.tbl_files.setCellWidget(row, 3, btn_del)

    def on_delete_clicked(self):
        sender = self.sender()
        if not sender:
            return
        fname = sender.property("filename")
        reply = QMessageBox.question(
            self, "Confirm Delete",
            f"Are you sure you want to delete '{fname}' from the MicroSD card?\nThis action cannot be undone.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
        )
        if reply == QMessageBox.StandardButton.Yes:
            self.lbl_progress_status.setText(f"Deleting {fname} from SD card...")
            if self.current_mode == "DIRECT_SD":
                self.worker_direct.queue_action("DELETE", fname)
            else:
                self.worker_serial.queue_action("DELETE", fname)

    def on_delete_done(self, success: bool, msg: str):
        if success:
            self.lbl_progress_status.setText(f"✓ Deleted '{msg}' successfully!")
        else:
            QMessageBox.critical(self, "Delete Error", f"Could not delete: {msg}")

    # -------------------------------------------------------------------------
    # Queue & Upload Management
    # -------------------------------------------------------------------------
    def browse_files(self):
        files, _ = QFileDialog.getOpenFileNames(
            self, "Select Songs to Upload", "", "Audio Files (*.mp3 *.wav)"
        )
        if files:
            self.add_files_to_queue(files)

    def add_files_to_queue(self, paths: list):
        for p in paths:
            if p not in self.upload_queue:
                self.upload_queue.append(p)
        self.refresh_queue_table()

    def clear_queue(self):
        self.upload_queue.clear()
        self.refresh_queue_table()

    def refresh_queue_table(self):
        self.tbl_queue.setRowCount(len(self.upload_queue))
        for row, path in enumerate(self.upload_queue):
            name = os.path.basename(path)
            size = os.path.getsize(path)
            sz_str = f"{size / (1024*1024):.2f} MB" if size >= 1024*1024 else f"{size / 1024:.1f} KB"

            item_idx = QTableWidgetItem(str(row + 1))
            item_idx.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            self.tbl_queue.setItem(row, 0, item_idx)

            self.tbl_queue.setItem(row, 1, QTableWidgetItem(name))

            item_sz = QTableWidgetItem(sz_str)
            item_sz.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            self.tbl_queue.setItem(row, 2, item_sz)

            item_st = QTableWidgetItem("Queued")
            item_st.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            self.tbl_queue.setItem(row, 3, item_st)

    def start_upload(self):
        if not self.upload_queue:
            QMessageBox.information(self, "Queue Empty", "Please add at least one MP3 or WAV file to the upload queue.")
            return

        if self.current_mode == "DIRECT_SD":
            if not self.selected_sd_path or not os.path.exists(self.selected_sd_path):
                QMessageBox.warning(self, "No MicroSD Drive", "Please select a valid MicroSD card drive or folder first!")
                return
            self.set_ui_uploading_state(True)
            self.lbl_progress_status.setText("⚡ Initiating ultra-fast transfer to MicroSD card...")
            self.worker_direct.queue_action("UPLOAD_BATCH", list(self.upload_queue))
        else:
            if not self.is_serial_connected:
                QMessageBox.warning(self, "Not Connected", "Please connect to the Sakura Music Player via USB cable before uploading.")
                return
            self.set_ui_uploading_state(True)
            self.lbl_progress_status.setText("Initiating batch upload over USB cable...")
            self.worker_serial.queue_action("UPLOAD_BATCH", list(self.upload_queue))

    def cancel_upload(self):
        if self.current_mode == "DIRECT_SD":
            self.worker_direct.request_abort()
        else:
            self.worker_serial.request_abort()
        self.lbl_progress_status.setText("Cancelling transfer...")
        self.btn_cancel.setEnabled(False)

    def set_ui_uploading_state(self, is_uploading: bool):
        self.btn_upload.setEnabled(not is_uploading)
        self.btn_cancel.setEnabled(is_uploading)
        self.btn_browse.setEnabled(not is_uploading)
        self.btn_clear_queue.setEnabled(not is_uploading)
        self.btn_mode_direct.setEnabled(not is_uploading)
        self.btn_mode_serial.setEnabled(not is_uploading)
        if self.current_mode == "USB_SERIAL":
            self.btn_connect.setEnabled(not is_uploading)
        if is_uploading:
            self.bar_file.setValue(0)
            self.bar_batch.setValue(0)

    def on_file_progress_direct(self, pct: int, filename: str, speed_mbps: float):
        self.bar_file.setValue(pct)
        self.lbl_progress_status.setText(f"⚡ Transferring '{filename}' ({pct}%)")
        self.lbl_speed_stats.setText(f"Speed: {speed_mbps:.1f} MB/s")

    def on_file_progress_serial(self, pct: int, filename: str, speed_kbps: float):
        self.bar_file.setValue(pct)
        self.lbl_progress_status.setText(f"Uploading '{filename}' ({pct}%)")
        self.lbl_speed_stats.setText(f"Speed: {speed_kbps:.1f} KB/s")

    def on_batch_progress(self, current_file: int, total_files: int, total_pct: int):
        self.bar_batch.setValue(total_pct)
        self.setWindowTitle(f"🌸 Transferring [{current_file}/{total_files}] ({total_pct}%) - Sakura Music Manager")

    def on_upload_finished(self, success: bool, msg: str):
        self.set_ui_uploading_state(False)
        self.setWindowTitle("🌸 Sakura Music Manager - MAX32655")

        if success:
            self.bar_file.setValue(100)
            self.bar_batch.setValue(100)
            self.lbl_progress_status.setText(f"✓ {msg}")
            QMessageBox.information(self, "Transfer Success", f"Done!\n\n{msg}")
            self.upload_queue.clear()
            self.refresh_queue_table()
        else:
            self.lbl_progress_status.setText(f"✕ {msg}")
            QMessageBox.critical(self, "Transfer Alert", f"Status:\n\n{msg}")

    def log_msg(self, msg: str):
        print(f"[MANAGER] {msg}")

# =============================================================================
# Application Entry Point
# =============================================================================
def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    win = SakuraManagerWindow()
    win.show()
    sys.exit(app.exec())

if __name__ == "__main__":
    main()
