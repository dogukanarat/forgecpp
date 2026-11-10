"""c2 commands on a serial line, framed as zephyrus frames them (modules/lib/cobs).

The wire format (docs/protocol.md): a c2 frame is one message; a message is
cut into fragments, and each fragment is

    0x00 | COBS(flags | seq | total | part of the message | CRC16 little-endian) | 0x00

flags: 1 = first, 2 = last. CRC16-CCITT: poly 0x1021, start 0xFFFF.

    from forge import serial, messages_pb2 as m

    link = serial.Client("/dev/ttyUSB0", baud=115200)
    print(link.call(m.info, m.info_request(), m.info_reply).name)

Only the standard library: no pyserial. A refused command raises c2.Refused.
"""
import os
import select
import struct
import termios
import time
import tty

from . import c2

__all__ = ["Client", "Framing", "crc16", "encode_frame", "decode_frame"]

_HEADER = struct.Struct(">BBHHH")
FIRST, LAST = 1, 2


def crc16(data, crc=0xFFFF):
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_frame(payload):
    """One frame with both delimiters."""
    data = bytes(payload) + struct.pack("<H", crc16(payload))
    out = bytearray(b"\x00\x00")
    code_at, code = 1, 1
    for byte in data:
        if byte == 0:
            out[code_at], code, code_at = code, 1, len(out)
            out.append(0)
            continue
        out.append(byte)
        code += 1
        if code == 0xFF:
            out[code_at], code, code_at = code, 1, len(out)
            out.append(0)
    out[code_at] = code
    out.append(0)
    return bytes(out)


def decode_frame(encoded):
    """The payload of the bytes between two delimiters, or None if it is damaged."""
    out, i = bytearray(), 0
    while i < len(encoded):
        code = encoded[i]
        i += 1
        if code == 0 or i + code - 1 > len(encoded):
            return None
        out += encoded[i:i + code - 1]
        i += code - 1
        if code != 0xFF and i < len(encoded):
            out.append(0)
    if len(out) < 2 or crc16(out[:-2]) != struct.unpack("<H", out[-2:])[0]:
        return None
    return bytes(out[:-2])


class Framing:
    """Messages to bytes and bytes to messages, as zephyrus's cobs_framing."""

    def __init__(self, frame_size=244, max_message=16384):
        self.frame_size, self.max_message = frame_size, max_message
        self._rx, self._in_frame = bytearray(), False
        self._message, self._assembling, self._expected, self._total = bytearray(), False, 0, 0

    def encode(self, message):
        part = self.frame_size - 3 - 5 - self.frame_size // 254
        total = (len(message) + part - 1) // part
        if not message or len(message) > self.max_message or total > 255:
            raise ValueError(f"a message of {len(message)} bytes does not fit the link")
        out = bytearray()
        for seq in range(total):
            flags = (FIRST if seq == 0 else 0) | (LAST if seq == total - 1 else 0)
            out += encode_frame(bytes([flags, seq, total]) + message[seq * part:(seq + 1) * part])
        return bytes(out)

    def receive(self, data):
        """The complete messages in `data` (and the bytes before it)."""
        messages = []
        for byte in data:
            if byte == 0:
                if self._in_frame and self._rx:
                    self._fragment(decode_frame(bytes(self._rx)), messages)
                    self._in_frame = False
                else:
                    self._in_frame = True
                self._rx.clear()
            elif self._in_frame:
                self._rx.append(byte)
        return messages

    def _fragment(self, frame, messages):
        if frame is None or len(frame) < 3:
            return
        flags, seq, total = frame[0], frame[1], frame[2]
        if flags & FIRST:
            self._message, self._assembling, self._expected, self._total = bytearray(), True, 0, total
        if not self._assembling:
            return
        if seq != self._expected or total != self._total or len(self._message) + len(frame) - 3 > self.max_message:
            self._assembling = False
            return
        self._message += frame[3:]
        self._expected = (self._expected + 1) & 0xFF
        if flags & LAST:
            self._assembling = False
            messages.append(bytes(self._message))


