#!/usr/bin/env python3
import argparse
import queue
import sys
import threading
import time

import serial
import sounddevice as sd
from array import array
import math


DEFAULT_SERIAL_RATE = 801
DEFAULT_AUDIO_RATE = 8000
DEFAULT_BAUD = 9600
DEFAULT_BLOCK = 80
MAX_RX_BUFFER_LATENCY_MS = 40
SILENCE_BYTE = 0x80
SILENCE = bytes([SILENCE_BYTE])


class Stats:
    def __init__(self):
        self.tx_drops = 0
        self.rx_drops = 0
        self.audio_status = False
        self.serial_rx_bytes = 0
        self.serial_tx_bytes = 0
        self.rx_level_peak = 0
        self.tx_level_peak = 0
        self.tx_mute_until = 0.0


def parse_device(value):
    if value is None:
        return None
    value = value.strip()
    if value.isdigit():
        return int(value)
    return value


def serial_reader(ser, rx_queue, running, stats):
    while running.is_set():
        data = ser.read(ser.in_waiting or 1)
        if data:
            stats.serial_rx_bytes += len(data)
            stats.tx_mute_until = time.monotonic() + 0.08
            peak = max(abs(sample - SILENCE_BYTE) for sample in data)
            if peak > stats.rx_level_peak:
                stats.rx_level_peak = peak
            try:
                rx_queue.put_nowait(data)
            except queue.Full:
                try:
                    rx_queue.get_nowait()
                except queue.Empty:
                    pass
                try:
                    rx_queue.put_nowait(data)
                except queue.Full:
                    stats.rx_drops += len(data)
        else:
            time.sleep(0.001)


def serial_writer(ser, tx_queue, running, stats):
    while running.is_set():
        try:
            data = tx_queue.get(timeout=0.1)
        except queue.Empty:
            continue
        if data:
            if time.monotonic() < stats.tx_mute_until:
                continue
            try:
                ser.write(data)
                stats.serial_tx_bytes += len(data)
                peak = max(abs(sample - SILENCE_BYTE) for sample in data)
                if peak > stats.tx_level_peak:
                    stats.tx_level_peak = peak
            except serial.SerialTimeoutException:
                stats.tx_drops += len(data)


def make_audio_callback(rx_queue, tx_queue, stats, enable_tx, enable_rx, audio_rate, serial_rate, agc_enabled=True):
    rx_buffer = bytearray()
    tx_accum = 0
    rx_accum = audio_rate
    playback_sample = SILENCE_BYTE
    prev_gain = 1.0
    max_rx_buffer_bytes = max(32, int(serial_rate * MAX_RX_BUFFER_LATENCY_MS / 1000))

    # host audio e pe 16-bit int
    # convertim intre int16 si u8 pentru serial
    def callback(indata, outdata, frames, time_info, status):
        nonlocal tx_accum, rx_accum, playback_sample, prev_gain

        if status:
            stats.audio_status = True

        # indata si outdata sunt raw bytes pentru RawStream dtype='int16'
        # folosim memoryview pentru acces ca int16
        in_mv = memoryview(indata).cast('h')
        out_mv = memoryview(outdata).cast('h')
        tx_muted = time.monotonic() < stats.tx_mute_until

        # tx: convertim int16 -> u8 si facem downsample la serial_rate
        if enable_tx and not tx_muted:
            tx_chunk = bytearray()
            if agc_enabled and len(in_mv) > 0:
                # calculez medie si rms pe bloc
                n = len(in_mv)
                ssum = 0
                for s in in_mv:
                    ssum += s
                mean = ssum / n
                ssq = 0
                for s in in_mv:
                    t = s - mean
                    ssq += t * t
                rms = math.sqrt(ssq / n) if n > 0 else 0.0
                # rms 0.12 din full-scale
                target_rms = 0.12 * 32767.0
                desired_gain = target_rms / (rms + 1e-12)
                # Limitez gain la o plaja rezonabila
                if desired_gain < 0.5:
                    desired_gain = 0.5
                if desired_gain > 8.0:
                    desired_gain = 8.0
                # smooth schimbarea de gain
                if desired_gain > prev_gain:
                    prev_gain = prev_gain + (desired_gain - prev_gain) * 0.35
                else:
                    prev_gain = prev_gain + (desired_gain - prev_gain) * 0.05

            for s in in_mv:
                # dc removal
                if agc_enabled:
                    s = int(s - mean)
                    s = int(max(-32768, min(32767, int(s * prev_gain))))
                # mapam int16 -> u8
                sample8 = ((s >> 8) + 128) & 0xFF
                tx_accum += serial_rate
                if tx_accum >= audio_rate:
                    tx_chunk.append(sample8)
                    tx_accum -= audio_rate
            if tx_chunk:
                try:
                    tx_queue.put_nowait(bytes(tx_chunk))
                except queue.Full:
                    stats.tx_drops += len(tx_chunk)
        else:
            tx_accum = 0

        # daca e dezactivat rx iesim cu silence
        if not enable_rx:
            rx_buffer.clear()
            out_mv[:] = array('h', [0]) * len(out_mv)
            return

        # scoatem toate chunk-urile primite din coada si le punem in rx_buffer
        while True:
            try:
                chunk = rx_queue.get_nowait()
            except queue.Empty:
                break
            rx_buffer.extend(chunk)

        # pastram doar ultimele pt a face intarzierea mica
        if len(rx_buffer) > max_rx_buffer_bytes:
            del rx_buffer[:-max_rx_buffer_bytes]

        # playback pt fiecare frame iau urmatorul u8 si convertesc la int16
        for idx in range(len(out_mv)):
            rx_accum += serial_rate
            if rx_accum >= audio_rate:
                if rx_buffer:
                    playback_sample = rx_buffer[0]
                    del rx_buffer[0]
                else:
                    playback_sample = SILENCE_BYTE
                rx_accum -= audio_rate
            out_mv[idx] = (int(playback_sample) - 128) << 8

    return callback


