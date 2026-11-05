#!/usr/bin/env python3
"""Record real SSH key exchanges with OpenSSH's sshd and print tests/unit/ssh_handshake_vectors.h.

    python3 tests/unit/fixtures/ssh/record_handshakes.py > tests/unit/ssh_handshake_vectors.h

Needs sshd, ssh-keygen and the Python `cryptography` package; the tests that read the header need
none of them. The host keys are throwaway ones in a temporary directory, deleted afterwards. Each
connection is one `sshd -i` over a socketpair, so nothing listens on a port and the system's sshd
configuration and keys are not read.

This is a client written separately from apps/ssh, on OpenSSL rather than mbedTLS, so a mistake in
the app or in crypto.c cannot end up in the vectors. A vector is kept only if OpenSSL verifies the
host's signature over H, and a handshake vector only if sshd then answers an encrypted
SERVICE_REQUEST, which it does only if it derived the same keys.
"""
import hashlib
import hmac
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa, x25519
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

V_C = b"SSH-2.0-PicoDeckVectorRecorder_1.0"
CIPHER = "aes128-ctr"
MAC = "hmac-sha2-256"
# Handshake vectors: every key exchange with every host key algorithm the app offers.
COMBOS = [(kex, algo) for kex in ("curve25519-sha256", "ecdh-sha2-nistp256")
          for algo in ("ecdsa-sha2-nistp256", "rsa-sha2-256")]
# Give up looking for an uncommon signature shape after this many handshakes.
MAX_TRIES = 5000


def s(b):
    return struct.pack(">I", len(b)) + b


def mpint(b):
    b = b.lstrip(b"\0")
    if b and b[0] & 0x80:
        b = b"\0" + b
    return s(b)


def read_s(buf, pos):
    n = struct.unpack_from(">I", buf, pos)[0]
    return buf[pos + 4 : pos + 4 + n], pos + 4 + n


class Sshd:
    """One sshd -i with one host key; one connection."""

    def __init__(self, sshd_path, key, kex, algo):
        self.sock, child = socket.socketpair()
        self.sock.settimeout(20)
        self.proc = subprocess.Popen(
            [sshd_path, "-i", "-f", "/dev/null", "-h", key, "-o", "PidFile=none", "-o", "UsePAM=no",
             "-o", "KexAlgorithms=" + kex, "-o", "HostKeyAlgorithms=" + algo],
            stdin=child, stdout=child, stderr=subprocess.DEVNULL)
        child.close()
        self.rx = b""

    def _fill(self):
        d = self.sock.recv(65536)
        if not d:
            raise EOFError("sshd closed the connection")
        self.rx += d

    def need(self, n):
        while len(self.rx) < n:
            self._fill()
        out, self.rx = self.rx[:n], self.rx[n:]
        return out

    def line(self):
        while b"\r\n" not in self.rx:
            self._fill()
        out, _, self.rx = self.rx.partition(b"\r\n")
        return out

    def send_plain(self, payload):
        pad = 8 - (5 + len(payload)) % 8
        if pad < 4:
            pad += 8
        self.sock.sendall(struct.pack(">IB", 1 + len(payload) + pad, pad) + payload + os.urandom(pad))

    def recv_plain(self):
        length = struct.unpack(">I", self.need(4))[0]
        body = self.need(length)
        return body[1 : length - body[0]]

    def close(self):
        self.sock.close()
        self.proc.wait(timeout=20)


def inner_signature(sig_blob):
    _, pos = read_s(sig_blob, 0)
    return read_s(sig_blob, pos)[0]


def verify_with_openssl(k_s, H, sig_blob):
    """Raises InvalidSignature unless the host's signature over H is good."""
    keytype, pos = read_s(k_s, 0)
    sigalg, _ = read_s(sig_blob, 0)
    inner = inner_signature(sig_blob)
    if keytype == b"ssh-rsa":
        e, pos = read_s(k_s, pos)
        n, pos = read_s(k_s, pos)
        pub = rsa.RSAPublicNumbers(int.from_bytes(e, "big"), int.from_bytes(n, "big")).public_key()
        assert sigalg == b"rsa-sha2-256", sigalg
        pub.verify(inner, H, padding.PKCS1v15(), hashes.SHA256())
    else:
        assert keytype == b"ecdsa-sha2-nistp256" and sigalg == keytype, (keytype, sigalg)
        _, pos = read_s(k_s, pos)
        q, pos = read_s(k_s, pos)
        pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), q)
        r, rpos = read_s(inner, 0)
        ss, _ = read_s(inner, rpos)
        der = encode_dss_signature(int.from_bytes(r, "big"), int.from_bytes(ss, "big"))
        pub.verify(der, H, ec.ECDSA(hashes.SHA256()))


