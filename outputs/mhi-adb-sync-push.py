#!/usr/bin/env python3
"""Upload one file through the RM505Q PCIe MHI ADB channel.

This uses ADB's sync service directly rather than `adb push`.  It keeps the
working legacy CNXN greeting, sends every ADB message as a normal framed TCP
packet (the router bridge splits its header/payload correctly for MHI), and
waits for an ADB flow-control acknowledgement before each chunk.

Use a small disposable /tmp file first.  Verify the remote SHA-256 separately
with mhi-adb-run.py before attempting a real payload.
"""

import argparse
import os
import socket
import struct
import sys

HEADER = struct.Struct("<6I")
CNXN = int.from_bytes(b"CNXN", "little")
OPEN = int.from_bytes(b"OPEN", "little")
OKAY = int.from_bytes(b"OKAY", "little")
CLSE = int.from_bytes(b"CLSE", "little")
WRTE = int.from_bytes(b"WRTE", "little")
SYNC = struct.Struct("<4sI")
CHUNK = 2048  # Sync DATA header + data remains safely below ADB's 4096-byte limit.


class AdbError(RuntimeError):
    pass


def adb_id(name):
    return int.from_bytes(name.encode("ascii"), "little")


def adb_name(value):
    return value.to_bytes(4, "little").decode("ascii", "replace")


def checksum(payload):
    return sum(payload) & 0xFFFFFFFF


def packet(command, arg0=0, arg1=0, payload=b""):
    return HEADER.pack(command, arg0, arg1, len(payload), checksum(payload),
                       command ^ 0xFFFFFFFF) + payload


class Transport:
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.local_id = 1
        self.remote_id = None
        self.pending = bytearray()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send(self, command, arg0=0, arg1=0, payload=b""):
        self.sock.sendall(packet(command, arg0, arg1, payload))

    def exact(self, length):
        out = bytearray()
        while len(out) < length:
            data = self.sock.recv(length - len(out))
            if not data:
                raise AdbError("ADB transport closed")
            out.extend(data)
        return bytes(out)

    def receive(self):
        raw = self.exact(HEADER.size)
        command, arg0, arg1, length, received_sum, magic = HEADER.unpack(raw)
        if magic != (command ^ 0xFFFFFFFF):
            raise AdbError("invalid ADB header")
        payload = self.exact(length) if length else b""
        if checksum(payload) != received_sum:
            raise AdbError("invalid ADB payload checksum")
        return command, arg0, arg1, payload

    def handshake(self):
        # Featureless original greeting: known stable on this Quectel firmware.
        self.send(CNXN, 0x01000000, 0x1000, b"")
        while True:
            command, _a0, _a1, _payload = self.receive()
            if command == CNXN:
                return

    def open_sync(self):
        self.send(OPEN, self.local_id, 0, b"sync:\0")
        while True:
            command, arg0, arg1, payload = self.receive()
            if command == CNXN:
                continue
            if command == OKAY and arg1 == self.local_id:
                self.remote_id = arg0
                return
            if command == WRTE and arg1 == self.local_id:
                self.send(OKAY, self.local_id, arg0)
                self.pending.extend(payload)
                continue
            raise AdbError("unexpected while opening sync: " + adb_name(command))

    def write(self, payload):
        self.send(WRTE, self.local_id, self.remote_id, payload)
        while True:
            command, arg0, arg1, incoming = self.receive()
            if command == CNXN:
                continue
            if command == OKAY and arg0 == self.remote_id and arg1 == self.local_id:
                return
            if command == WRTE and arg1 == self.local_id:
                self.send(OKAY, self.local_id, arg0)
                self.pending.extend(incoming)
                continue
            if command == CLSE and arg1 == self.local_id:
                self.send(CLSE, self.local_id, arg0)
                raise AdbError("sync service closed unexpectedly")
            raise AdbError("unexpected while writing: " + adb_name(command))

    def take_sync_reply(self, length):
        while len(self.pending) < length:
            command, arg0, arg1, payload = self.receive()
            if command == CNXN:
                continue
            if command == WRTE and arg1 == self.local_id:
                self.send(OKAY, self.local_id, arg0)
                self.pending.extend(payload)
                continue
            if command == OKAY and arg0 == self.remote_id and arg1 == self.local_id:
                continue
            if command == CLSE and arg1 == self.local_id:
                self.send(CLSE, self.local_id, arg0)
                raise AdbError("sync service closed before reply")
            raise AdbError("unexpected while reading sync reply: " + adb_name(command))
        result = bytes(self.pending[:length])
        del self.pending[:length]
        return result

    def close_service(self):
        if self.remote_id is not None:
            self.send(CLSE, self.local_id, self.remote_id)


def push(args):
    source_size = os.path.getsize(args.local)
    remote_spec = (args.remote + ",%d" % args.mode).encode()
    if len(remote_spec) > 1024:
        raise AdbError("remote path is too long")

    transport = Transport(args.host, args.port, args.timeout)
    try:
        transport.handshake()
        transport.open_sync()
        transport.write(SYNC.pack(b"SEND", len(remote_spec)) + remote_spec)
        sent = 0
        with open(args.local, "rb") as source:
            while True:
                data = source.read(CHUNK)
                if not data:
                    break
                transport.write(SYNC.pack(b"DATA", len(data)) + data)
                sent += len(data)
                print("\rUploaded %d / %d bytes" % (sent, source_size), end="", file=sys.stderr, flush=True)
        transport.write(SYNC.pack(b"DONE", 0))
        status, value = SYNC.unpack(transport.take_sync_reply(SYNC.size))
        if status == b"FAIL":
            message = transport.take_sync_reply(value).decode("utf-8", "replace")
            raise AdbError("remote sync failure: " + message)
        if status != b"OKAY":
            raise AdbError("unexpected sync result: " + repr(status))
        print("\nUpload completed; verify its SHA-256 before using it.", file=sys.stderr)
    finally:
        transport.close_service()
        transport.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("local")
    parser.add_argument("remote")
    parser.add_argument("--mode", type=lambda text: int(text, 8), default=0o644,
                        help="remote mode in octal (default: 644)")
    parser.add_argument("--timeout", type=int, default=20)
    args = parser.parse_args()
    try:
        push(args)
    except (AdbError, OSError, socket.timeout) as exc:
        raise SystemExit("Upload did not complete: " + str(exc))


if __name__ == "__main__":
    main()
