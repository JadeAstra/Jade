// What the stencils (src/native/stencils.cpp) read of the VM's objects, as byte offsets: the
// stencils are compiled on their own, without the VM's headers, so they are told here. Each
// number is checked against the VM's own structures when jade.exe is built (native.cpp, by
// static_assert; and std::vector's layout by `jade --native-check`), so a change to an object
// stops the build rather than reading the wrong bytes.
#pragma once

#include <cstdint>

namespace jade::native::layout {

// A value: its kind in the first byte, its payload 8 bytes in.
constexpr int kValueSize = 16, kValuePayload = 8;
constexpr uint8_t kKindBool = 1, kKindInt = 2, kKindFloat = 3, kKindObj = 4, kKindByte = 10;

// Every object (Obj): what it is, the core of a parallel section that owns it (0: shared), and
// whether it is frozen.
constexpr int kObjType = 16, kObjCore = 19, kObjFrozen = 20;
constexpr uint8_t kTypeList = 1, kTypeBytes = 12;

// A list (ObjList): its packed items, a pointer (null where the list is not packed).
constexpr int kListPack = 32;
// Packed items (PackedItems): how they are kept, whether any may be null, how many there are,
// and the block holding them (a std::vector: its first item's address first).
constexpr int kPackStore = 0, kPackNullable = 1, kPackCount = 24, kPackBlock = 48;
constexpr uint8_t kStoreInts = 1, kStoreFloats = 2;

// Bytes (ObjBytes): a std::vector of bytes, its first byte's address, then the end's.
constexpr int kBytesData = 24;

// A list's own items where it is not packed: a std::vector of values.
constexpr int kListItems = 40;

// An iterator (ObjIterator): what it goes through (a value), and the next item's index.
constexpr int kIterSource = 24, kIterIndex = 40;
constexpr uint8_t kTypeIterator = 11;

// An object of a class (ObjInstance): its layout's number, and after the object itself the bits
// saying which of its fields kept as payloads alone hold one (payloadData). A field is where a
// place's cache says (GetProp's: the layout in its top 32 bits, the field's offset in the 16
// below, 0x80 for a payload alone with its kind in the low 2 bits).
constexpr uint8_t kTypeInstance = 8;
constexpr int kInstShape = 32, kInstData = 48;
constexpr uint8_t kKindUndefined = 5, kKindRef = 7;
constexpr uint8_t kTypeIdInt = 1, kTypeIdFloat = 2;

}  // namespace jade::native::layout
