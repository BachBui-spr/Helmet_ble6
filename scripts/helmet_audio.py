#!/usr/bin/env python3
"""Nghe lai am thanh LE Audio ma Helmet nhan duoc, lay ve qua J-Link (SEGGER RTT kenh 1).

Firmware Helmet chep nguyen cac goi LC3 nhan tu dien thoai vao kenh RTT 1
(apps/main/src/debug/capture.c). Script nay:
  - ket noi J-Link (chi can day SWD), dat dia chi RTT lay tu file ELF,
  - gui lenh "helmet capture on" qua shell RTT,
  - giai ma LC3 bang liblc3 cua he thong (cung thu vien voi firmware),
  - phat ra loa may tinh (paplay) va luu file WAV,
  - dong thoi hien log kenh 0 va cho go lenh shell (thay cho "make rtt").

Chi dung thu vien chuan cua Python + libjlinkarm.so (SEGGER) + liblc3.so.1 (he thong).

Dung:
  scripts/helmet_audio.py                    # build/main, phat loa + luu WAV vao build/audio/
  scripts/helmet_audio.py --no-play          # chi luu WAV
  scripts/helmet_audio.py --sink <ten>       # chon loa phat (xem: pactl list short sinks)
  scripts/helmet_audio.py --raw cap.bin      # luu them du lieu tho de giai ma lai sau
  scripts/helmet_audio.py --replay cap.bin   # giai ma lai file tho -> WAV (khong can mach)
  scripts/helmet_audio.py --selftest         # tu kiem tra phan giai ma tren PC (khong can mach)
Thoat: Ctrl+C
"""

import argparse
import ctypes
import ctypes.util
import datetime
import math
import os
import select
import struct
import subprocess
import sys
import time
import wave
from array import array
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---- Dinh dang ban ghi tren RTT kenh 1 (phai khop voi capture.c) ----
REC_CFG, REC_SDU, REC_STOP = 1, 2, 3
SYNC = b"\xa5\x5a"
HDR = struct.Struct("<2sBBHH")   # sync | type | stream | len | rec_seq
CFG = struct.Struct("<IIHBBI")   # freq_hz | frame_dur_us | octets | chan_cnt | 0 | chan_alloc
SDU = struct.Struct("<BBHI")     # iso_flags | 0 | iso_seq | ts
MAX_PAYLOAD = SDU.size + 320

ISO_FLAG_VALID = 0x01  # BT_ISO_FLAGS_VALID: du lieu tot
ISO_FLAG_ERROR = 0x02  # BT_ISO_FLAGS_ERROR: nhan duoc nhung co the sai (CRC)
ISO_FLAG_LOST = 0x04   # BT_ISO_FLAGS_LOST: khong nhan duoc gi trong cac su kien CIS
ISO_FLAG_TS = 0x08     # BT_ISO_FLAGS_TS

CH_L, CH_R = 1, 2
LOC_FL, LOC_FR = 0x1, 0x2

RTT_CH_LOG, RTT_CH_AUDIO = 0, 1
RTT_AUDIO_NAME = "HelmetAudio"


def log(msg):
    print(f"[audio] {msg}", flush=True)


# ---------------- ELF: lay dia chi _SEGGER_RTT ----------------

def find_elf(build_dir: Path) -> Path:
    """Sysbuild: ELF cua app nam o <build>/<ten_app>/zephyr/zephyr.elf (giong scripts/rtt.sh)"""
    candidates = []
    cache = build_dir / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("APP_DIR:PATH="):
                candidates.append(build_dir / Path(line.split("=", 1)[1]).name / "zephyr/zephyr.elf")
    candidates.append(build_dir / "zephyr/zephyr.elf")
    for elf in candidates:
        if elf.exists():
            return elf
    sys.exit(f"[audio] Khong tim thay zephyr.elf trong {build_dir} - hay build truoc "
             "(make build)")


def elf_symbol(path: Path, name: str):
    """Doc bang ky hieu ELF32 (khong can arm-zephyr-eabi-nm)"""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise ValueError(f"{path} khong phai ELF32")
    end = "<" if data[5] == 1 else ">"
    shoff, = struct.unpack_from(end + "I", data, 0x20)
    shentsize, shnum = struct.unpack_from(end + "HH", data, 0x2E)
    sections = [struct.unpack_from(end + "10I", data, shoff + i * shentsize) for i in range(shnum)]
    want = name.encode()
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strtab_off = sections[sec[6]][4]
        for off in range(sec[4], sec[4] + sec[5], 16):
            st_name, st_value = struct.unpack_from(end + "II", data, off)
            start = strtab_off + st_name
            if data[start:start + len(want) + 1] == want + b"\0":
                return st_value
    return None