def level_bar(level, width=24):
    filled = min(width, int((level * width + 63) / 127))
    return "[" + "#" * filled + "." * (width - filled) + "]"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Serial <-> audio bridge for Arduino-Shell walkie-talkie mode."
    )
    parser.add_argument("--port", help="Serial port (e.g. COM7 or /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument(
        "--rate",
        type=int,
        default=DEFAULT_AUDIO_RATE,
        help="PC audio device sample rate",
    )
    parser.add_argument(
        "--serial-rate",
        type=int,
        default=DEFAULT_SERIAL_RATE,
        help="8-bit PCM sample rate used on the serial link",
    )
    parser.add_argument("--block", type=int, default=DEFAULT_BLOCK)
    parser.add_argument("--rx-only", action="store_true", help="Receive/playback only")
    parser.add_argument("--tx-only", action="store_true", help="Transmit from PC mic only")
    parser.add_argument("--list-devices", action="store_true", help="List audio devices and exit")
    parser.add_argument("--in-device", help="Input device index or substring")
    parser.add_argument("--out-device", help="Output device index or substring")
    parser.add_argument("--no-agc", action="store_true", help="Disable AGC on TX")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.list_devices:
        print(sd.query_devices())
        return 0

    if not args.port:
        print("--port is required", file=sys.stderr)
        return 2

    enable_tx = not args.rx_only
    enable_rx = not args.tx_only

    in_device = parse_device(args.in_device)
    out_device = parse_device(args.out_device)
    device = None
    if in_device is not None or out_device is not None:
        device = (in_device, out_device)

    try:
        ser = serial.Serial(
            args.port,
            args.baud,
            timeout=0,
            write_timeout=0,
        )
    except serial.SerialException as exc:
        print(f"Failed to open serial port: {exc}", file=sys.stderr)
        return 1

    ser.reset_input_buffer()
    ser.reset_output_buffer()

    rx_queue = queue.Queue(maxsize=64)
    tx_queue = queue.Queue(maxsize=64)
    stats = Stats()
    running = threading.Event()
    running.set()

    reader_thread = threading.Thread(
        target=serial_reader, args=(ser, rx_queue, running, stats), daemon=True
    )
    writer_thread = threading.Thread(
        target=serial_writer, args=(ser, tx_queue, running, stats), daemon=True
    )
    reader_thread.start()
    writer_thread.start()

    callback = make_audio_callback(
        rx_queue,
        tx_queue,
        stats,
        enable_tx,
        enable_rx,
        args.rate,
        args.serial_rate,
        agc_enabled=(not args.no_agc),
    )

    print("Starting audio stream...")
    print(f"Serial: {args.port} @ {args.baud} baud")
    print(f"Audio device: {args.rate} Hz, block {args.block} frames")
    print(f"Serial PCM: {args.serial_rate} Hz")
    print("Press Ctrl+C to stop.")

    try:
        with sd.RawStream(
            samplerate=args.rate,
            blocksize=args.block,
            dtype="int16",
            channels=1,
            device=device,
            callback=callback,
        ):
            last_rx = 0
            last_tx = 0
            while True:
                time.sleep(0.5)
                rx_now = stats.serial_rx_bytes
                tx_now = stats.serial_tx_bytes
                rx_bps = int((rx_now - last_rx) * 2)
                tx_bps = int((tx_now - last_tx) * 2)
                last_rx = rx_now
                last_tx = tx_now
                rx_peak = stats.rx_level_peak
                tx_peak = stats.tx_level_peak
                stats.rx_level_peak = 0
                stats.tx_level_peak = 0
                print(
                    f"RX {rx_bps:4d} B/s {level_bar(rx_peak)} peak={rx_peak:3d} | "
                    f"TX {tx_bps:4d} B/s {level_bar(tx_peak)} peak={tx_peak:3d}",
                    flush=True,
                )
    except KeyboardInterrupt:
        pass
    finally:
        running.clear()
        time.sleep(0.1)
        ser.close()

    if stats.audio_status:
        print("Audio status warnings occurred.")
    print(f"TX drops: {stats.tx_drops}")
    print(f"RX drops: {stats.rx_drops}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
