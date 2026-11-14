#!/usr/bin/env python3
"""Expose one modem-local TCP port through the RM505Q PCIe ADB channel.

Example (run twice, in two terminals):
  mhi-adb-tcp-forward.py 127.0.0.1 6560 8080 80
  mhi-adb-tcp-forward.py 127.0.0.1 6560 8443 443

Both listeners bind only to 127.0.0.1.  This is a tunnel, not a modem/router
configuration change.  It uses the same legacy ADB greeting proven reliable
by mhi-adb-run.py and opens the standard device-side `tcp:<port>` service.
"""

import argparse
import select
import socket
import struct
import sys

HEADER = struct.Struct("<6I")
CNXN = int.from_bytes(b"CNXN", "little")
OPEN = int.from_bytes(b"OPEN", "little")
OKAY = int.from_bytes(b"OKAY", "little")
CLSE = int.from_bytes(b"CLSE", "little")
WRTE = int.from_bytes(b"WRTE", "little")


class AdbError(RuntimeError):
    pass


def name(command):
    return command.to_bytes(4, "little").decode("ascii", "replace")


def checksum(payload):
    return sum(payload) & 0xFFFFFFFF


def packet(command, arg0=0, arg1=0, payload=b""):
    return HEADER.pack(command, arg0, arg1, len(payload), checksum(payload),
                       command ^ 0xFFFFFFFF) + payload


class Adb:
    def __init__(self, host, port, timeout=20):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

    def close(self):
        self.sock.close()

    def send(self, command, arg0=0, arg1=0, payload=b""):
        self.sock.sendall(packet(command, arg0, arg1, payload))

    def exact(self, length):
        result = bytearray()
        while len(result) < length:
            data = self.sock.recv(length - len(result))
            if not data:
                raise AdbError("ADB transport closed")
            result.extend(data)
        return bytes(result)

    def receive(self):
        raw = self.exact(HEADER.size)
        command, arg0, arg1, length, received_sum, magic = HEADER.unpack(raw)
        if magic != (command ^ 0xFFFFFFFF):
            raise AdbError("invalid ADB header")
        payload = self.exact(length) if length else b""
        if checksum(payload) != received_sum:
            raise AdbError("invalid ADB checksum")
        return command, arg0, arg1, payload

    def handshake(self):
        self.send(CNXN, 0x01000000, 0x1000, b"")
        while True:
            command, _arg0, _arg1, _payload = self.receive()
            if command == CNXN:
                return

    def open_tcp(self, local_id, remote_port):
        self.send(OPEN, local_id, 0, ("tcp:%d" % remote_port).encode() + b"\0")
        while True:
            command, arg0, arg1, payload = self.receive()
            if command == CNXN:
                continue
            if command == OKAY and arg1 == local_id:
                return arg0
            if command == WRTE and arg1 == local_id:
                self.send(OKAY, local_id, arg0)
                continue
            if command == CLSE and arg1 == local_id:
                self.send(CLSE, local_id, arg0)
                raise AdbError("device refused tcp:%d" % remote_port)
            raise AdbError("unexpected opening tcp service: " + name(command))


def serve_connection(client, adb, remote_port):
    local_id = 1
    remote_id = adb.open_tcp(local_id, remote_port)
    print("Connected local client to modem tcp:%d" % remote_port, file=sys.stderr)
    client.setblocking(False)
    adb.sock.setblocking(False)
    adb_buffer = bytearray()
    waiting_for_ack = False
    client_buffer = bytearray()

    def drain_frames():
        nonlocal waiting_for_ack
        while len(adb_buffer) >= HEADER.size:
            command, arg0, arg1, length, received_sum, magic = HEADER.unpack(adb_buffer[:HEADER.size])
            if magic != (command ^ 0xFFFFFFFF):
                raise AdbError("invalid ADB header while forwarding")
            total = HEADER.size + length
            if len(adb_buffer) < total:
                return
            payload = bytes(adb_buffer[HEADER.size:total])
            del adb_buffer[:total]
            if checksum(payload) != received_sum:
                raise AdbError("invalid ADB checksum while forwarding")
            if command == CNXN:
                continue
            if command == OKAY and arg0 == remote_id and arg1 == local_id:
                waiting_for_ack = False
                continue
            if command == WRTE and arg1 == local_id:
                adb.send(OKAY, local_id, arg0)
                if payload:
                    client.sendall(payload)
                continue
            if command == CLSE and arg1 == local_id:
                adb.send(CLSE, local_id, arg0)
                raise EOFError
            raise AdbError("unexpected ADB packet: " + name(command))

    try:
        while True:
            readers = [adb.sock]
            if not waiting_for_ack:
                readers.append(client)
            ready, _, _ = select.select(readers, [], [], 30)
            if not ready:
                continue
            if adb.sock in ready:
                data = adb.sock.recv(16384)
                if not data:
                    raise EOFError
                adb_buffer.extend(data)
                drain_frames()
            if client in ready:
                data = client.recv(2048)
                if not data:
                    return
                client_buffer.extend(data)
            if client_buffer and not waiting_for_ack:
                payload = bytes(client_buffer[:2048])
                del client_buffer[:2048]
                adb.send(WRTE, local_id, remote_id, payload)
                waiting_for_ack = True
    finally:
        try:
            adb.send(CLSE, local_id, remote_id)
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("adb_port", type=int)
    parser.add_argument("local_port", type=int)
    parser.add_argument("modem_port", type=int)
    args = parser.parse_args()

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.local_port))
    listener.listen(4)
    print("Listening on 127.0.0.1:%d -> modem tcp:%d" %
          (args.local_port, args.modem_port), file=sys.stderr)
    try:
        while True:
            client, _address = listener.accept()
            adb = None
            try:
                adb = Adb(args.host, args.adb_port)
                adb.handshake()
                serve_connection(client, adb, args.modem_port)
            except (AdbError, EOFError, OSError, socket.timeout) as exc:
                print("Tunnel connection closed: %s" % exc, file=sys.stderr)
            finally:
                client.close()
                if adb is not None:
                    adb.close()
    finally:
        listener.close()


if __name__ == "__main__":
    main()