# ---------------- J-Link (libjlinkarm.so qua ctypes) ----------------

class _RttStart(ctypes.Structure):
    _fields_ = [("ConfigBlockAddress", ctypes.c_uint32), ("Reserved", ctypes.c_uint32 * 3)]


class _RttBufDesc(ctypes.Structure):
    _fields_ = [("BufferIndex", ctypes.c_int32), ("Direction", ctypes.c_uint32),
                ("acName", ctypes.c_char * 32), ("SizeOfBuffer", ctypes.c_uint32),
                ("Flags", ctypes.c_uint32)]


class JLink:
    """Goi truc tiep thu vien J-Link cua SEGGER - khong can cai them goi Python"""
    RTT_START, RTT_STOP, RTT_GETDESC, RTT_GETNUMBUF = 0, 1, 2, 3
    RTT_DIR_UP = 0
    TIF_SWD = 1

    def __init__(self, lib_path):
        try:
            d = ctypes.CDLL(lib_path)
        except OSError as e:
            sys.exit(f"[audio] Khong nap duoc thu vien J-Link {lib_path}: {e}")
        d.JLINKARM_OpenEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        d.JLINKARM_OpenEx.restype = ctypes.c_char_p
        d.JLINKARM_ExecCommand.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        d.JLINKARM_ExecCommand.restype = ctypes.c_int
        d.JLINKARM_EMU_SelectByUSBSN.argtypes = [ctypes.c_uint32]
        d.JLINKARM_EMU_SelectByUSBSN.restype = ctypes.c_int
        d.JLINKARM_TIF_Select.argtypes = [ctypes.c_int]
        d.JLINKARM_SetSpeed.argtypes = [ctypes.c_uint32]
        d.JLINKARM_Connect.restype = ctypes.c_int
        d.JLINK_RTTERMINAL_Control.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
        d.JLINK_RTTERMINAL_Control.restype = ctypes.c_int
        d.JLINK_RTTERMINAL_Read.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32]
        d.JLINK_RTTERMINAL_Read.restype = ctypes.c_int
        d.JLINK_RTTERMINAL_Write.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32]
        d.JLINK_RTTERMINAL_Write.restype = ctypes.c_int
        self.d = d
        self.opened = False
        self.rtt_running = False
        self._buf = ctypes.create_string_buffer(65536)

    def open(self, serial, device, speed_khz, retries=5):
        if serial and self.d.JLINKARM_EMU_SelectByUSBSN(int(serial)) < 0:
            sys.exit(f"[audio] Khong thay J-Link S/N {serial}")
        err = self.d.JLINKARM_OpenEx(None, None)
        if err:
            sys.exit(f"[audio] Khong mo duoc J-Link: {err.decode(errors='replace')} "
                     "- kiem tra cap USB cua probe")
        self.opened = True
        msg = ctypes.create_string_buffer(256)
        self.d.JLINKARM_ExecCommand(f"Device = {device}".encode(), msg, len(msg))
        self.d.JLINKARM_TIF_Select(self.TIF_SWD)
        self.d.JLINKARM_SetSpeed(speed_khz)
        # Probe nay doi khi can vai lan moi nhan chip (giong scripts/rtt.sh)
        for _ in range(retries):
            if self.d.JLINKARM_Connect() >= 0:
                return
            time.sleep(0.3)
        sys.exit("[audio] Khong ket noi duoc chip qua SWD - kiem tra SWDIO/SWCLK/GND/VTref")

    def rtt_start(self, address, timeout=5.0):
        start = _RttStart(address)
        if self.d.JLINK_RTTERMINAL_Control(self.RTT_START, ctypes.byref(start)) < 0:
            sys.exit("[audio] J-Link khong khoi dong duoc RTT")
        self.rtt_running = True
        deadline = time.time() + timeout
        while time.time() < deadline:
            direction = ctypes.c_int(self.RTT_DIR_UP)
            n = self.d.JLINK_RTTERMINAL_Control(self.RTT_GETNUMBUF, ctypes.byref(direction))
            if n >= 0:
                return n
            time.sleep(0.05)
        sys.exit(f"[audio] Khong thay RTT control block tai 0x{address:08x} - firmware tren mach "
                 "co khop voi file ELF khong? (nap lai: make flash)")

    def rtt_up_name(self, index):
        desc = _RttBufDesc(index, self.RTT_DIR_UP)
        if self.d.JLINK_RTTERMINAL_Control(self.RTT_GETDESC, ctypes.byref(desc)) < 0:
            return ""
        return desc.acName.decode(errors="replace")

    def read(self, channel):
        n = self.d.JLINK_RTTERMINAL_Read(channel, self._buf, len(self._buf))
        if n < 0:
            raise IOError(f"doc RTT kenh {channel} loi ({n}) - mat ket noi J-Link?")
        return self._buf.raw[:n]

    def write(self, channel, data: bytes):
        return self.d.JLINK_RTTERMINAL_Write(channel, data, len(data))

    def close(self):
        if self.rtt_running:
            self.d.JLINK_RTTERMINAL_Control(self.RTT_STOP, None)
            self.rtt_running = False
        if self.opened:
            self.d.JLINKARM_Close()
            self.opened = False


