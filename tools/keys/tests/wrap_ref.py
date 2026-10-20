"""Test-only reference implementation of the Story 6.1 wrap formats (wrapped_key_v1, vault_entry_v1) in pure Python.
AES-256 (encrypt direction only, as GCM needs), GCM and HKDF-SHA256. Slow and not constant-time: it exists to pin test vectors, not to ship."""
import hashlib
import hmac
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import velora_keys as vk  # noqa: E402


# ---- AES-256 ----
def _build_sbox():
    p = q = 1
    sbox = [0] * 256
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ (q << 1 | q >> 7) & 0xFF ^ (q << 2 | q >> 6) & 0xFF ^ (q << 3 | q >> 5) & 0xFF ^ (q << 4 | q >> 4) & 0xFF
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


SBOX = _build_sbox()


def _xt(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _expand(key):
    w = [list(key[i:i + 4]) for i in range(0, 32, 4)]
    rcon = 1
    for i in range(8, 60):
        t = list(w[i - 1])
        if i % 8 == 0:
            t = [SBOX[t[1]] ^ rcon, SBOX[t[2]], SBOX[t[3]], SBOX[t[0]]]
            rcon = _xt(rcon)
        elif i % 8 == 4:
            t = [SBOX[b] for b in t]
        w.append([a ^ b for a, b in zip(w[i - 8], t)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(15)]


def aes256_encrypt_block(key, block):
    rk = _expand(key)
    s = [a ^ b for a, b in zip(block, rk[0])]
    for r in range(1, 15):
        s = [SBOX[b] for b in s]
        s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]  # ShiftRows
        if r != 14:
            n = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                n += [_xt(a[0]) ^ (_xt(a[1]) ^ a[1]) ^ a[2] ^ a[3], a[0] ^ _xt(a[1]) ^ (_xt(a[2]) ^ a[2]) ^ a[3],
                      a[0] ^ a[1] ^ _xt(a[2]) ^ (_xt(a[3]) ^ a[3]), (_xt(a[0]) ^ a[0]) ^ a[1] ^ a[2] ^ _xt(a[3])]
            s = n
        s = [a ^ b for a, b in zip(s, rk[r])]
    return bytes(s)


# ---- GCM (96-bit IV) ----
def _gmul(x, y):
    z, v = 0, y
    for i in range(127, -1, -1):
        if (x >> i) & 1:
            z ^= v
        v = (v >> 1) ^ (0xE1 << 120) if v & 1 else v >> 1
    return z


def _ghash(h, aad, ct):
    def blocks(b):
        for i in range(0, len(b), 16):
            yield int.from_bytes(b[i:i + 16].ljust(16, b"\0"), "big")
    y = 0
    for blk in list(blocks(aad)) + list(blocks(ct)) + [(len(aad) * 8) << 64 | len(ct) * 8]:
        y = _gmul(y ^ blk, h)
    return y


def _ctr(key, j0, data):
    out = bytearray()
    ctr = int.from_bytes(j0, "big")
    for i in range(0, len(data), 16):
        ctr = (ctr & ~0xFFFFFFFF) | ((ctr + 1) & 0xFFFFFFFF)
        ks = aes256_encrypt_block(key, ctr.to_bytes(16, "big"))
        out += bytes(a ^ b for a, b in zip(data[i:i + 16], ks))
    return bytes(out)


def gcm_encrypt(key, iv, plaintext, aad):
    assert len(key) == 32 and len(iv) == 12
    h = int.from_bytes(aes256_encrypt_block(key, bytes(16)), "big")
    j0 = iv + b"\0\0\0\1"
    ct = _ctr(key, j0, plaintext)
    tag = _ghash(h, aad, ct) ^ int.from_bytes(aes256_encrypt_block(key, j0), "big")
    return ct, tag.to_bytes(16, "big")


def gcm_decrypt(key, iv, ct, tag, aad):
    assert len(key) == 32 and len(iv) == 12
    h = int.from_bytes(aes256_encrypt_block(key, bytes(16)), "big")
    j0 = iv + b"\0\0\0\1"
    want = (_ghash(h, aad, ct) ^ int.from_bytes(aes256_encrypt_block(key, j0), "big")).to_bytes(16, "big")
    if not hmac.compare_digest(want, tag):
        raise ValueError("authentication failed")
    return _ctr(key, j0, ct)


# ---- HKDF-SHA256 (RFC 5869) ----
def hkdf_sha256(ikm, salt, info, length):
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, t = b"", b""
    for i in range(1, -(-length // 32) + 1):
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
    return out[:length]


# ---- wrapped_key_v1 ----
WK_MAGIC, WK_ALG = b"VWK1", 1


def wrap_key_v1(device_pub, eph_priv, nonce, library_id, content_key):
    """Returns (blob, parts) where parts holds the intermediates for the pinned vectors."""
    eph_pub = vk.x25519_public(eph_priv)
    shared = vk.x25519(eph_priv, device_pub)
    if shared == bytes(32):
        raise ValueError("all-zero shared secret")
    info = b"velora/wrapped_key_v1/" + library_id.encode()
    wk = hkdf_sha256(shared, eph_pub + device_pub, info, 32)
    header = WK_MAGIC + bytes([WK_ALG]) + vk.key_id(device_pub) + eph_pub + nonce  # 57 bytes
    aad = header[:45] + library_id.encode()
    ct, tag = gcm_encrypt(wk, nonce, content_key, aad)
    return header + ct + tag, {"shared": shared, "wrapping_key": wk, "aad": aad, "info": info}


def unwrap_key_v1(device_priv, blob, library_id):
    if len(blob) != 105 or blob[:4] != WK_MAGIC or blob[4] != WK_ALG:
        raise ValueError("bad header")
    device_pub = vk.x25519_public(device_priv)
    if blob[5:13] != vk.key_id(device_pub):
        raise ValueError("not for this device")
    eph_pub, nonce, ct, tag = blob[13:45], blob[45:57], blob[57:89], blob[89:105]
    shared = vk.x25519(device_priv, eph_pub)
    if shared == bytes(32):
        raise ValueError("all-zero shared secret")
    wk = hkdf_sha256(shared, eph_pub + device_pub, b"velora/wrapped_key_v1/" + library_id.encode(), 32)
    return gcm_decrypt(wk, nonce, ct, tag, blob[:45] + library_id.encode())


# ---- vault_entry_v1 ----
def vault_entry_v1(kek, nonce, library_id, content_key):
    header = b"VKV1" + vk.kek_id(kek) + nonce  # 24 bytes
    aad = header[:12] + b"velora/vault_entry_v1/" + library_id.encode()
    ct, tag = gcm_encrypt(kek, nonce, content_key, aad)
    return header + ct + tag, {"aad": aad}


def open_vault_entry_v1(kek, entry, library_id):
    if len(entry) != 72 or entry[:4] != b"VKV1" or entry[4:12] != vk.kek_id(kek):
        raise ValueError("bad entry")
    aad = entry[:12] + b"velora/vault_entry_v1/" + library_id.encode()
    return gcm_decrypt(kek, entry[12:24], entry[24:56], entry[56:72], aad)
