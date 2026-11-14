#!/usr/bin/env python3
"""Run shell commands over one persistent ADB-over-MHI TCP transport.

Usage:
  ./mhi-adb-run.py 127.0.0.1 6560 'id' 'uname -a' 'ls -ld /data'

This intentionally does not use the host adb server. Some Quectel PCIe MHI
ADB endpoints do not recover reliably when that server reconnects per command,
or when more than one ADB shell service is opened. All command arguments are
therefore joined into one shell script and sent in one ADB OPEN request.
"""

import socket
import struct
import sys
import time
import secrets

HEADER = struct.Struct("<6I")
CNXN = int.from_bytes(b"CNXN", "little")
OPEN = int.from_bytes(b"OPEN", "little")
OKAY = int.from_bytes(b"OKAY", "little")
CLSE = int.from_bytes(b"CLSE", "little")
WRTE = int.from_bytes(b"WRTE", "little")
# RM505Q-AE's PCIe ADB endpoint is reliably responsive to the original,
# featureless ADB CNXN used by mhi-adb-probe.  Its newer feature banner can
# leave this firmware silent after an ADB reset.
FEATURES = b""


class AdbError(RuntimeError):
    pass


def checksum(payload):
    return sum(payload) & 0xFFFFFFFF


def packet(command, arg0=0, arg1=0, payload=b""):
    return HEADER.pack(command, arg0, arg1, len(payload), checksum(payload),
                       command ^ 0xFFFFFFFF) + payload


def command_name(value):
    return value.to_bytes(4, "little").decode("ascii", "replace")


class AdbTransport:
    def __init__(self, host, port, timeout=20):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.next_local_id = 1

    def close(self):
        self.sock.close()

    def send(self, command, arg0=0, arg1=0, payload=b""):
        self.sock.sendall(packet(command, arg0, arg1, payload))

    def receive_exact(self, amount):
        chunks = []
        while amount:
            data = self.sock.recv(amount)
            if not data:
                raise AdbError("ADB TCP transport closed")
            chunks.append(data)
            amount -= len(data)
        return b"".join(chunks)

    def receive(self):
        raw = self.receive_exact(HEADER.size)
        command, arg0, arg1, length, received_sum, magic = HEADER.unpack(raw)
        if magic != (command ^ 0xFFFFFFFF):
            raise AdbError("invalid ADB header magic")
        payload = self.receive_exact(length) if length else b""
        if checksum(payload) != received_sum:
            raise AdbError("invalid ADB payload checksum")
        return command, arg0, arg1, payload

    def handshake(self):
        self.send(CNXN, 0x01000000, 0x1000, FEATURES)
        while True:
            command, _arg0, _arg1, _payload = self.receive()
            if command == CNXN:
                return
            # The bridge suppresses known delayed greetings.  Ignore any
            # remaining stale traffic until the device greeting arrives.

    def shell(self, text, completion_marker):
        local_id = self.next_local_id
        self.next_local_id += 1
        self.send(OPEN, local_id, 0, b"shell:" + text.encode() + b"\0")
        remote_id = None
        output = bytearray()
        while True:
            command, arg0, arg1, payload = self.receive()
            if command == CNXN:
                continue
            if command == OKAY and arg1 == local_id:
                remote_id = arg0
                continue
            if command == WRTE and arg1 == local_id:
                remote_id = arg0 if remote_id is None else remote_id
                output.extend(payload)
                self.send(OKAY, local_id, remote_id)
                marker_at = output.find(completion_marker)
                if marker_at >= 0:
                    # RM505Q's PCIe ADB transport commonly omits the final
                    # CLSE.  A unique marker emitted after the shell script
                    # proves the script reached its end, so close our ADB
                    # service explicitly rather than waiting forever.
                    self.send(CLSE, local_id, remote_id)
                    return bytes(output[:marker_at])
                continue
            if command == CLSE and arg1 == local_id:
                remote_id = arg0 if remote_id is None else remote_id
                self.send(CLSE, local_id, remote_id)
                return bytes(output)


def main():
    if len(sys.argv) < 4:
        raise SystemExit("Usage: mhi-adb-run.py HOST PORT COMMAND [COMMAND ...]")
    host, port = sys.argv[1], int(sys.argv[2])
    transport = AdbTransport(host, port)
    try:
        transport.handshake()
        # Quectel's shell service is reliable with a compact, one-line
        # command but can stall on the long multiline script used earlier.
        # A short random marker gives us an explicit completion signal while
        # keeping the OPEN request small.
        marker = ("__MHI_" + secrets.token_hex(4) + "__").encode()
        script = ";".join(sys.argv[3:])
        script += ";printf '" + marker.decode() + "\\n'"
        print("$ persistent shell script", file=sys.stderr)
        sys.stdout.buffer.write(transport.shell(script, marker))
        sys.stdout.buffer.flush()
    finally:
        transport.close()


if __name__ == "__main__":
    main()