# ---------------- LC3 (liblc3.so.1 qua ctypes) ----------------

class Lc3:
    PCM_S16 = 0  # LC3_PCM_FORMAT_S16

    def __init__(self):
        name = ctypes.util.find_library("lc3") or "liblc3.so.1"
        try:
            lib = ctypes.CDLL(name)
        except OSError:
            sys.exit("[audio] Khong co liblc3 - cai bang: sudo apt install liblc3-1")
        c_int, c_uint, c_void_p = ctypes.c_int, ctypes.c_uint, ctypes.c_void_p
        lib.lc3_frame_samples.argtypes = [c_int, c_int]
        lib.lc3_frame_samples.restype = c_int
        lib.lc3_decoder_size.argtypes = [c_int, c_int]
        lib.lc3_decoder_size.restype = c_uint
        lib.lc3_setup_decoder.argtypes = [c_int, c_int, c_int, c_void_p]
        lib.lc3_setup_decoder.restype = c_void_p
        lib.lc3_decode.argtypes = [c_void_p, ctypes.c_char_p, c_int, c_int, c_void_p, c_int]
        lib.lc3_decode.restype = c_int
        lib.lc3_encoder_size.argtypes = [c_int, c_int]
        lib.lc3_encoder_size.restype = c_uint
        lib.lc3_setup_encoder.argtypes = [c_int, c_int, c_int, c_void_p]
        lib.lc3_setup_encoder.restype = c_void_p
        lib.lc3_encode.argtypes = [c_void_p, c_int, c_void_p, c_int, c_int, c_void_p]
        lib.lc3_encode.restype = c_int
        self.lib = lib


class Lc3Decoder:
    def __init__(self, lc3: Lc3, dt_us, sr_hz):
        lib = lc3.lib
        self.lib = lib
        self.samples = lib.lc3_frame_samples(dt_us, sr_hz)
        size = lib.lc3_decoder_size(dt_us, sr_hz)
        if self.samples <= 0 or size == 0:
            raise ValueError(f"LC3 khong ho tro {sr_hz} Hz / {dt_us} us")
        self._mem = ctypes.create_string_buffer(size)
        self._dec = lib.lc3_setup_decoder(dt_us, sr_hz, 0, self._mem)
        self._pcm = (ctypes.c_int16 * self.samples)()

    def decode(self, frame):
        """frame = 1 khung LC3 (bytes), hoac None khi mat goi -> liblc3 noi suy (PLC).
        Tra ve (ma ket qua, PCM): 0 = OK, 1 = PLC, < 0 = loi."""
        ret = self.lib.lc3_decode(self._dec, frame, len(frame) if frame else 0,
                                  Lc3.PCM_S16, self._pcm, 1)
        if ret < 0:
            return ret, array("h", bytes(2 * self.samples))
        return ret, array("h", bytes(self._pcm))


class Lc3Encoder:
    """Chi dung cho --selftest"""

    def __init__(self, lc3: Lc3, dt_us, sr_hz):
        lib = lc3.lib
        self.lib = lib
        self.samples = lib.lc3_frame_samples(dt_us, sr_hz)
        self._mem = ctypes.create_string_buffer(lib.lc3_encoder_size(dt_us, sr_hz))
        self._enc = lib.lc3_setup_encoder(dt_us, sr_hz, 0, self._mem)

    def encode(self, pcm: array, nbytes):
        src = (ctypes.c_int16 * len(pcm)).from_buffer_copy(pcm.tobytes())
        out = ctypes.create_string_buffer(nbytes)
        if self.lib.lc3_encode(self._enc, Lc3.PCM_S16, src, 1, nbytes, out) != 0:
            raise RuntimeError("lc3_encode loi")
        return out.raw


# ---------------- Tach ban ghi tu dong byte RTT ----------------