_BAUD = {b: getattr(termios, f"B{b}") for b in (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)
         if hasattr(termios, f"B{b}")}


class Client(c2.Calls):
    """Requests to the other end of a serial line (a device, or a forgecpp
    program with [forge].serial). Reports (push) that come in are kept in
    `pushes` as (cmd_id, payload)."""

    def __init__(self, device=None, baud=115200, timeout_ms=1000, frame_size=244, fd=None,
                 frame_per_write=False):
        """`fd`: an open descriptor instead of a tty. `frame_per_write`: write
        each frame on its own, for a transport where one write is one message
        (an ISO-TP socket: Client.isotp())."""
        if fd is None:
            fd = os.open(device, os.O_RDWR | os.O_NOCTTY)
            tty.setraw(fd)
            attrs = termios.tcgetattr(fd)
            attrs[4] = attrs[5] = _BAUD[baud]
            attrs[2] = (attrs[2] & ~(termios.CSTOPB | getattr(termios, "CRTSCTS", 0))) | termios.CLOCAL | termios.CREAD
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            termios.tcflush(fd, termios.TCIOFLUSH)
        self.fd, self.timeout_ms = fd, timeout_ms
        self.device = device or f"fd:{fd}"
        self._framing = Framing(frame_size)
        self._frame_per_write = frame_per_write
        self._seq = 0
        self.pushes = []

    @classmethod
    def isotp(cls, interface, tx_id=0x700, rx_id=0x701, extended=False, padding=0xCC, **kwargs):
        """The same commands on a CAN bus: an ISO-TP channel to a forgecpp
        program with [can].interface (its rx_id is our tx_id). Linux only."""
        import socket
        s = socket.socket(socket.AF_CAN, socket.SOCK_DGRAM, socket.CAN_ISOTP)
        if padding is not None:
            sol_can_isotp, can_isotp_opts, tx_padding = 106, 1, 0x004  # <linux/can/isotp.h>
            s.setsockopt(sol_can_isotp, can_isotp_opts, struct.pack("=IIBBBB", tx_padding, 0, 0, padding, 0, 0))
        eff = 0x80000000 if extended else 0
        s.bind((interface, rx_id | eff, tx_id | eff))
        client = cls(fd=s.detach(), frame_per_write=True, **kwargs)
        client.device = f"{interface} 0x{tx_id:X}/0x{rx_id:X}"
        return client

    def _write(self, data):
        if not self._frame_per_write:
            os.write(self.fd, data)
            return
        start = 0
        while start < len(data):  # each frame: a delimiter, COBS bytes, a delimiter
            end = data.index(0, start + 1) + 1
            os.write(self.fd, data[start:end])
            start = end

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _request(self, cmd_id, payload):
        self._seq = (self._seq + 1) & 0xFFFF
        frame = _HEADER.pack(c2.VERSION, c2.REQ, cmd_id, self._seq, len(payload)) + payload
        self._write(self._framing.encode(frame))
        answers, deadline = [], time.monotonic() + self.timeout_ms / 1000
        while True:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                raise TimeoutError(f"{self.device}: no answer to command 0x{cmd_id:04x}")
            for message in self._framing.receive(os.read(self.fd, 4096)):
                if len(message) < _HEADER.size:
                    continue  # not a c2 frame
                version, flags, cid, seq, length = _HEADER.unpack_from(message)
                if version != c2.VERSION or len(message) != _HEADER.size + length:
                    continue  # another protocol version, or a frame that does not add up: dropped
                body = message[_HEADER.size:]
                if flags & c2.PUSH and not flags & (c2.RSP | c2.ACK | c2.NACK):
                    self.pushes.append((cid, body))
                    continue
                if cid != cmd_id or seq != self._seq:
                    continue  # late, for an earlier request
                if flags & c2.NACK:
                    raise c2.Refused(cmd_id, struct.unpack(">I", body)[0])
                answers.append((flags, body))
                if not (flags & c2.RSP and flags & c2.MORE):
                    return answers
                deadline = time.monotonic() + self.timeout_ms / 1000  # the next part