def derive(K, H, sid, letter, n):
    out = hashlib.sha256(K + H + letter + sid).digest()
    while len(out) < n:
        out += hashlib.sha256(K + H + out).digest()
    return out[:n]


def key_exchange(sshd, kex, algo):
    """Run a key exchange up to the verified KEX_ECDH_REPLY; return its values."""
    sshd.sock.sendall(V_C + b"\r\n")
    while True:
        V_S = sshd.line()
        if V_S.startswith(b"SSH-"):
            break

    I_C = (bytes([20]) + os.urandom(16) + s(kex.encode()) + s(algo.encode())
           + s(CIPHER.encode()) * 2 + s(MAC.encode()) * 2 + s(b"none") * 2 + s(b"") * 2
           + b"\0" + struct.pack(">I", 0))
    sshd.send_plain(I_C)
    I_S = sshd.recv_plain()
    assert I_S[0] == 20, I_S[0]

    if kex == "curve25519-sha256":
        priv = x25519.X25519PrivateKey.generate()
        e = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    else:
        priv = ec.generate_private_key(ec.SECP256R1())
        e = priv.public_key().public_bytes(serialization.Encoding.X962,
                                           serialization.PublicFormat.UncompressedPoint)
    sshd.send_plain(bytes([30]) + s(e))

    reply = sshd.recv_plain()
    assert reply[0] == 31, reply[0]
    K_S, pos = read_s(reply, 1)
    f, pos = read_s(reply, pos)
    sig_blob, pos = read_s(reply, pos)

    if kex == "curve25519-sha256":
        shared = priv.exchange(x25519.X25519PublicKey.from_public_bytes(f))
    else:
        shared = priv.exchange(ec.ECDH(), ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), f))
    K = mpint(shared)
    H = hashlib.sha256(s(V_C) + s(V_S) + s(I_C) + s(I_S) + s(K_S) + s(e) + s(f) + K).digest()
    verify_with_openssl(K_S, H, sig_blob)
    return dict(V_C=V_C, V_S=V_S, I_C=I_C, I_S=I_S, K_S=K_S, e=e, f=f, shared=shared, K=K, H=H,
                sig_blob=sig_blob)


def record_handshake(sshd_path, key, kex, algo):
    sshd = Sshd(sshd_path, key, kex, algo)
    v = key_exchange(sshd, kex, algo)
    K, H = v["K"], v["H"]

    sshd.send_plain(bytes([21]))
    assert sshd.recv_plain() == bytes([21])

    # H is also the session id on the first key exchange.
    v.update(iv_c2s=derive(K, H, H, b"A", 16), iv_s2c=derive(K, H, H, b"B", 16),
             key_c2s=derive(K, H, H, b"C", 16), key_s2c=derive(K, H, H, b"D", 16),
             mac_c2s=derive(K, H, H, b"E", 32), mac_s2c=derive(K, H, H, b"F", 32))
    enc = Cipher(algorithms.AES(v["key_c2s"]), modes.CTR(v["iv_c2s"])).encryptor()
    dec = Cipher(algorithms.AES(v["key_s2c"]), modes.CTR(v["iv_s2c"])).decryptor()

    # Sequence numbers count from the start of the connection and each side has sent three
    # packets: KEXINIT, ECDH_INIT or ECDH_REPLY, NEWKEYS.
    payload = bytes([5]) + s(b"ssh-userauth")
    pad = 16 - (5 + len(payload)) % 16
    if pad < 4:
        pad += 16
    packet = struct.pack(">IB", 1 + len(payload) + pad, pad) + payload + os.urandom(pad)
    mac = hmac.new(v["mac_c2s"], struct.pack(">I", 3) + packet, hashlib.sha256).digest()
    sshd.sock.sendall(enc.update(packet) + mac)

    first = sshd.need(16)
    clear = dec.update(first)
    length = struct.unpack(">I", clear[:4])[0]
    rest = sshd.need(length + 4 - 16)
    clear += dec.update(rest)
    wire_mac = sshd.need(32)
    assert wire_mac == hmac.new(v["mac_s2c"], struct.pack(">I", 3) + clear, hashlib.sha256).digest()
    v["accept_wire"] = first + rest + wire_mac
    v["accept_payload"] = clear[5 : 4 + length - clear[4]]
    assert v["accept_payload"][0] == 6, v["accept_payload"][0]  # SERVICE_ACCEPT
    sshd.close()
    v.update(name=kex + " + " + algo, kex=kex, hostkey_algo=algo)
    return v


def find_signature(sshd_path, key, algo, wanted):
    """Handshake until the signature blob satisfies wanted(inner signature)."""
    for _ in range(MAX_TRIES):
        sshd = Sshd(sshd_path, key, "curve25519-sha256", algo)
        v = key_exchange(sshd, "curve25519-sha256", algo)
        sshd.close()
        if wanted(inner_signature(v["sig_blob"])):
            return v
    sys.exit("no signature of the wanted shape in %d handshakes" % MAX_TRIES)