class RecordParser:
    def __init__(self):
        self.buf = bytearray()
        self.next_seq = None
        self.lost = 0       # ban ghi bi bo tren mach (bo dem RTT day: PC doc khong kip)
        self.skipped = 0    # byte rac bi bo qua khi do lai dau ban ghi
        self.bytes = 0

    def feed(self, data):
        self.bytes += len(data)
        self.buf += data
        out = []
        while True:
            i = self.buf.find(SYNC)
            if i < 0:
                keep = 1 if self.buf[-1:] == SYNC[:1] else 0
                self.skipped += len(self.buf) - keep
                del self.buf[:len(self.buf) - keep]
                break
            if i > 0:
                self.skipped += i
                del self.buf[:i]
            if len(self.buf) < HDR.size:
                break
            _, rtype, idx, length, seq = HDR.unpack_from(self.buf)
            if rtype not in (REC_CFG, REC_SDU, REC_STOP) or length > MAX_PAYLOAD:
                self.skipped += 1
                del self.buf[:1]
                continue
            if len(self.buf) < HDR.size + length:
                break
            payload = bytes(self.buf[HDR.size:HDR.size + length])
            del self.buf[:HDR.size + length]
            if self.next_seq is not None and seq != self.next_seq:
                self.lost += (seq - self.next_seq) & 0xFFFF
            self.next_seq = (seq + 1) & 0xFFFF
            out.append((rtype, idx, payload))
        return out


# ---------------- Giai ma + ghep kenh (cung quy tac voi audio_out.c) ----------------

class Stream:
    def __init__(self, lc3, cfg):
        self.cfg = cfg
        self.freq, self.dur, self.octets, self.chans, alloc = cfg
        self.dec = [Lc3Decoder(lc3, self.dur, self.freq) for _ in range(self.chans)]
        if self.chans >= 2:
            self.out = [CH_L, CH_R]
        elif alloc & LOC_FL and not alloc & LOC_FR:
            self.out = [CH_L]
        elif alloc & LOC_FR and not alloc & LOC_FL:
            self.out = [CH_R]
        else:
            self.out = [CH_L | CH_R]  # mono -> ca hai loa
        self.last_seq = None
        self.last_ts = None

    def describe(self):
        if self.chans >= 2:
            return "L+R"
        return {CH_L: "L", CH_R: "R"}.get(self.out[0], "L=R (mono)")


def ts_before(a, b):
    """a xay ra truoc b (timestamp us 32 bit, co the tran)"""
    return a != b and ((b - a) & 0xFFFFFFFF) < 0x80000000


