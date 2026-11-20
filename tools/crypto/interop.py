# The other way round from tests/crypto_keys.ww: keys and signatures of ww's own making checked by
# Python's cryptography package (OpenSSL). Runs `ww tools/crypto/interop_make.ww` (the ww given, or
# ~/ww-clang/ww) several times and checks everything it gives:
#
#   wsl -e python3 tools/crypto/interop.py [ww] [rounds]
#
# X25519 and P-256: the public keys from the private ones, and the shared secrets; Ed25519: the
# public key and the signature; ECDSA: DER and raw signatures over SHA-256 and SHA-384; RSA: the
# key's numbers (OpenSSL checks they make one key), PKCS#1 v1.5 and PSS signatures.
import json, os, subprocess, sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, ed25519, padding, rsa, x25519
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature

ww = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/ww-clang/ww")
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 5
here = os.path.dirname(os.path.abspath(__file__))
checked = 0


def raw(key):
    return key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()


def point(key, form=serialization.PublicFormat.UncompressedPoint):
    return key.public_bytes(serialization.Encoding.X962, form).hex()


def h(v):
    return bytes.fromhex(v)


def num(v):
    return int(v, 16)


for _ in range(rounds):
    made = json.loads(subprocess.run([ww, os.path.join(here, "interop_make.ww")], check=True, capture_output=True, text=True).stdout)
    msg = h(made["msg"])

    x = made["x25519"]
    a = x25519.X25519PrivateKey.from_private_bytes(h(x["private"]))
    assert raw(a.public_key()) == x["public"], "x25519 public"
    assert a.exchange(x25519.X25519PublicKey.from_public_bytes(h(x["peer"]))).hex() == x["shared"], "x25519 shared"

    e = made["ed25519"]
    ed = ed25519.Ed25519PrivateKey.from_private_bytes(h(e["seed"]))
    assert raw(ed.public_key()) == e["public"], "ed25519 public"
    ed25519.Ed25519PublicKey.from_public_bytes(h(e["public"])).verify(h(e["sig"]), msg)
    assert ed.sign(msg).hex() == e["sig"], "ed25519 deterministic"

    p = made["p256"]
    key = ec.derive_private_key(num(p["private"]), ec.SECP256R1())
    pub = key.public_key()
    assert point(pub) == p["public"] and point(pub, serialization.PublicFormat.CompressedPoint) == p["compressed"], "p256 public"
    peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), h(p["peer"]))
    assert key.exchange(ec.ECDH(), peer).hex() == p["shared"], "p256 shared"
    pub.verify(h(p["sha256"]), msg, ec.ECDSA(hashes.SHA256()))
    pub.verify(h(p["sha384"]), msg, ec.ECDSA(hashes.SHA384()))
    rs = h(p["raw"])
    pub.verify(encode_dss_signature(int.from_bytes(rs[:32], "big"), int.from_bytes(rs[32:], "big")), msg, ec.ECDSA(hashes.SHA256()))
    assert key.sign(msg, ec.ECDSA(hashes.SHA256(), deterministic_signing=True)).hex() == p["sha256"], "p256 rfc6979"

    for k in made["rsa"]:
        numbers = rsa.RSAPrivateNumbers(p=num(k["p"]), q=num(k["q"]), d=num(k["d"]), dmp1=num(k["dp"]), dmq1=num(k["dq"]), iqmp=num(k["qinv"]),
                                        public_numbers=rsa.RSAPublicNumbers(num(k["e"]), num(k["n"])))
        private = numbers.private_key()  # OpenSSL checks the numbers make one key
        public = private.public_key()
        public.verify(h(k["pkcs1sha256"]), msg, padding.PKCS1v15(), hashes.SHA256())
        public.verify(h(k["pkcs1sha512"]), msg, padding.PKCS1v15(), hashes.SHA512())
        public.verify(h(k["psssha256"]), msg, padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32), hashes.SHA256())
        public.verify(h(k["psssha384"]), msg, padding.PSS(mgf=padding.MGF1(hashes.SHA384()), salt_length=48), hashes.SHA384())
        assert private.sign(msg, padding.PKCS1v15(), hashes.SHA256()).hex() == k["pkcs1sha256"], "rsa pkcs1 deterministic"
    checked += 1

print(f"{checked} rounds of ww's keys and signatures, all as OpenSSL has them")