def ecdsa_mpint_len(inner):
    r, pos = read_s(inner, 0)
    return len(r), len(read_s(inner, pos)[0])


def c_hex(indent, b):
    text = b.hex()
    lines = [text[i : i + 96] for i in range(0, len(text), 96)] or [""]
    return "\n".join(indent + '"' + line + '"' for line in lines)


def main():
    sshd_path = shutil.which("sshd") or "/usr/sbin/sshd"
    keygen = shutil.which("ssh-keygen") or "ssh-keygen"
    with tempfile.TemporaryDirectory(prefix="picodeck-ssh-vectors-") as tmp:
        keys = {}
        for algo, args in (("ecdsa-sha2-nistp256", ["-t", "ecdsa", "-b", "256"]),
                           ("rsa-sha2-256", ["-t", "rsa", "-b", "3072"])):
            keys[algo] = os.path.join(tmp, algo)
            subprocess.run([keygen, "-q", "-N", "", "-f", keys[algo], *args], check=True)
        handshakes = [record_handshake(sshd_path, keys[algo], kex, algo) for kex, algo in COMBOS]

        # The encodings a server produces only sometimes.
        shapes = [
            ("ecdsa, r with a sign byte (33 bytes), s without (32)", "ecdsa-sha2-nistp256",
             lambda i: ecdsa_mpint_len(i) == (33, 32)),
            ("ecdsa, r without a sign byte (32 bytes), s with (33)", "ecdsa-sha2-nistp256",
             lambda i: ecdsa_mpint_len(i) == (32, 33)),
            ("ecdsa, r short: its top byte was zero", "ecdsa-sha2-nistp256",
             lambda i: ecdsa_mpint_len(i)[0] < 32),
            ("ecdsa, s short: its top byte was zero", "ecdsa-sha2-nistp256",
             lambda i: ecdsa_mpint_len(i)[1] < 32),
            ("rsa, signature starts with a zero byte", "rsa-sha2-256", lambda i: i[0] == 0),
        ]
        signatures = [(name, algo, find_signature(sshd_path, keys[algo], algo, wanted))
                      for name, algo, wanted in shapes]

    version = subprocess.run([sshd_path, "-V"], capture_output=True, text=True).stderr.strip()
    out = ["// Generated by tests/unit/fixtures/ssh/record_handshakes.py; do not edit.",
           "// Real key exchanges with " + re.sub(r"\s+", " ", version) + ", hex-encoded. See that",
           "// script for how each vector is checked before it is kept.",
           "#pragma once", "",
           "typedef struct {",
           "    const char *name, *kex, *hostkey_algo;",
           "    // Exchange hash inputs (RFC 4253 section 8), the shared secret and the host's signature.",
           "    const char *V_C, *V_S, *I_C, *I_S, *K_S, *e, *f, *shared, *K, *H, *sig_blob;",
           "    // aes128-ctr + hmac-sha2-256 keys, then the server's encrypted SERVICE_ACCEPT, the",
           "    // fourth packet it sent (sequence number 3), and that packet's payload.",
           "    const char *iv_c2s, *iv_s2c, *key_c2s, *key_s2c, *mac_c2s, *mac_s2c;",
           "    const char *accept_wire, *accept_payload;",
           "} ssh_handshake_t;", "",
           "// The signature blob's two parts: string(algorithm name) + string(signature).",
           "typedef struct { const char *name, *hostkey_algo, *K_S, *H, *sig_blob; } ssh_signature_t;",
           ""]
    out.append("static const ssh_handshake_t __attribute__((unused)) SSH_HANDSHAKES[] = {")
    for v in handshakes:
        out.append('  { "%s", "%s", "%s",' % (v["name"], v["kex"], v["hostkey_algo"]))
        for field in ("V_C", "V_S", "I_C", "I_S", "K_S", "e", "f", "shared", "K", "H", "sig_blob",
                      "iv_c2s", "iv_s2c", "key_c2s", "key_s2c", "mac_c2s", "mac_s2c",
                      "accept_wire", "accept_payload"):
            out.append(c_hex("    ", v[field]) + ",")
        out.append("  },")
    out += ["};", ""]
    out.append("static const ssh_signature_t __attribute__((unused)) SSH_SIGNATURES[] = {")
    for name, algo, v in signatures:
        out.append('  { "%s", "%s",' % (name, algo))
        for field in ("K_S", "H", "sig_blob"):
            out.append(c_hex("    ", v[field]) + ",")
        out.append("  },")
    out.append("};")
    print("\n".join(out))


if __name__ == "__main__":
    try:
        main()
    except (EOFError, InvalidSignature) as err:
        sys.exit("recording failed: %r" % err)
