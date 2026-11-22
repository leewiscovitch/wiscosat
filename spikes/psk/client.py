#!/usr/bin/env python3
"""The PSK spike's client (throwaway): one TLS-PSK connection or several to the spike on a unit.

    SPIKE_KEY_FILE=<file with 64 hex characters> client.py <address> --mode <mode> [--count N] [--wrong-key|--no-psk]

Modes: tls13 (TLS 1.3, PSK with (EC)DHE), and the TLS 1.2 suites by their OpenSSL names, e.g.
ECDHE-PSK-CHACHA20-POLY1305, ECDHE-PSK-AES128-CBC-SHA256, PSK-AES128-GCM-SHA256.
The key is read from a file, never an argument (it would show in ps). Prints one JSON line per connection:
the unit's answer and the client's own handshake time, or the error. Exit 0 when every connection gave the line
back, 1 otherwise. Standard library only; Python 3.13 or later (the PSK callbacks).
"""
import argparse
import json
import os
import socket
import ssl
import sys
import time


def context(mode, key, name, no_psk):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    if mode == "tls13":
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    else:
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        if not no_psk:
            ctx.set_ciphers(mode)
    if not no_psk:
        ctx.set_psk_client_callback(lambda hint: (name, key))
    return ctx


def one(address, port, ctx, text):
    t0 = time.monotonic()
    with socket.create_connection((address, port), timeout=5) as raw:
        t1 = time.monotonic()
        with ctx.wrap_socket(raw) as tls:
            t2 = time.monotonic()
            tls.sendall(text.encode() + b"\n")
            buf = b""
            while b"\n" not in buf:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                buf += chunk
            answer = json.loads(buf.decode().strip())
            answer["client_connect_ms"] = round((t1 - t0) * 1000, 1)
            answer["client_handshake_ms"] = round((t2 - t1) * 1000, 1)
            answer["client_cipher"] = tls.cipher()[0]
            answer["client_version"] = tls.version()
            return answer


def main():
    p = argparse.ArgumentParser()
    p.add_argument("address")
    p.add_argument("--port", type=int, default=5577)
    p.add_argument("--name", default="unit-1")
    p.add_argument("--mode", required=True)
    p.add_argument("--count", type=int, default=1)
    p.add_argument("--wrong-key", action="store_true")
    p.add_argument("--no-psk", action="store_true")
    a = p.parse_args()
    key = bytes.fromhex(open(os.environ["SPIKE_KEY_FILE"]).read().strip())
    if a.wrong_key:
        key = bytes(b ^ 0x01 for b in key)
    ok = True
    for i in range(a.count):
        text = f"hello-{i}"
        try:
            ans = one(a.address, a.port, context(a.mode, key, a.name, a.no_psk), text)
            good = ans.get("echo") == text
            ok &= good
            print(json.dumps(ans))
        except Exception as e:  # the spike reports every failure as a line, for the record
            ok = False
            print(json.dumps({"ok": False, "error": f"{type(e).__name__}: {e}"}))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
