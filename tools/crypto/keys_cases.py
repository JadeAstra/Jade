# Cases for tests/crypto_keys_cases.ww: keys made by Python's cryptography package (OpenSSL
# underneath) and what it gives with them, for import Crypto to give the same: X25519 and P-256
# shared secrets, Ed25519 public keys and signatures, ECDSA signatures with RFC 6979's nonces
# (deterministic, so ww's must be the very same bytes) and with random ones (to be verified),
# RSA keys of 2048, 3072 and 4096 bits with PKCS#1 v1.5 signatures (deterministic: the same
# bytes) and PSS ones (random salt: verified). Run in WSL, where CPython 3.14 and cryptography 46
# are:
#
#   wsl -e python3 tools/crypto/keys_cases.py tests/data/crypto_keys_cases.json
import json, random, sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, ed25519, padding, rsa, x25519
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

r = random.Random(20261121)


def data(n):
    return bytes(r.getrandbits(8) for _ in range(n))


def raw(key):
    return key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def num(v, size=None):
    if size is None:
        size = (v.bit_length() + 7) // 8
    return v.to_bytes(size, "big").hex()


cases = {"x25519": [], "ed25519": [], "p256": [], "rsa": []}

for _ in range(20):
    a, b = x25519.X25519PrivateKey.from_private_bytes(data(32)), x25519.X25519PrivateKey.from_private_bytes(data(32))
    cases["x25519"].append({"private": a.private_bytes_raw().hex(), "peer": raw(b.public_key()).hex(), "shared": a.exchange(b.public_key()).hex()})

for _ in range(20):
    seed = data(32)
    key = ed25519.Ed25519PrivateKey.from_private_bytes(seed)
    msg = data(r.choice([0, 1, 31, 64, 200, 1000]))
    cases["ed25519"].append({"seed": seed.hex(), "public": raw(key.public_key()).hex(), "msg": msg.hex(), "sig": key.sign(msg).hex()})

curve = ec.SECP256R1()
order = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
for _ in range(20):
    d = r.randrange(1, order)
    key = ec.derive_private_key(d, curve)
    peer = ec.derive_private_key(r.randrange(1, order), curve)
    msg = data(r.choice([0, 3, 64, 500]))
    pub = key.public_key()
    case = {
        "private": num(d, 32),
        "public": pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint).hex(),
        "compressed": pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.CompressedPoint).hex(),
        "peer": peer.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint).hex(),
        "shared": key.exchange(ec.ECDH(), peer.public_key()).hex(),
        "msg": msg.hex(),
        "sha256": key.sign(msg, ec.ECDSA(hashes.SHA256(), deterministic_signing=True)).hex(),
        "sha512": key.sign(msg, ec.ECDSA(hashes.SHA512(), deterministic_signing=True)).hex(),
        "random": key.sign(msg, ec.ECDSA(hashes.SHA256())).hex(),
    }
    rr, ss = decode_dss_signature(bytes.fromhex(case["sha256"]))
    case["sha256Raw"] = num(rr, 32) + num(ss, 32)
    cases["p256"].append(case)

for bits in [2048, 3072, 4096]:
    key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
    n = key.private_numbers()
    msg = data(100)
    hashed = {"sha256": hashes.SHA256(), "sha384": hashes.SHA384(), "sha512": hashes.SHA512()}
    cases["rsa"].append({
        "n": num(n.public_numbers.n), "e": num(n.public_numbers.e), "d": num(n.d), "p": num(n.p), "q": num(n.q),
        "dp": num(n.dmp1), "dq": num(n.dmq1), "qinv": num(n.iqmp), "msg": msg.hex(),
        "pkcs1": {k: key.sign(msg, padding.PKCS1v15(), h).hex() for k, h in hashed.items()},
        "pss": {k: key.sign(msg, padding.PSS(mgf=padding.MGF1(h), salt_length=h.digest_size), h).hex() for k, h in hashed.items()},
    })

with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as f:
    json.dump(cases, f, separators=(",", ":"))
    f.write("\n")
print({k: len(v) for k, v in cases.items()})