class Mixer:
    MAX_GAP = 50     # mat <= 50 khung lien tiep (0.5 s) thi bu bang PLC
    PAIR_WINDOW = 8  # cho kenh con lai toi da 8 khung (RTT co the bo vai ban ghi cua 1 luong)

    def __init__(self, lc3, sink):
        self.lc3 = lc3
        self.sink = sink
        self.streams = {}
        self.slots = {}  # ts -> [PCM trai, PCM phai] cua mot khoang thoi gian
        self.last_out_ts = None
        self.rate = None
        self.frame_us = None
        self.st = dict(sdu=0, bad=0, plc=0, gap=0, dec_err=0, frames=0, late=0, no_cfg=0,
                       lost=0, crc=0, empty=0, short=0)

    def handle(self, rtype, idx, payload):
        if rtype == REC_CFG and len(payload) >= CFG.size:
            freq, dur, octets, chans, _, alloc = CFG.unpack_from(payload)
            self.config(idx, (freq, dur, octets, chans, alloc))
        elif rtype == REC_SDU and len(payload) >= SDU.size:
            flags, _, seq, ts = SDU.unpack_from(payload)
            self.sdu(idx, flags, seq, ts, payload[SDU.size:])
        elif rtype == REC_STOP:
            self.stop(idx)

    def expected(self):
        mask = 0
        for s in self.streams.values():
            for o in s.out:
                mask |= o
        return mask

    def config(self, idx, cfg):
        old = self.streams.get(idx)
        if old is not None and old.cfg == cfg:
            return  # cau hinh gui lai dinh ky
        freq, dur, octets, chans, alloc = cfg
        if chans not in (1, 2) or octets == 0:
            log(f"Stream {idx}: cau hinh khong hop le {cfg}")
            return
        if self.rate is not None and (freq, dur) != (self.rate, self.frame_us):
            self.finish()  # doi tan so -> ket thuc file cu
        try:
            self.streams[idx] = Stream(self.lc3, cfg)
        except ValueError as e:
            log(f"Stream {idx}: {e}")
            return
        if self.rate is None:
            self.rate, self.frame_us = freq, dur
            self.sink.open(freq)
        log(f"Stream {idx}: LC3 {freq} Hz, {dur} us, {chans} kenh, {octets} B/khung "
            f"(~{octets * chans * 8 * 1000 // dur} kbps), alloc 0x{alloc:x} -> "
            f"{self.streams[idx].describe()}")

    def stop(self, idx):
        if self.streams.pop(idx, None) is None:
            return
        log(f"Stream {idx} dung")
        if self.streams:
            self._flush(force=True)
        else:
            self.finish()

    def finish(self):
        self._flush(force=True)
        self.streams.clear()
        self.sink.close()
        self.rate = self.frame_us = self.last_out_ts = None

    def sdu(self, idx, flags, seq, ts, data):
        s = self.streams.get(idx)
        if s is None:
            self.st["no_cfg"] += 1  # chua nhan cau hinh (mach gui lai moi ~1 s)
            return
        self.st["sdu"] += 1
        if not flags & ISO_FLAG_TS:
            ts = (s.last_ts + s.dur) & 0xFFFFFFFF if s.last_ts is not None else 0
        if s.last_seq is not None:
            gap = (seq - s.last_seq - 1) & 0xFFFF
            if 0 < gap <= self.MAX_GAP:
                self.st["gap"] += gap
                for k in range(1, gap + 1):
                    self._decode(s, None, (s.last_ts + k * s.dur) & 0xFFFFFFFF)
        valid = bool(flags & ISO_FLAG_VALID) and len(data) >= s.octets * s.chans
        if not valid:
            self.st["bad"] += 1
            if flags & ISO_FLAG_LOST:
                self.st["lost"] += 1
            elif flags & ISO_FLAG_ERROR:
                self.st["crc"] += 1
            elif not data:
                self.st["empty"] += 1
            else:
                self.st["short"] += 1
        self._decode(s, data if valid else None, ts)
        s.last_seq, s.last_ts = seq, ts
        self._flush(newest=ts)

    def _decode(self, s, data, ts):
        # Khung nay da phat roi (den qua tre) -> van giai ma de bo giai ma giu dung trang thai
        late = self.last_out_ts is not None and not ts_before(self.last_out_ts, ts)
        if late:
            self.st["late"] += 1
        slot = [None, None] if late else self.slots.setdefault(ts, [None, None])
        for c, dec in enumerate(s.dec):
            frame = data[c * s.octets:(c + 1) * s.octets] if data is not None else None
            ret, pcm = dec.decode(frame)
            if ret == 1:
                self.st["plc"] += 1
            elif ret < 0:
                self.st["dec_err"] += 1
            if s.out[c] & CH_L:
                slot[0] = pcm
            if s.out[c] & CH_R:
                slot[1] = pcm

    def _flush(self, newest=None, force=False):
        exp = self.expected()
        if newest is None:
            order = sorted(self.slots)
        else:
            order = sorted(self.slots, key=lambda t: (t - newest + 0x80000000) & 0xFFFFFFFF)
        for ts in order:
            left, right = self.slots[ts]
            complete = (not exp & CH_L or left is not None) and (not exp & CH_R or right is not None)
            # Cho qua PAIR_WINDOW khung ma van thieu kenh -> phat luon (chep kenh con lai)
            stale = newest is not None and self.frame_us and ts_before(ts, newest) and \
                ((newest - ts) & 0xFFFFFFFF) >= self.PAIR_WINDOW * self.frame_us
            if not (complete or stale or force):
                break
            del self.slots[ts]
            self.last_out_ts = ts
            self._emit(left, right)

    def _emit(self, left, right):
        left = left if left is not None else right
        right = right if right is not None else left
        if left is None:
            return
        out = array("h", bytes(4 * len(left)))
        out[0::2] = left
        out[1::2] = right
        self.sink.write(out.tobytes())
        self.st["frames"] += 1


# ---------------- Dau ra: file WAV + loa may tinh ----------------

class AudioSink:
    def __init__(self, out_dir, play, sink_name=None, prefix="helmet"):
        self.out_dir = out_dir
        self.play = play
        self.sink_name = sink_name
        self.prefix = prefix
        self.wav = None
        self.path = None
        self.player = None
        self.rate = None
        self.nbytes = 0
        self.saved = []

    def open(self, rate):
        self.close()
        self.rate = rate
        self.nbytes = 0
        if self.out_dir is not None:
            self.out_dir.mkdir(parents=True, exist_ok=True)
            stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            self.path = self.out_dir / f"{self.prefix}_{stamp}_{rate // 1000}k.wav"
            self.wav = wave.open(str(self.path), "wb")
            self.wav.setnchannels(2)
            self.wav.setsampwidth(2)
            self.wav.setframerate(rate)
            log(f"Ghi file {self.path}")
        if self.play:
            cmd = ["paplay", "--raw", "--format=s16le", f"--rate={rate}", "--channels=2",
                   "--latency-msec=100", "--client-name=helmet_audio",
                   "--stream-name=Helmet LE Audio capture"]
            if self.sink_name:
                cmd.append(f"--device={self.sink_name}")
            try:
                # Phien rieng: Ctrl+C chi dung script, paplay duoc dong gon gang
                self.player = subprocess.Popen(cmd, stdin=subprocess.PIPE, start_new_session=True)
                log(f"Phat ra loa may tinh ({self.sink_name or 'loa mac dinh'})")
            except OSError as e:
                log(f"Khong chay duoc paplay ({e}) - chi luu WAV")
                self.play = False

    def write(self, pcm: bytes):
        self.nbytes += len(pcm)
        if self.wav is not None:
            self.wav.writeframes(pcm)
        if self.player is not None:
            try:
                self.player.stdin.write(pcm)
            except (BrokenPipeError, OSError):
                log("paplay da dung - tat phat loa, van luu WAV")
                self.player = None

    def close(self):
        if self.wav is not None:
            self.wav.close()
            secs = self.nbytes / 4 / self.rate if self.rate else 0
            log(f"Da luu {self.path} ({secs:.1f} s)")
            self.saved.append(self.path)
            self.wav = None
        if self.player is not None:
            try:
                self.player.stdin.close()
                self.player.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                self.player.kill()
            self.player = None
        self.rate = None


