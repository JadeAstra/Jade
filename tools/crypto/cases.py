# Cases for tests/crypto_cases.ww: what Python's hashlib and the `cryptography` package (OpenSSL
# underneath) give for inputs drawn at random, the same each run, for everything import Crypto
# does: every hash at many lengths and in pieces, HMAC, HKDF, PBKDF2, BLAKE2b, Argon2id, AES-GCM
# with nonces of any size, ChaCha20-Poly1305 over long messages. Run in WSL, where CPython 3.14
# and cryptography 46 are:
#
#   wsl -e python3 tools/crypto/cases.py tests/data/crypto_cases.json
#
# Beside it, crypto_cases_quick.json: every tenth case of each kind, the long messages left out,
# for the quick runs under GC stress (WW_TEST_QUICK=1), where reading the whole file alone takes
# minutes.
import hashlib, hmac, json, random, sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.argon2 import Argon2id
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

r = random.Random(20261120)


def data(n):
    return bytes(r.getrandbits(8) for _ in range(n))


def sizes(small, big):
    # Every length near a block's edges, then some at random.
    return list(range(0, small)) + [r.randrange(small, big) for _ in range(12)]


pool = data(6000)
cases = {"pool": pool.hex(), "hash": [], "hmac": [], "hkdf": [], "pbkdf2": [], "blake2b": [], "argon2id": [], "aead": []}

hash_names = ["sha224", "sha256", "sha384", "sha512", "sha3_224", "sha3_256", "sha3_384", "sha3_512"]
for name in hash_names:
    for n in sizes(260, 6000):
        m = pool[:n]
        # Pieces it is taken in, to cross the blocks in every way.
        cuts = sorted(r.randrange(0, n + 1) for _ in range(r.randrange(0, 4)))
        cases["hash"].append({"hash": name, "size": n, "cuts": cuts, "digest": hashlib.new(name, m).hexdigest()})
    cases["hash"].append({"hash": name, "size": 256, "times": 4096, "cuts": [], "digest": hashlib.new(name, pool[:256] * 4096).hexdigest()})
for name in ["shake_128", "shake_256"]:
    for n in sizes(200, 6000):
        m = pool[:n]
        out = r.choice([1, 16, 32, 64, 167, 168, 169, 500, 1000])
        cases["hash"].append({"hash": name, "size": n, "cuts": [], "out": out, "digest": hashlib.new(name, m).hexdigest(out)})

for name in ["sha224", "sha256", "sha384", "sha512", "sha3_224", "sha3_256", "sha3_384", "sha3_512"]:
    for key_size in [0, 1, 16, 31, 32, 63, 64, 65, 127, 128, 129, 200]:
        key, msg = data(key_size), data(r.randrange(0, 600))
        cases["hmac"].append({"hash": name, "key": key.hex(), "msg": msg.hex(), "mac": hmac.new(key, msg, name).hexdigest()})

kdf_hashes = {"sha256": hashes.SHA256, "sha384": hashes.SHA384, "sha512": hashes.SHA512, "sha3_256": hashes.SHA3_256}
for name, h in kdf_hashes.items():
    for _ in range(10):
        ikm, salt, info = data(r.randrange(1, 100)), data(r.randrange(0, 70)), data(r.randrange(0, 50))
        size = r.randrange(1, 255 * h.digest_size // 4)
        okm = HKDF(algorithm=h(), length=size, salt=salt or None, info=info).derive(ikm)
        cases["hkdf"].append({"hash": name, "ikm": ikm.hex(), "salt": salt.hex(), "info": info.hex(), "size": size, "okm": okm.hex()})
    for _ in range(6):
        password, salt = data(r.randrange(0, 80)), data(r.randrange(1, 40))
        iterations, size = r.choice([1, 2, 3, 100, 1000, 5000]), r.randrange(1, 3 * h.digest_size)
        dk = PBKDF2HMAC(algorithm=h(), length=size, salt=salt, iterations=iterations).derive(password)
        cases["pbkdf2"].append({"hash": name, "password": password.hex(), "salt": salt.hex(), "iterations": iterations, "size": size, "dk": dk.hex()})

for _ in range(40):
    m, size = data(r.randrange(0, 600)), r.randrange(1, 65)
    key = data(r.choice([0, 0, 1, 32, 64]))
    cases["blake2b"].append({"msg": m.hex(), "size": size, "key": key.hex(), "digest": hashlib.blake2b(m, digest_size=size, key=key).hexdigest()})

argon_shapes = [(1, 8, 1), (1, 16, 2), (2, 32, 4), (3, 32, 4), (1, 100, 3), (2, 256, 1), (4, 64, 2), (1, 1024, 8)]
for iterations, memory, lanes in argon_shapes:
    for size in [4, 32, 64, 65, 100]:
        password, salt = data(r.randrange(0, 40)), data(r.randrange(8, 33))
        secret = data(r.choice([0, 8, 32]))
        ad = data(r.choice([0, 12]))
        tag = Argon2id(salt=salt, length=size, iterations=iterations, lanes=lanes, memory_cost=memory, ad=ad or None, secret=secret or None).derive(password)
        cases["argon2id"].append({"password": password.hex(), "salt": salt.hex(), "secret": secret.hex(), "ad": ad.hex(), "iterations": iterations,
                                  "memory": memory, "lanes": lanes, "size": size, "tag": tag.hex()})

for cipher in ["aes-gcm", "chacha20-poly1305"]:
    for _ in range(150):
        if cipher == "aes-gcm":
            key = data(r.choice([16, 24, 32]))
            nonce = data(r.choice([12, 12, 12, 8, 16, 33, 64, 100]))
            sealed_by = AESGCM(key)
        else:
            key = data(32)
            nonce = data(12)
            sealed_by = ChaCha20Poly1305(key)
        msg = data(r.choice([0, 1, 15, 16, 17, 63, 64, 65, r.randrange(0, 3000)]))
        aad = data(r.choice([0, 1, 13, 16, 100]))
        sealed = sealed_by.encrypt(nonce, msg, aad)
        cases["aead"].append({"cipher": cipher, "key": key.hex(), "nonce": nonce.hex(), "aad": aad.hex(), "msg": msg.hex(), "sealed": sealed.hex()})
    # Long messages, past GCM's 32-bit counter's low bits and many ChaCha blocks.
    key = data(32)
    nonce = data(12)
    msg = data(70000)
    sealed = (AESGCM(key) if cipher == "aes-gcm" else ChaCha20Poly1305(key)).encrypt(nonce, msg, b"")
    cases["aead"].append({"cipher": cipher, "key": key.hex(), "nonce": nonce.hex(), "aad": "", "msg": msg.hex(), "sealed": sealed.hex()})

with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as f:
    json.dump(cases, f, separators=(",", ":"))
    f.write("\n")
# Beside it, every tenth case of each kind with the long messages left out: for the quick runs
# under GC stress (WW_TEST_QUICK=1), where reading the whole file alone takes minutes.
quick = {k: (v if k == "pool" else [c for i, c in enumerate(v) if i % 10 == 0 and len(c.get("msg", "")) < 20000]) for k, v in cases.items()}
with open(sys.argv[1].replace(".json", "_quick.json"), "w", encoding="utf-8", newline="\n") as f:
    json.dump(quick, f, separators=(",", ":"))
    f.write("\n")
print({k: len(v) for k, v in cases.items()})
