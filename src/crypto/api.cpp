// What `import Crypto` (lib/Crypto) is built on: the built-in __crypto, over the primitives of
// src/crypto. A hash, a MAC or a cipher's key is a resource the program holds; collected, its
// state is wiped. The work that takes long (PBKDF2, Argon2id) runs through VM::blocking on copies
// of its inputs, so the program's other threads go on meanwhile.

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../builtins.h"
#include "../vm.h"
#include "crypto.h"

namespace ww {

namespace {

using namespace crypto;

struct HashBox {
    std::unique_ptr<Hash> hash;
};
struct MacBox {
    std::unique_ptr<Hmac> mac;
};
struct AeadBox {
    std::unique_ptr<Aead> aead;
};

void releaseHash(int64_t id) { delete reinterpret_cast<HashBox*>(id); }
void releaseMac(int64_t id) { delete reinterpret_cast<MacBox*>(id); }
void releaseAead(int64_t id) { delete reinterpret_cast<AeadBox*>(id); }

template <typename Box>
Box& boxOf(VM& vm, const Value& v, const char* kind) {
    if (!v.isObjType(ObjType::Resource) || std::strcmp(static_cast<ObjResource*>(v.as.obj)->kind, kind) != 0)
        throw vm.runtimeError(Thrown::IllegalArgumentException, std::string("not a ") + kind);
    return *reinterpret_cast<Box*>(static_cast<ObjResource*>(v.as.obj)->id);
}

ObjBytes* bytesOf(VM& vm, const Value& v, const char* what) {
    if (!v.isObjType(ObjType::Bytes)) throw vm.runtimeError(Thrown::ClassCastException, std::string(what) + " is Bytes, not " + vm.typeName(v));
    return asBytes(v);
}

// A run of bytes: `count` of them from `offset`, checked to fit.
struct Span {
    uint8_t* data;
    size_t size;
};
Span spanOf(VM& vm, const Value& v, const Value& offsetValue, const Value& countValue, const char* what) {
    ObjBytes* b = bytesOf(vm, v, what);
    const int64_t size = static_cast<int64_t>(b->data.size());
    const int64_t offset = vm.toInt(offsetValue, "offset");
    const int64_t count = vm.toInt(countValue, "count");
    if (offset < 0 || count < 0 || offset > size || count > size - offset)
        throw vm.runtimeError(Thrown::IndexOutOfBoundsException,
                              std::to_string(count) + " bytes from " + std::to_string(offset) + " do not fit in bytes of size " + std::to_string(size));
    return Span{b->data.data() + offset, static_cast<size_t>(count)};
}
// The whole of the bytes, or nothing for null.
Span wholeOf(VM& vm, const Value& v, const char* what) {
    if (v.isNull()) return Span{nullptr, 0};
    ObjBytes* b = bytesOf(vm, v, what);
    return Span{b->data.data(), b->data.size()};
}

Value newBytes(VM& vm, const uint8_t* data, size_t size) {
    ObjBytes* made = vm.alloc<ObjBytes>();
    made->data.assign(data, data + size);
    vm.account(made->data.size());
    return Value::object(made);
}

HashKind kindOf(VM& vm, const Value& v) {
    const int64_t k = vm.toInt(v, "hash");
    if (k < 1 || k > 11) throw vm.runtimeError(Thrown::IllegalArgumentException, "no hash numbered " + std::to_string(k));
    return static_cast<HashKind>(k);
}

std::unique_ptr<Hash> hashFor(VM& vm, const Value& kindValue, const Value& sizeValue) {
    const HashKind kind = kindOf(vm, kindValue);
    const int64_t size = sizeValue.isNull() ? 0 : vm.toInt(sizeValue, "size");
    if ((kind == HashKind::Shake128 || kind == HashKind::Shake256) && size < 1)
        throw vm.runtimeError(Thrown::IllegalArgumentException, "a SHAKE's size is 1 byte or more, not " + std::to_string(size));
    return newHash(kind, static_cast<size_t>(size));
}

// Bytes of exactly `size`: a key or point of a fixed size.
Span exactly(VM& vm, const Value& v, size_t size, const char* what) {
    const Span s = wholeOf(vm, v, what);
    if (s.data == nullptr || s.size != size)
        throw vm.runtimeError(Thrown::IllegalArgumentException, std::string(what) + " is " + std::to_string(size) + " bytes, not " + std::to_string(s.size));
    return s;
}

// Where `count` bytes go: `into` from `at`, checked to fit, and not frozen.
uint8_t* destination(VM& vm, const Value& into, const Value& atValue, size_t count) {
    ObjBytes* b = bytesOf(vm, into, "where it goes");
    vm.guardWrite(b);
    const int64_t at = vm.toInt(atValue, "at");
    const int64_t size = static_cast<int64_t>(b->data.size());
    if (at < 0 || at > size || static_cast<int64_t>(count) > size - at)
        throw vm.runtimeError(Thrown::IndexOutOfBoundsException,
                              std::to_string(count) + " bytes at " + std::to_string(at) + " do not fit in bytes of size " + std::to_string(size));
    return b->data.data() + at;
}

}  // namespace

void registerCrypto(VM& vm) {
    ObjClass* holder = vm.alloc<ObjClass>(vm.intern("crypto"));
    auto def = [&](const char* name, int minArgs, int maxArgs, NativeFn fn) {
        holder->statics.set(vm.intern(name), Value::object(vm.alloc<ObjNative>(std::string("crypto.") + name, minArgs, maxArgs, fn)));
    };

    // ---- Hashes ----

    // hashNew(kind, size): a hash in its starting state; `size` is a SHAKE's digest size.
    def("hashNew", 2, 2, [](VM& vm, int, Value* args) -> Value {
        auto* box = new HashBox{hashFor(vm, args[0], args[1])};
        return Value::object(vm.alloc<ObjResource>("hash", reinterpret_cast<int64_t>(box), &releaseHash));
    });
    // hashUpdate(hash, bytes, offset, count)
    def("hashUpdate", 4, 4, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        const Span s = spanOf(vm, args[1], args[2], args[3], "what is hashed");
        box.hash->update(s.data, s.size);
        return Value::null();
    });
    // hashFinish(hash): the digest as new bytes; the hash starts again.
    def("hashFinish", 1, 1, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        std::vector<uint8_t> out(box.hash->size());
        box.hash->finish(out.data());
        return newBytes(vm, out.data(), out.size());
    });
    // hashFinishInto(hash, into, at): the digest written into bytes.
    def("hashFinishInto", 3, 3, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        uint8_t* to = destination(vm, args[1], args[2], box.hash->size());
        box.hash->finish(to);
        return Value::null();
    });
    // hashSqueeze(hash, count): a SHAKE's next `count` bytes of output.
    def("hashSqueeze", 2, 2, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        const int64_t count = vm.toInt(args[1], "count");
        if (count < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a count is 0 or more, not " + std::to_string(count));
        std::vector<uint8_t> out(static_cast<size_t>(count));
        if (!box.hash->squeeze(out.data(), out.size())) throw vm.runtimeError(Thrown::UnsupportedOperationException, "only a SHAKE gives more output");
        return newBytes(vm, out.data(), out.size());
    });
    def("hashCopy", 1, 1, [](VM& vm, int, Value* args) -> Value {
        HashBox& box = boxOf<HashBox>(vm, args[0], "hash");
        auto* copy = new HashBox{box.hash->clone()};
        return Value::object(vm.alloc<ObjResource>("hash", reinterpret_cast<int64_t>(copy), &releaseHash));
    });
    def("hashReset", 1, 1, [](VM& vm, int, Value* args) -> Value {
        boxOf<HashBox>(vm, args[0], "hash").hash->reset();
        return Value::null();
    });
    // hashSize(kind, size), hashBlockSize(kind): a kind's sizes, with no hash made.
    def("hashSize", 2, 2, [](VM& vm, int, Value* args) -> Value { return Value::integer(static_cast<int64_t>(hashFor(vm, args[0], args[1])->size())); });
    def("hashBlockSize", 1, 1, [](VM& vm, int, Value* args) -> Value {
        return Value::integer(static_cast<int64_t>(hashFor(vm, args[0], Value::integer(32))->blockSize()));
    });
    // digest(kind, size, bytes, offset, count): the digest of the bytes, at once.
    def("digest", 5, 5, [](VM& vm, int, Value* args) -> Value {
        std::unique_ptr<Hash> h = hashFor(vm, args[0], args[1]);
        const Span s = spanOf(vm, args[2], args[3], args[4], "what is hashed");
        h->update(s.data, s.size);
        std::vector<uint8_t> out(h->size());
        h->finish(out.data());
        return newBytes(vm, out.data(), out.size());
    });

    // ---- HMAC ----

    // macNew(kind, key): HMAC with the key over a hash of the kind (not a SHAKE).
    def("macNew", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const HashKind kind = kindOf(vm, args[0]);
        if (kind == HashKind::Shake128 || kind == HashKind::Shake256) throw vm.runtimeError(Thrown::IllegalArgumentException, "HMAC is not made over a SHAKE");
        const Span key = wholeOf(vm, args[1], "a key");
        std::unique_ptr<Hash> h = newHash(kind);
        auto* box = new MacBox{std::make_unique<Hmac>(*h, key.data, key.size)};
        return Value::object(vm.alloc<ObjResource>("mac", reinterpret_cast<int64_t>(box), &releaseMac));
    });
    def("macUpdate", 4, 4, [](VM& vm, int, Value* args) -> Value {
        MacBox& box = boxOf<MacBox>(vm, args[0], "mac");
        const Span s = spanOf(vm, args[1], args[2], args[3], "what is authenticated");
        box.mac->update(s.data, s.size);
        return Value::null();
    });
    def("macFinish", 1, 1, [](VM& vm, int, Value* args) -> Value {
        MacBox& box = boxOf<MacBox>(vm, args[0], "mac");
        std::vector<uint8_t> out(box.mac->size());
        box.mac->finish(out.data());
        return newBytes(vm, out.data(), out.size());
    });
    def("macCopy", 1, 1, [](VM& vm, int, Value* args) -> Value {
        MacBox& box = boxOf<MacBox>(vm, args[0], "mac");
        auto* copy = new MacBox{box.mac->clone()};
        return Value::object(vm.alloc<ObjResource>("mac", reinterpret_cast<int64_t>(copy), &releaseMac));
    });
    def("macReset", 1, 1, [](VM& vm, int, Value* args) -> Value {
        boxOf<MacBox>(vm, args[0], "mac").mac->reset();
        return Value::null();
    });

    // ---- Key derivation ----

    // pbkdf2(kind, password, salt, iterations, size)
    def("pbkdf2", 5, 5, [](VM& vm, int, Value* args) -> Value {
        const HashKind kind = kindOf(vm, args[0]);
        if (kind == HashKind::Shake128 || kind == HashKind::Shake256) throw vm.runtimeError(Thrown::IllegalArgumentException, "PBKDF2 is not made over a SHAKE");
        const Span password = wholeOf(vm, args[1], "a password");
        const Span salt = wholeOf(vm, args[2], "a salt");
        const int64_t iterations = vm.toInt(args[3], "iterations");
        const int64_t size = vm.toInt(args[4], "size");
        if (iterations < 1) throw vm.runtimeError(Thrown::IllegalArgumentException, "PBKDF2's iterations are 1 or more, not " + std::to_string(iterations));
        if (size < 1 || size > (int64_t(1) << 32)) throw vm.runtimeError(Thrown::IllegalArgumentException, "PBKDF2 gives 1 byte or more, not " + std::to_string(size));
        struct Work {
            std::vector<uint8_t> password, salt, out;
            ~Work() { wipe(password.data(), password.size()), wipe(out.data(), out.size()); }
        };
        auto work = std::make_shared<Work>();
        work->password.assign(password.data, password.data + password.size);
        work->salt.assign(salt.data, salt.data + salt.size);
        work->out.resize(static_cast<size_t>(size));
        return vm.blocking(
            [work, kind, iterations] {
                std::unique_ptr<Hash> h = newHash(kind);
                pbkdf2(*h, work->password.data(), work->password.size(), work->salt.data(), work->salt.size(), static_cast<uint64_t>(iterations), work->out.data(),
                       work->out.size());
            },
            [work](VM& vm) -> Value { return newBytes(vm, work->out.data(), work->out.size()); });
    });

    // argon2id(password, salt, secret, data, iterations, memoryKiB, lanes, size)
    def("argon2id", 8, 8, [](VM& vm, int, Value* args) -> Value {
        struct Work {
            std::vector<uint8_t> password, salt, secret, data, out;
            uint32_t iterations = 0, memory = 0, lanes = 0;
            std::string why;
            bool done = false;
            ~Work() { wipe(password.data(), password.size()), wipe(secret.data(), secret.size()), wipe(out.data(), out.size()); }
        };
        auto work = std::make_shared<Work>();
        const Span password = wholeOf(vm, args[0], "a password"), salt = wholeOf(vm, args[1], "a salt"), secret = wholeOf(vm, args[2], "a secret"),
                   data = wholeOf(vm, args[3], "associated data");
        work->password.assign(password.data, password.data + password.size);
        work->salt.assign(salt.data, salt.data + salt.size);
        work->secret.assign(secret.data, secret.data + secret.size);
        work->data.assign(data.data, data.data + data.size);
        const int64_t iterations = vm.toInt(args[4], "iterations"), memory = vm.toInt(args[5], "memory"), lanes = vm.toInt(args[6], "parallelism"),
                      size = vm.toInt(args[7], "size");
        auto inRange = [&](int64_t v, int64_t low, int64_t high, const char* what) {
            if (v < low || v > high)
                throw vm.runtimeError(Thrown::IllegalArgumentException,
                                      std::string("Argon2's ") + what + " is from " + std::to_string(low) + " to " + std::to_string(high) + ", not " + std::to_string(v));
        };
        inRange(iterations, 1, 0xFFFFFFFFLL, "iterations");
        inRange(lanes, 1, 0xFFFFFF, "parallelism");
        inRange(memory, 8 * lanes, 0xFFFFFFFFLL, "memory in KiB");
        inRange(size, 4, 1 << 20, "size");
        work->iterations = static_cast<uint32_t>(iterations);
        work->memory = static_cast<uint32_t>(memory);
        work->lanes = static_cast<uint32_t>(lanes);
        work->out.resize(static_cast<size_t>(size));
        return vm.blocking(
            [work] {
                work->done = argon2id(work->password.data(), work->password.size(), work->salt.data(), work->salt.size(), work->secret.data(), work->secret.size(),
                                      work->data.data(), work->data.size(), work->iterations, work->memory, work->lanes, work->out.data(), work->out.size(), work->why);
            },
            [work](VM& vm) -> Value {
                if (!work->done) throw vm.runtimeError(Thrown::IllegalArgumentException, work->why);
                return newBytes(vm, work->out.data(), work->out.size());
            });
    });

    // blake2b(bytes, size, key): BLAKE2b's digest of 1 to 64 bytes, keyed by up to 64 bytes or not.
    def("blake2b", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const Span in = wholeOf(vm, args[0], "what is hashed");
        const int64_t size = vm.toInt(args[1], "size");
        const Span key = wholeOf(vm, args[2], "a key");
        if (size < 1 || size > 64) throw vm.runtimeError(Thrown::IllegalArgumentException, "a BLAKE2b digest is 1 to 64 bytes, not " + std::to_string(size));
        if (key.size > 64) throw vm.runtimeError(Thrown::IllegalArgumentException, "a BLAKE2b key is at most 64 bytes, not " + std::to_string(key.size));
        uint8_t out[64];
        blake2b(out, static_cast<size_t>(size), in.data, in.size, key.data, key.size);
        return newBytes(vm, out, static_cast<size_t>(size));
    });

    // ---- Authenticated ciphers ----

    // aeadNew(kind, key): 1 AES-GCM (a key of 16, 24 or 32 bytes), 2 ChaCha20-Poly1305 (32).
    def("aeadNew", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const int64_t kind = vm.toInt(args[0], "cipher");
        const Span key = wholeOf(vm, args[1], "a key");
        std::unique_ptr<Aead> made = kind == 1 ? newAesGcm(key.data, key.size) : kind == 2 ? newChaCha20Poly1305(key.data, key.size) : nullptr;
        if (made == nullptr)
            throw vm.runtimeError(Thrown::IllegalArgumentException, kind == 1 ? "an AES key is 16, 24 or 32 bytes, not " + std::to_string(key.size)
                                                                              : "a ChaCha20-Poly1305 key is 32 bytes, not " + std::to_string(key.size));
        auto* box = new AeadBox{std::move(made)};
        return Value::object(vm.alloc<ObjResource>("cipher", reinterpret_cast<int64_t>(box), &releaseAead));
    });
    // aeadSeal(cipher, nonce, aad, from, offset, count, into, at): `count` bytes encrypted to
    // `into` at `at`, the 16-byte tag after them; gives count + 16. In place is allowed.
    def("aeadSeal", 8, 8, [](VM& vm, int, Value* args) -> Value {
        AeadBox& box = boxOf<AeadBox>(vm, args[0], "cipher");
        const Span nonce = wholeOf(vm, args[1], "a nonce");
        if (!box.aead->nonceFits(nonce.size)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a nonce of " + std::to_string(nonce.size) + " bytes does not fit this cipher");
        const Span aad = wholeOf(vm, args[2], "associated data");
        const Span in = spanOf(vm, args[3], args[4], args[5], "what is sealed");
        uint8_t* out = destination(vm, args[6], args[7], in.size + Aead::kTag);
        if (out != in.data && out < in.data + in.size && in.data < out + in.size)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "sealed bytes go in the same place as the plain ones, or apart from them, not overlapping");
        box.aead->seal(nonce.data, nonce.size, aad.data, aad.size, in.data, in.size, out, out + in.size);
        return Value::integer(static_cast<int64_t>(in.size + Aead::kTag));
    });
    // aeadOpen(cipher, nonce, aad, from, offset, count, into, at): the `count` sealed bytes (the
    // tag last) checked and decrypted to `into` at `at`; gives count - 16, or -1 where they are
    // not authentic, with nothing written.
    def("aeadOpen", 8, 8, [](VM& vm, int, Value* args) -> Value {
        AeadBox& box = boxOf<AeadBox>(vm, args[0], "cipher");
        const Span nonce = wholeOf(vm, args[1], "a nonce");
        if (!box.aead->nonceFits(nonce.size)) throw vm.runtimeError(Thrown::IllegalArgumentException, "a nonce of " + std::to_string(nonce.size) + " bytes does not fit this cipher");
        const Span aad = wholeOf(vm, args[2], "associated data");
        const Span in = spanOf(vm, args[3], args[4], args[5], "what is opened");
        if (in.size < Aead::kTag) return Value::integer(-1);
        const size_t textSize = in.size - Aead::kTag;
        uint8_t* out = destination(vm, args[6], args[7], textSize);
        if (out != in.data && out < in.data + in.size && in.data < out + textSize)
            throw vm.runtimeError(Thrown::IllegalArgumentException, "opened bytes go in the same place as the sealed ones, or apart from them, not overlapping");
        const bool authentic = box.aead->open(nonce.data, nonce.size, aad.data, aad.size, in.data, textSize, in.data + textSize, out);
        return Value::integer(authentic ? static_cast<int64_t>(textSize) : -1);
    });

    // ---- Public keys (networking plan, part 3) ----

    // x25519(scalar, u): the shared secret, or null where it is all zeros.
    def("x25519", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const Span k = exactly(vm, args[0], 32, "an X25519 private key"), u = exactly(vm, args[1], 32, "an X25519 public key");
        uint8_t out[32];
        if (!x25519(out, k.data, u.data)) return Value::null();
        const Value made = newBytes(vm, out, 32);
        wipe(out, 32);
        return made;
    });
    def("x25519Public", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const Span k = exactly(vm, args[0], 32, "an X25519 private key");
        uint8_t out[32];
        x25519Public(out, k.data);
        return newBytes(vm, out, 32);
    });
    def("ed25519Public", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const Span seed = exactly(vm, args[0], 32, "an Ed25519 private key");
        uint8_t out[32];
        ed25519Public(out, seed.data);
        return newBytes(vm, out, 32);
    });
    def("ed25519Sign", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const Span seed = exactly(vm, args[0], 32, "an Ed25519 private key");
        const Span m = wholeOf(vm, args[1], "a message");
        uint8_t sig[64];
        ed25519Sign(sig, seed.data, m.data, m.size);
        return newBytes(vm, sig, 64);
    });
    // ed25519Verify(public, message, signature): false for any size but 32 and 64 too.
    def("ed25519Verify", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const Span key = wholeOf(vm, args[0], "a public key"), m = wholeOf(vm, args[1], "a message"), sig = wholeOf(vm, args[2], "a signature");
        if (key.size != 32 || sig.size != 64) return Value::boolean(false);
        return Value::boolean(ed25519Verify(key.data, m.data, m.size, sig.data));
    });
    // p256Point(bytes): the point as 04 X Y, checked on the curve; null where it is none.
    def("p256Point", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const Span b = wholeOf(vm, args[0], "a point");
        uint8_t out[65];
        if (!p256Point(b.data, b.size, out)) return Value::null();
        return newBytes(vm, out, 65);
    });
    def("p256ValidPrivate", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const Span d = wholeOf(vm, args[0], "a private key");
        return Value::boolean(d.size == 32 && p256ValidPrivate(d.data));
    });
    def("p256Public", 1, 1, [](VM& vm, int, Value* args) -> Value {
        const Span d = exactly(vm, args[0], 32, "a P-256 private key");
        uint8_t out[65];
        p256Public(out, d.data);
        return newBytes(vm, out, 65);
    });
    // p256Ecdh(private, point65): the shared x, or null where the result is infinity.
    def("p256Ecdh", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const Span d = exactly(vm, args[0], 32, "a P-256 private key"), q = exactly(vm, args[1], 65, "a P-256 point");
        uint8_t out[32];
        if (!p256Ecdh(out, d.data, q.data)) return Value::null();
        const Value made = newBytes(vm, out, 32);
        wipe(out, 32);
        return made;
    });
    // p256Sign(private, hash kind, message): r and s, 64 bytes.
    def("p256Sign", 3, 3, [](VM& vm, int, Value* args) -> Value {
        const Span d = exactly(vm, args[0], 32, "a P-256 private key");
        std::unique_ptr<Hash> h = hashFor(vm, args[1], Value::integer(32));
        const Span m = wholeOf(vm, args[2], "a message");
        h->update(m.data, m.size);
        std::vector<uint8_t> digest(h->size());
        h->finish(digest.data());
        uint8_t sig[64];
        p256Sign(sig, sig + 32, d.data, digest.data(), digest.size(), *h);
        return newBytes(vm, sig, 64);
    });
    // p256Verify(point65, hash kind, message, signature64)
    def("p256Verify", 4, 4, [](VM& vm, int, Value* args) -> Value {
        const Span q = wholeOf(vm, args[0], "a point"), m = wholeOf(vm, args[2], "a message"), sig = wholeOf(vm, args[3], "a signature");
        std::unique_ptr<Hash> h = hashFor(vm, args[1], Value::integer(32));
        if (q.size != 65 || sig.size != 64) return Value::boolean(false);
        h->update(m.data, m.size);
        std::vector<uint8_t> digest(h->size());
        h->finish(digest.data());
        return Value::boolean(p256Verify(q.data, digest.data(), digest.size(), sig.data, sig.data + 32));
    });
    // rsaGenerate(bits, e): [n, e, d, p, q, dp, dq, qinv], waiting on its thread as a read does.
    def("rsaGenerate", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const int64_t bits = vm.toInt(args[0], "bits"), e = vm.toInt(args[1], "public exponent");
        struct Work {
            RsaKey key;
            std::string why;
            bool done = false;
            ~Work() { wipe(key.d.data(), key.d.size()), wipe(key.p.data(), key.p.size()), wipe(key.q.data(), key.q.size()); }
        };
        auto work = std::make_shared<Work>();
        if (bits < 0 || e < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "an RSA key's bits and exponent are positive");
        return vm.blocking([work, bits, e] { work->done = rsaGenerate(static_cast<size_t>(bits), static_cast<uint64_t>(e), work->key, work->why); },
                           [work](VM& vm) -> Value {
                               if (!work->done) throw vm.runtimeError(Thrown::IllegalArgumentException, work->why);
                               const RsaKey& k = work->key;
                               ObjList* list = newList(vm);
                               TempRoot keep(vm, Value::object(list));
                               for (const auto* part : {&k.n, &k.e, &k.d, &k.p, &k.q, &k.dp, &k.dq, &k.qinv}) list->values().push_back(newBytes(vm, part->data(), part->size()));
                               return Value::object(list);
                           });
    });
    // rsaSign([n, e, d, p, q, dp, dq, qinv], hash kind, message, pss, salt length, mgf hash kind)
    def("rsaSign", 6, 6, [](VM& vm, int, Value* args) -> Value {
        if (!args[0].isObjType(ObjType::List) || asList(args[0])->size() != 8) throw vm.runtimeError(Thrown::IllegalArgumentException, "not an RSA private key");
        RsaKey key;
        std::vector<uint8_t>* parts[8] = {&key.n, &key.e, &key.d, &key.p, &key.q, &key.dp, &key.dq, &key.qinv};
        for (size_t i = 0; i < 8; i++) {
            const Value v = vm.listGet(asList(args[0]), i);
            if (v.isNull()) continue;
            const Span s = wholeOf(vm, v, "a key's number");
            parts[i]->assign(s.data, s.data + s.size);
        }
        const HashKind kind = kindOf(vm, args[1]);
        const Span m = wholeOf(vm, args[2], "a message");
        const bool pss = args[3].truthy();
        const int64_t saltLen = vm.toInt(args[4], "salt length");
        const HashKind mgf = kindOf(vm, args[5]);
        if (saltLen < 0) throw vm.runtimeError(Thrown::IllegalArgumentException, "a salt is 0 bytes or more");
        std::vector<uint8_t> signature;
        std::string why;
        const bool done = rsaSign(key, kind, m.data, m.size, pss, static_cast<size_t>(saltLen), mgf, signature, why);
        for (auto* part : parts) wipe(part->data(), part->size());
        if (!done) throw vm.runtimeError(Thrown::IllegalArgumentException, why);
        return newBytes(vm, signature.data(), signature.size());
    });
    // rsaVerify(n, e, hash kind, message, signature, pss, salt length, mgf hash kind)
    def("rsaVerify", 8, 8, [](VM& vm, int, Value* args) -> Value {
        const Span n = wholeOf(vm, args[0], "a modulus"), e = wholeOf(vm, args[1], "an exponent"), m = wholeOf(vm, args[3], "a message"),
                   sig = wholeOf(vm, args[4], "a signature");
        const HashKind kind = kindOf(vm, args[2]), mgf = kindOf(vm, args[7]);
        const int64_t saltLen = vm.toInt(args[6], "salt length");
        if (saltLen < 0 || n.data == nullptr || e.data == nullptr || sig.data == nullptr) return Value::boolean(false);
        return Value::boolean(rsaVerify(std::vector<uint8_t>(n.data, n.data + n.size), std::vector<uint8_t>(e.data, e.data + e.size), kind, m.data, m.size,
                                        std::vector<uint8_t>(sig.data, sig.data + sig.size), args[5].truthy(), static_cast<size_t>(saltLen), mgf));
    });

    // ---- What every secret needs ----

    // equal(a, b): whether the bytes are the same, in a time that depends on their size alone.
    def("equal", 2, 2, [](VM& vm, int, Value* args) -> Value {
        const Span a = wholeOf(vm, args[0], "bytes"), b = wholeOf(vm, args[1], "bytes");
        if (a.size != b.size) return Value::boolean(false);
        return Value::boolean(equal(a.data, b.data, a.size));
    });
    // wipe(bytes): zeros, written so the compiler keeps them.
    def("wipe", 1, 1, [](VM& vm, int, Value* args) -> Value {
        ObjBytes* b = bytesOf(vm, args[0], "what is wiped");
        vm.guardWrite(b);
        wipe(b->data.data(), b->data.size());
        return Value::null();
    });
    // instructions(): the processor's instructions in use, as names.
    def("instructions", 0, 0, [](VM& vm, int, Value*) -> Value {
        const Cpu& c = cpu();
        ObjList* list = newList(vm);
        TempRoot keep(vm, Value::object(list));
        if (c.aes) list->values().push_back(vm.str("aes"));
        if (c.clmul) list->values().push_back(vm.str("clmul"));
        if (c.sha) list->values().push_back(vm.str("sha"));
        return Value::object(list);
    });
    // setPortable(on): the portable code everywhere, or the processor's instructions again.
    def("setPortable", 1, 1, [](VM&, int, Value* args) -> Value {
        setPortable(args[0].truthy());
        return Value::null();
    });
    def("portable", 0, 0, [](VM&, int, Value*) -> Value { return Value::boolean(portable()); });

    vm.defineGlobal("__crypto", Value::object(holder));
}

}  // namespace ww
