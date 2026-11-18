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
// A string's characters (ObjString::units_, a std::string) and its hash (bit 32 set where it has
// been worked out; ObjString::hash_), for native code's probing of string keys (2026-11-16).
constexpr int kStringUnits = 40, kStringHash = 72, kStringWidth = 25;
// The first character's address in a std::string, by the standard library's layout: MSVC's keeps
// up to 15 bytes in the object itself, a pointer past that (its room 8 bytes after its size);
// libstdc++'s always a pointer first. Checked as ww starts its native code (native.cpp,
// stringAsRead): where it does not hold, no step reads a string's characters.
inline const uint8_t* stringUnits(const uint8_t* s) {
#if defined(_MSC_VER)
    uint64_t room;
    __builtin_memcpy(&room, s + 24, 8);
    if (room < 16) return s;
#endif
    const uint8_t* p;
    __builtin_memcpy(&p, s, 8);
    return p;
}
// A type (ObjTypeInfo): its class, and its arguments (a std::vector, its first one's address first).
constexpr int kTypeInfoCls = 32, kTypeInfoArgs = 40;
// A class (ObjClass): its number among the value types kept in the value itself (0 for none). The
// standard library's containers before it differ in size: MSVC's and libstdc++'s.
#if defined(_MSC_VER)
constexpr int kClassSmallId = 828;
#else
constexpr int kClassSmallId = 796;
#endif
// A map's or a set's table (ObjMap::map, ObjSet::items, a ValueMap), and a set's map where it is a
// view of one; in the table, the entries (a std::vector of Entry: a key, a value, whether it is
// live, its hash), the slots (a std::vector of uint32_t: an entry's place plus one, 0 empty,
// all ones gone), the live entries' count and the shift for the first slot a hash tries.
constexpr int kTableAt = 24, kSetViewOf = 104;
constexpr uint8_t kTypeMapObj = 2, kTypeSetObj = 17;  // ObjType::Map, ObjType::Set
constexpr int kTableEntries = 0, kTableSlots = 24, kTableLive = 48, kTableShift = 56;
constexpr int kEntrySize = 40, kEntryValue = 16, kEntryLive = 32, kEntryHash = 36;
// The table's version (moved on with every key added or taken out), a set's flags where it is a
// view (of the map's entries, of its values), and the version an iterator began with.
constexpr int kTableVersion = 64, kSetEntries = 112, kSetValues = 113, kIterVersion = 64;
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
constexpr uint8_t kKindChar = 9;  // a char, which equals a string of it alone as a key
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

// How many times round a region made first counts what it runs before it is made again from the
// counts (registers-plan.html part B, 2026-11-17): its back edge leaves for the VM at this count.
constexpr uint64_t kCountedRounds = 1000;

}  // namespace ww::native::layout