class MemorySink(AudioSink):
    """--selftest: giu lai PCM de kiem tra, dong thoi luu WAV de nghe"""

    def __init__(self, out_dir, prefix):
        super().__init__(out_dir, play=False, prefix=prefix)
        self.sessions = []

    def open(self, rate):
        super().open(rate)
        self.sessions.append((rate, bytearray()))

    def write(self, pcm):
        super().write(pcm)
        self.sessions[-1][1].extend(pcm)


# ---------------- Che do chay ----------------

def rtt_session_open(port=19021):
    try:
        out = subprocess.run(["ss", "-ltnH", f"sport = :{port}"], capture_output=True,
                             text=True, timeout=3).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return bool(out.strip())


def stats_line(mixer, parser, elapsed, since_bytes=0):
    """elapsed: thoi gian tu lan in truoc; since_bytes: so byte da dem o lan in truoc"""
    st = mixer.st
    kbs = (parser.bytes - since_bytes) / 1024 / elapsed if elapsed > 0 else 0
    bad = st["bad"]
    detail = (f" = mat {st['lost']}, sai CRC {st['crc']}, rong {st['empty']}, ngan {st['short']}"
              if bad else "")
    line = (f"{st['frames']} khung, SDU {st['sdu']} (loi {bad}{detail}), PLC {st['plc']}, "
            f"mat qua RTT {parser.lost} ban ghi, {kbs:.1f} KB/s")
    if st["sdu"] >= 50 and bad * 10 >= st["sdu"] * 9:
        worst = max(("lost", "crc", "empty", "short"), key=lambda k: st[k])
        line += "\n[audio]   -> " + {
            "lost": "Helmet KHONG nhan duoc du lieu nao trong cac su kien CIS: phia phat mo kenh "
                    "nhung khong gui (xem 'btmon' ben phat: co goi ISO Data TX khong?)",
            "crc": "Goi den nhung sai CRC: van de song RF (khoang cach, anten, nhieu)",
            "empty": "Phia phat gui SDU do dai 0: ung dung phat chua dua mau am thanh vao",
            "short": "SDU ngan hon cau hinh codec: phia phat gui sai kich thuoc khung",
        }[worst]
    return line


