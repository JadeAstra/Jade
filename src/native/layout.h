// What the stencils (src/native/stencils.cpp) read of the VM's objects, as byte offsets: the
// stencils are compiled on their own, without the VM's headers, so they are told here. Each
// number is checked against the VM's own structures when ww.exe is built (native.cpp, by
// static_assert; and std::vector's layout by `ww --native-check`), so a change to an object
// stops the build rather than reading the wrong bytes.
#pragma once

#include <cstdint>

namespace ww::native::layout {

// A value: its kind in the first byte, its payload 8 bytes in.
constexpr int kValueSize = 16, kValuePayload = 8;
constexpr uint8_t kKindNull = 0, kKindBool = 1, kKindInt = 2, kKindFloat = 3, kKindObj = 4, kKindByte = 10;

// Every object (Obj): what it is, the core of a parallel section that owns it (0: shared), and
// whether it is frozen.
constexpr int kObjType = 16, kObjCore = 19, kObjFrozen = 20;
constexpr uint8_t kTypeList = 1, kTypeBytes = 12;

// A list (ObjList): its packed items, a pointer (null where the list is not packed).
constexpr int kListPack = 32;
// What it was made to hold (ObjList::type, an ObjTypeInfo), just before.
constexpr int kListType = 24;
// A string (ObjString, ObjType::String): its length in characters.
constexpr uint8_t kTypeString = 0;
constexpr int kStringLength = 32;
// A type (ObjTypeInfo): its class, and its arguments (a std::vector, its first one's address first).
constexpr int kTypeInfoCls = 32, kTypeInfoArgs = 40;
// Packed items (PackedItems): how they are kept, whether any may be null, how many there are,
// and the block holding them (a std::vector: its first item's address first).
constexpr int kPackStore = 0, kPackNullable = 1, kPackCount = 24, kPackBlock = 48;
// Where nullable: a bit per item, set where it is null (a std::vector of words, its first one's address first).
constexpr int kPackNulls = 72;
constexpr uint8_t kStoreInts = 1, kStoreFloats = 2, kStoreBools = 3;  // ListStore's Ints, Floats, Bools (a byte each, 0 or 1)
// A list of a value type's items: the bytes an item takes, and the value type (an ObjClass).
constexpr int kPackWidth = 4, kPackItemClass = 32;
constexpr uint8_t kStoreItems = 5;
// A list of narrow items (List<byte>, ...): ListStore::Narrow, what each is at kPackNarrow.
constexpr uint8_t kStoreNarrow = 4, kNarrowByte = 1;
// A captured variable's cell (ObjUpvalue): where its value is.
constexpr int kUpvalueLocation = 24;
constexpr int kPackNarrow = 40;

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
constexpr int kInstClass = 24;  // its class (ObjInstance::cls)
constexpr uint8_t kKindUndefined = 5, kKindRef = 7;
// A value of a value type (M9.5 B1): a Ref (the object holding it, a list of items say, the item's
// position, the place in the item and how deep it is) or a Small value (its bytes in the payload,
// its type's number); a list's items' own type and where in an item it is kept (G4); and the
// object a frame keeps its variables' values in.
constexpr uint8_t kKindSmall = 8;
constexpr int kRefDepth = 1, kRefOffset = 2, kRefIndex = 4;
constexpr int kPackItemType = 16, kPackTypeSlot = 8;
constexpr uint8_t kTypePlaces = 19;
constexpr uint8_t kTypeValue = 16;  // a value of a value type in a box (ObjValue, 2026-11-15)
constexpr uint8_t kTypeIdInt = 1, kTypeIdFloat = 2;
// A place of a frame's (ObjPlace): the class of the value it holds, that value's own type (null
// for none), and where its bytes start (2026-11-13: a value made where a call leaves it).
constexpr int kPlaceClass = 24, kPlaceType = 32, kPlaceData = 40;
// A value of a value type in a box (ObjValue): its fields' bytes, as a place keeps them (2026-11-15).
constexpr int kValueData = 40;
// The places a line of execution keeps values of its frames' variables in (PlaceArena), each
// this many bytes on from the one before: a frame's place k is k strides down from its first.
constexpr int kPlaceStride = 104, kPlaceBytes = 64;

}  // namespace ww::native::layout