def run_live(args, lc3):
    if rtt_session_open():
        sys.exit("[audio] Dang co mot phien 'make rtt' mo (cong 19021) - dong no truoc "
                 "(Ctrl+C trong terminal do). Script nay da hien log + shell thay cho no.")

    elf = find_elf(args.build)
    addr = elf_symbol(elf, "_SEGGER_RTT")
    if addr is None:
        sys.exit(f"[audio] {elf} khong co _SEGGER_RTT")

    jl = JLink(args.jlink_lib)
    jl.open(args.sn, args.device, args.speed)
    sink = AudioSink(None if args.no_wav else args.out, play=not args.no_play,
                     sink_name=args.sink)
    parser = RecordParser()
    mixer = Mixer(lc3, sink)
    raw = open(args.raw, "wb") if args.raw else None
    use_stdin = sys.stdin.isatty()

    try:
        jl.rtt_start(addr)
        name = jl.rtt_up_name(RTT_CH_AUDIO)
        log(f"J-Link OK, RTT @ 0x{addr:08x} ({elf.relative_to(ROOT)})")
        if name != RTT_AUDIO_NAME:
            log(f"CANH BAO: firmware tren mach chua co kenh '{RTT_AUDIO_NAME}' (thay '{name}') - "
                "hay nap ban moi: make build flash")
        jl.write(RTT_CH_LOG, b"\nhelmet capture on\n")
        log("Da bat 'helmet capture on'. Mo nhac tren thiet bi phat LE Audio. "
            "Go lenh shell (vd: helmet status) + Enter. Ctrl+C de thoat.")

        t0 = last = time.time()
        last_bytes = 0
        while True:
            busy = False
            text = jl.read(RTT_CH_LOG)
            if text:
                sys.stdout.write(text.decode("utf-8", "replace"))
                sys.stdout.flush()
                busy = True
            data = jl.read(RTT_CH_AUDIO)
            if data:
                busy = True
                if raw:
                    raw.write(data)
                for rec in parser.feed(data):
                    mixer.handle(*rec)
            if use_stdin and select.select([sys.stdin], [], [], 0)[0]:
                line = sys.stdin.readline()
                if line:
                    jl.write(RTT_CH_LOG, line.rstrip("\r\n").encode() + b"\n")
                else:
                    use_stdin = False
            now = time.time()
            if now - last >= args.stats:
                if mixer.st["sdu"]:
                    log(stats_line(mixer, parser, now - last, last_bytes))
                last, last_bytes = now, parser.bytes
            if not busy:
                time.sleep(0.005)
    except KeyboardInterrupt:
        print()
    finally:
        try:
            if jl.rtt_running:
                jl.write(RTT_CH_LOG, b"helmet capture off\n")
                time.sleep(0.1)
        except Exception:  # noqa: BLE001 - dang thoat, bo qua loi J-Link
            pass
        mixer.finish()
        jl.close()
        if raw:
            raw.close()
            log(f"Du lieu tho: {args.raw}")
        if mixer.st["sdu"]:
            log("Tong ket (KB/s trung binh): " + stats_line(mixer, parser, time.time() - t0))
        elif not sink.saved:
            log("Khong nhan duoc goi am thanh nao (chua co luong LE Audio toi Helmet?)")


def run_replay(args, lc3):
    sink = AudioSink(args.out, play=False, prefix=Path(args.replay).stem)
    parser = RecordParser()
    mixer = Mixer(lc3, sink)
    for rec in parser.feed(Path(args.replay).read_bytes()):
        mixer.handle(*rec)
    mixer.finish()
    log(stats_line(mixer, parser, 0))


def tone(freq, rate, start, n, amp=8000):
    w = 2 * math.pi * freq / rate
    return array("h", (int(amp * math.sin(w * (start + i))) for i in range(n)))


def zero_cross_freq(samples, rate):
    crossings = sum(1 for a, b in zip(samples, samples[1:]) if (a < 0) != (b < 0))
    return crossings / 2 / (len(samples) / rate)


def run_selftest(args, lc3):
    """Ma hoa tin hieu mau bang liblc3, dong goi y het firmware (co chen rac, goi loi,
    ban ghi bi bo), roi cho qua dung duong ong tach ban ghi -> giai ma -> ghep kenh."""
    cases = [
        # ten, tan so, khung us, octet, cac luong [(stream, so kenh, alloc, [tan so tone])],
        # cac khung bi bo tren RTT (cua luong cuoi)
        ("stereo_1cis_48k", 48000, 10000, 120, [(0, 2, LOC_FL | LOC_FR, [440, 1000])], (120, 121)),
        ("2cis_LR_48k", 48000, 10000, 100, [(0, 1, LOC_FL, [440]), (1, 1, LOC_FR, [1000])],
         (120, 121)),
        ("2cis_long_drop", 48000, 10000, 100, [(0, 1, LOC_FL, [440]), (1, 1, LOC_FR, [1000])],
         tuple(range(120, 132))),
        ("mono_16k_7.5ms", 16000, 7500, 30, [(0, 1, 0, [700])], (120, 121)),
    ]
    frames = 200
    ok_all = True
    for name, rate, dur, octets, streams, drop_ks in cases:
        encs = {(sid, c): Lc3Encoder(lc3, dur, rate) for sid, chans, _, _ in streams
                for c in range(chans)}
        ns = next(iter(encs.values())).samples
        stream = bytearray(b"\x00\x11\xa5\x22")  # rac o dau: kiem tra do lai dau ban ghi
        seq = 0
        dropped = exp_plc = 0  # PLC dem theo tung kenh LC3

        def rec(rtype, idx, payload, drop=False):
            nonlocal seq
            if not drop:
                stream.extend(HDR.pack(SYNC, rtype, idx, len(payload), seq & 0xFFFF) + payload)
            seq += 1

        for sid, chans, alloc, _ in streams:
            rec(REC_CFG, sid, CFG.pack(rate, dur, octets, chans, 0, alloc))
        for k in range(frames):
            for sid, chans, alloc, freqs in streams:
                lc3_data = b"".join(encs[(sid, c)].encode(tone(freqs[c], rate, k * ns, ns), octets)
                                    for c in range(chans))
                flags = ISO_FLAG_VALID | ISO_FLAG_TS
                if k == 50 and sid == 0:
                    flags = ISO_FLAG_TS  # goi loi -> PLC
                    exp_plc += chans
                drop = k in drop_ks and sid == streams[-1][0]  # bo dem RTT day
                dropped += drop
                exp_plc += chans * drop
                rec(REC_SDU, sid, SDU.pack(flags, 0, k, k * dur) + lc3_data, drop=drop)
        for sid, *_ in streams:
            rec(REC_STOP, sid, b"")

        sink = MemorySink(args.out, prefix=f"selftest_{name}")
        parser = RecordParser()
        mixer = Mixer(lc3, sink)
        # Cat thanh tung manh ngau nhien nhu khi doc qua J-Link
        pos, step = 0, 1
        while pos < len(stream):
            for r in parser.feed(stream[pos:pos + step]):
                mixer.handle(*r)
            pos += step
            step = (step * 7 + 13) % 997 + 1
        mixer.finish()

        problems = []
        if len(sink.sessions) != 1:
            problems.append(f"{len(sink.sessions)} phien (mong doi 1)")
        rate_out, pcm_bytes = sink.sessions[0] if sink.sessions else (0, b"")
        pcm = array("h", bytes(pcm_bytes))
        got_frames = len(pcm) // 2 // ns
        if got_frames != frames:
            problems.append(f"{got_frames} khung (mong doi {frames})")
        if parser.lost != dropped:
            problems.append(f"phat hien mat {parser.lost} ban ghi (mong doi {dropped})")
        if mixer.st["plc"] != exp_plc:
            problems.append(f"PLC {mixer.st['plc']} (mong doi {exp_plc})")
        if mixer.st["late"] and len(drop_ks) < Mixer.PAIR_WINDOW:
            problems.append(f"{mixer.st['late']} khung den tre")
        body = slice(10 * ns * 2, (frames - 10) * ns * 2)
        left, right = pcm[body][0::2], pcm[body][1::2]
        want = [f for _, _, _, fs in streams for f in fs]
        want_l, want_r = (want[0], want[-1])
        fl, fr = zero_cross_freq(left, rate), zero_cross_freq(right, rate)
        for side, got, exp in (("L", fl, want_l), ("R", fr, want_r)):
            if abs(got - exp) > exp * 0.03:
                problems.append(f"kenh {side} {got:.0f} Hz (mong doi {exp} Hz)")
        status = "PASS" if not problems else "FAIL: " + "; ".join(problems)
        ok_all &= not problems
        log(f"selftest {name:16s} L {fl:6.0f} Hz, R {fr:6.0f} Hz, {got_frames} khung, "
            f"PLC {mixer.st['plc']}, mat {parser.lost}, tre {mixer.st['late']} -> {status}")
    log("SELFTEST " + ("PASS" if ok_all else "FAIL"))
    return 0 if ok_all else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n\n", 1)[1])
    ap.add_argument("--build", type=Path, default=ROOT / "build/main",
                    help="thu muc build cua Helmet (de lay dia chi RTT)")
    ap.add_argument("--sn", default=os.environ.get("JLINK_SN"), help="so serial J-Link")
    ap.add_argument("--device", default="nRF54L15_M33", help="ten chip cho J-Link")
    ap.add_argument("--speed", type=int, default=4000, help="toc do SWD (kHz)")
    ap.add_argument("--jlink-lib", default=os.environ.get("JLINK_LIB",
                                                          "/opt/SEGGER/JLink/libjlinkarm.so"))
    ap.add_argument("--out", type=Path, default=ROOT / "build/audio", help="thu muc luu WAV")
    ap.add_argument("--no-wav", action="store_true", help="khong luu WAV")
    ap.add_argument("--no-play", action="store_true", help="khong phat ra loa may tinh")
    ap.add_argument("--sink", help="loa phat (ten trong 'pactl list short sinks')")
    ap.add_argument("--raw", help="luu them dong du lieu tho cua kenh RTT 1")
    ap.add_argument("--stats", type=float, default=10, help="chu ky in thong ke (s)")
    ap.add_argument("--replay", help="giai ma lai file tho (--raw) ra WAV, khong can mach")
    ap.add_argument("--selftest", action="store_true",
                    help="tu kiem tra tach ban ghi + giai ma LC3 tren PC")
    args = ap.parse_args()

    lc3 = Lc3()
    if args.selftest:
        return run_selftest(args, lc3)
    if args.replay:
        run_replay(args, lc3)
        return 0
    run_live(args, lc3)
    return 0


if __name__ == "__main__":
    sys.exit(main())
