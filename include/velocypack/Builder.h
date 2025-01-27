////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "velocypack/velocypack-common.h"
#include "velocypack/AttributeTranslator.h"
#include "velocypack/Basics.h"
#include "velocypack/Buffer.h"
#include "velocypack/Exception.h"
#include "velocypack/Options.h"
#include "velocypack/Serializable.h"
#include "velocypack/SharedSlice.h"
#include "velocypack/Slice.h"
#include "velocypack/SmallVector.h"
#include "velocypack/Value.h"
#include "velocypack/ValueType.h"

namespace arangodb::velocypack {
class ArrayIterator;
class ObjectIterator;

class Builder {
  friend class Parser;  // The parser needs access to internals.

  // Here are the mechanics of how this building process works:
  // The whole VPack being built starts at where _start points to.
  // The variable _pos keeps the
  // current write position. The method "set" simply writes a new
  // VPack subobject at the current write position and advances
  // it. Whenever one makes an array or object, a ValueLength for
  // the beginning of the value is pushed onto the _stack, which
  // remembers that we are in the process of building an array or
  // object. The _index vectors are used to collect information
  // for the index tables of arrays and objects, which are written
  // behind the subvalues. The add methods are used to keep track
  // of the new subvalue in _index followed by a set, and are
  // what the user from the outside calls. The close method seals
  // the innermost array or object that is currently being built
  // and pops a ValueLength off the _stack. The vectors in _index
  // stay until the next clearTemporary() is called to minimize
  // allocations. In the beginning, the _stack is empty, which
  // allows to build a sequence of unrelated VPack objects in the
  // buffer. Whenever the stack is empty, one can use the start,
  // size and slice methods to get out the ready built VPack
  // object(s).

 private:
  // Here we collect the result
  std::shared_ptr<Buffer<uint8_t>> _buffer;
  // used for quicker access than shared_ptr
  Buffer<uint8_t>* _bufferPtr;
  // Always points to the start of _buffer
  uint8_t* _start;
  // the append position
  ValueLength _pos;

  struct CompoundInfo {
    ValueLength startPos;
    ValueLength indexStartPos;
  };

  static constexpr std::size_t arenaSize = 64;

  // Start positions of open objects/arrays
  // we are using a SmallVector here, so we can stuff the first few
  // levels into the vector without making any allocations
  SmallVector<CompoundInfo, arenaSize>::allocator_type::arena_type _arena;
  SmallVector<CompoundInfo, arenaSize> _stack;

  // Indices for starts of subindex
  std::vector<ValueLength> _indexes;
  // indicates that in the current object the key has been written but the value
  // not yet
  bool _keyWritten;

 public:
  Options const* options;

  // create an empty Builder, using default Options
  Builder();

  // create an empty Builder, using Options
  explicit Builder(Options const* options);

  // create an empty Builder, using an existing buffer and default Options
  explicit Builder(std::shared_ptr<Buffer<uint8_t>> buffer);

  // create an empty Builder, using an existing buffer and Options
  explicit Builder(std::shared_ptr<Buffer<uint8_t>> buffer,
                   Options const* options);

  // create a Builder that uses an existing Buffer and default Options.
  // the Builder will not claim ownership for its Buffer
  explicit Builder(Buffer<uint8_t>& buffer) noexcept;

  // create a Builder that uses an existing Buffer. the Builder will not
  // claim ownership for this Buffer
  explicit Builder(Buffer<uint8_t>& buffer, Options const* options);

  // populate a Builder from a Slice
  explicit Builder(Slice slice, Options const* options = &Options::Defaults);

  ~Builder() = default;

  Builder(Builder const& that);
  Builder& operator=(Builder const& that);
  Builder(Builder&& that) noexcept;
  Builder& operator=(Builder&& that) noexcept;

  // get a reference to the Builder's Buffer object
  // note: this object may be a nullptr if the buffer was already stolen
  // from the Builder, or if the Builder has no ownership for the Buffer
  std::shared_ptr<Buffer<uint8_t>> const& buffer() const { return _buffer; }

  Buffer<uint8_t>& bufferRef() const {
    if (_bufferPtr == nullptr) {
      throw Exception(Exception::InternalError, "Builder has no Buffer");
    }
    return *_bufferPtr;
  }

  // steal the Builder's Buffer object. afterwards the Builder
  // is unusable - note: this may return a nullptr if the Builder does not
  // own the Buffer!
  std::shared_ptr<Buffer<uint8_t>> steal() {
    // After a steal the Builder is broken!
    std::shared_ptr<Buffer<uint8_t>> res(std::move(_buffer));
    _bufferPtr = nullptr;
    _start = nullptr;
    clear();
    return res;
  }

  uint8_t const* data() const noexcept {
    VELOCYPACK_ASSERT(_bufferPtr != nullptr);
    return _bufferPtr->data();
  }

  std::string toString() const;

  std::string toJson() const;

  static Builder clone(Slice const& slice,
                       Options const* options = &Options::Defaults) {
    if (options == nullptr) {
      throw Exception(Exception::InternalError, "Options cannot be a nullptr");
    }

    Builder b(options);
    b.add(slice);
    return b;  // Use return value optimization
  }

  void reserve(ValueLength len) {
    VELOCYPACK_ASSERT(_start == _bufferPtr->data());
    VELOCYPACK_ASSERT(_start + _pos >= _bufferPtr->data());
    VELOCYPACK_ASSERT(_start + _pos <= _bufferPtr->data() + _bufferPtr->size());

    // Reserves len bytes at pos of the current state (top of stack)
    // or throws an exception
    if (_pos + len < _bufferPtr->capacity()) {
      return;  // All OK, we can just increase tos->pos by len
    }

#ifndef VELOCYPACK_64BIT
    (void)checkOverflow(_pos + len);
#endif

    VELOCYPACK_ASSERT(_bufferPtr != nullptr);
    _bufferPtr->reserve(len);
    _start = _bufferPtr->data();
  }

  // Clear and start from scratch:
  void clear() noexcept {
    _pos = 0;
    _stack.clear();
    _indexes.clear();
    if (_bufferPtr != nullptr) {
      _bufferPtr->reset();
      _start = _bufferPtr->data();
    }
    _keyWritten = false;
  }

  // Return a pointer to the start of the result:
  uint8_t* start() const {
    if (isClosed()) {
      return _start;
    }
    throw Exception(Exception::BuilderNotSealed);
  }

  // Return a Slice of the result:
  inline Slice slice() const {
    if (isEmpty()) {
      return Slice();
    }
    return Slice(start());
  }

  // Return a SharedSlice of the result (makes a copy of the slice)
  [[nodiscard]] SharedSlice sharedSlice() const& {
    if (isEmpty()) {
      return SharedSlice{};
    }
    if (isClosed()) {
      return SharedSlice{*_buffer};
    }

    throw Exception(Exception::BuilderNotSealed);
  }

  // Steal the buffer and return a SharedSlice created from it.
  // Afterwards the Builder is unusable.
  // If the Builder is not responsible for its buffer, a copy is created.
  [[nodiscard]] SharedSlice sharedSlice() && {
    if (isEmpty()) {
      return SharedSlice{};
    }
    if (VELOCYPACK_UNLIKELY(!isClosed())) {
      throw Exception(Exception::BuilderNotSealed);
    }
    if (_buffer == nullptr) {
      // We're not responsible for the buffer, we need to copy.
      return SharedSlice{*_bufferPtr};
    }
    auto rv = SharedSlice{std::move(*_buffer)};
    _buffer = nullptr;
    _bufferPtr = nullptr;
    _start = nullptr;
    clear();
    return rv;
  }

  // Compute the actual size here, but only when sealed
  ValueLength size() const {
    if (!isClosed()) {
      throw Exception(Exception::BuilderNotSealed);
    }
    return _pos;
  }

  inline bool isEmpty() const noexcept { return _pos == 0; }

  inline bool isClosed() const noexcept { return _stack.empty(); }

  bool isOpenArray() const noexcept {
    if (_stack.empty()) {
      return false;
    }
    ValueLength const pos = _stack.back().startPos;
    return _start[pos] == 0x06 || _start[pos] == 0x13;
  }

  bool isOpenObject() const noexcept {
    if (_stack.empty()) {
      return false;
    }
    ValueLength const pos = _stack.back().startPos;
    return _start[pos] == 0x0b || _start[pos] == 0x14;
  }

  inline void openArray(bool unindexed = false) {
    openCompoundValue(unindexed ? 0x13 : 0x06);
  }

  inline void openObject(bool unindexed = false) {
    openCompoundValue(unindexed ? 0x14 : 0x0b);
  }

  template<typename T>
  uint8_t* addUnchecked(Slice key, T const& sub) {
    bool needCleanup = !_stack.empty();
    if (needCleanup) {
      reportAdd();
    }
    try {
      set(key);
      return writeValue(sub);
    } catch (...) {
      // clean up in case of an exception
      if (needCleanup) {
        cleanupAdd();
      }
      throw;
    }
  }

  template<typename T>
  uint8_t* addUnchecked(std::string_view attrName, T const& sub) {
    bool needCleanup = !_stack.empty();
    if (needCleanup) {
      reportAdd();
    }
    try {
      set(Value(attrName));
      return writeValue(sub);
    } catch (...) {
      // clean up in case of an exception
      if (needCleanup) {
        cleanupAdd();
      }
      throw;
    }
  }

  template<typename T>
  [[deprecated]] uint8_t* addUnchecked(char const* attrName,
                                       std::size_t attrLength, T const& sub) {
    bool needCleanup = !_stack.empty();
    if (needCleanup) {
      reportAdd();
    }
    try {
      set(ValuePair(attrName, attrLength, ValueType::String));
      return writeValue(sub);
    } catch (...) {
      // clean up in case of an exception
      if (needCleanup) {
        cleanupAdd();
      }
      throw;
    }
  }

  // Add a subvalue into an object from a Value:
  inline uint8_t* add(std::string_view attrName, Value const& sub) {
    return addInternal<Value>(attrName, sub);
  }

  template<typename T,
           std::enable_if_t<!std::is_same_v<std::decay_t<T>, Value> &&
                                std::is_constructible_v<Value, T&&>,
                            bool> = true>
  uint8_t* add(std::string_view attrName, T&& sub) {
    return add(attrName, Value(std::forward<T>(sub)));
  }

  [[deprecated]] inline uint8_t* add(char const* attrName,
                                     std::size_t attrLength, Value const& sub) {
    return addInternal<Value>(std::string_view(attrName, attrLength), sub);
  }

  // Add a subvalue into an object from a Slice:
  inline uint8_t* add(std::string_view attrName, Slice const& sub) {
    return addInternal<Slice>(attrName, sub);
  }

  [[deprecated]] inline uint8_t* add(char const* attrName,
                                     std::size_t attrLength, Slice const& sub) {
    return addInternal<Slice>(std::string_view(attrName, attrLength), sub);
  }

  // Add a subvalue into an object from a ValuePair:
  inline uint8_t* add(std::string_view attrName, ValuePair const& sub) {
    return addInternal<ValuePair>(attrName, sub);
  }

  [[deprecated]] inline uint8_t* add(char const* attrName,
                                     std::size_t attrLength,
                                     ValuePair const& sub) {
    return addInternal<ValuePair>(std::string_view(attrName, attrLength), sub);
  }

  // Add a subvalue into an object from a IStringFromParts:
  inline uint8_t* add(std::string_view attrName, IStringFromParts const& sub) {
    return addInternal<IStringFromParts>(attrName, sub);
  }

  // Add a subvalue into an object from a Serializable:
  inline uint8_t* add(std::string_view attrName, Serialize const& sub) {
    return addInternal<Serializable>(attrName, sub._sable);
  }

  inline uint8_t* add(char const* attrName, std::size_t attrLength,
                      Serialize const& sub) {
    return addInternal<Serializable>(std::string_view(attrName, attrLength),
                                     sub._sable);
  }

  // Add a subvalue into an array from a Value:
  inline uint8_t* add(Value const& sub) { return addInternal<Value>(sub); }

  // Add a slice to an array
  inline uint8_t* add(Slice const& sub) { return addInternal<Slice>(sub); }

  // Add a shared slice to an array
  inline uint8_t* add(SharedSlice const& sub) {
    return addInternal<Slice>(sub.slice());
  }

  // Add a subvalue into an array from a ValuePair:
  inline uint8_t* add(ValuePair const& sub) {
    return addInternal<ValuePair>(sub);
  }

  // Add a subvalue into an array from a ValueString2Parts:
  inline uint8_t* add(IStringFromParts const& sub) {
    return addInternal<IStringFromParts>(sub);
  }

  // Add a subvalue into an array from a Serializable:
  inline uint8_t* add(Serialize const& sub) {
    return addInternal<Serializable>(sub._sable);
  }

  // Add a subvalue into an object from a Value:
  inline uint8_t* addTagged(std::string_view attrName, uint64_t tag,
                            Value const& sub) {
    return addInternalTagged<Value>(attrName, tag, sub);
  }

  [[deprecated]] inline uint8_t* addTagged(char const* attrName,
                                           std::size_t attrLength, uint64_t tag,
                                           Value const& sub) {
    return addInternalTagged<Value>(std::string_view(attrName, attrLength), tag,
                                    sub);
  }

  // Add a subvalue into an object from a Slice:
  inline uint8_t* addTagged(std::string_view attrName, uint64_t tag,
                            Slice const& sub) {
    return addInternalTagged<Slice>(attrName, tag, sub);
  }

  [[deprecated]] inline uint8_t* addTagged(char const* attrName,
                                           std::size_t attrLength, uint64_t tag,
                                           Slice const& sub) {
    return addInternalTagged<Slice>(std::string_view(attrName, attrLength), tag,
                                    sub);
  }

  // Add a subvalue into an object from a ValuePair:
  inline uint8_t* addTagged(std::string_view attrName, uint64_t tag,
                            ValuePair const& sub) {
    return addInternalTagged<ValuePair>(attrName, tag, sub);
  }

  [[deprecated]] inline uint8_t* addTagged(char const* attrName,
                                           std::size_t attrLength, uint64_t tag,
                                           ValuePair const& sub) {
    return addInternalTagged<ValuePair>(std::string_view(attrName, attrLength),
                                        tag, sub);
  }

  // Add a subvalue into an object from a Serializable:
  inline uint8_t* addTagged(std::string_view attrName, uint64_t tag,
                            Serialize const& sub) {
    return addInternalTagged<Serializable>(attrName, tag, sub._sable);
  }

  [[deprecated]] inline uint8_t* addTagged(char const* attrName,
                                           std::size_t attrLength, uint64_t tag,
                                           Serialize const& sub) {
    return addInternalTagged<Serializable>(
        std::string_view(attrName, attrLength), tag, sub._sable);
  }

  // Add a subvalue into an array from a Value:
  inline uint8_t* addTagged(uint64_t tag, Value const& sub) {
    return addInternalTagged<Value>(tag, sub);
  }

  // Add a slice to an array
  inline uint8_t* addTagged(uint64_t tag, Slice const& sub) {
    return addInternalTagged<Slice>(tag, sub);
  }

  // Add a subvalue into an array from a ValuePair:
  inline uint8_t* addTagged(uint64_t tag, ValuePair const& sub) {
    return addInternalTagged<ValuePair>(tag, sub);
  }

  // Add a subvalue into an array from a Serialize:
  inline uint8_t* addTagged(uint64_t tag, Serialize const& sub) {
    return addInternalTagged<Serializable>(tag, sub._sable);
  }

  // Add an External slice to an array
  uint8_t* addExternal(uint8_t const* sub) {
    if (options->disallowExternals) {
      // External values explicitly disallowed as a security
      // precaution
      throw Exception(Exception::BuilderExternalsDisallowed);
    }

    bool haveReported = false;
    if (!_stack.empty()) {
      if (!_keyWritten) {
        reportAdd();
        haveReported = true;
      }
    }
    try {
      checkKeyHasValidType(Slice(sub));
      auto oldPos = _pos;
      reserve(1 + sizeof(void*));
      // store pointer. this doesn't need to be portable
      appendByteUnchecked(0x1d);
      memcpy(_start + _pos, &sub, sizeof(void*));
      advance(sizeof(void*));
      return _start + oldPos;
    } catch (...) {
      // clean up in case of an exception
      if (haveReported) {
        cleanupAdd();
      }
      throw;
    }
  }

  // Add all subkeys and subvalues into an object from an ObjectIterator
  // and leaves open the object intentionally
  uint8_t* add(ObjectIterator&& sub);
  uint8_t* add(ObjectIterator& sub) { return add(std::move(sub)); }

  // Add all subvalues into an array from an ArrayIterator
  // and leaves open the array intentionally
  uint8_t* add(ArrayIterator&& sub);
  uint8_t* add(ArrayIterator& sub) { return add(std::move(sub)); }

  // Seal the innermost array or object:
  Builder& close();

  // whether or not a specific key is present in an Object value
  bool hasKey(std::string_view key) const;

  // return an attribute from an Object value
  Slice getKey(std::string_view key) const;

  // Syntactic sugar for add:
  Builder& operator()(std::string_view attrName, Value const& sub) {
    add(attrName, sub);
    return *this;
  }

  // Syntactic sugar for add:
  Builder& operator()(std::string_view attrName, ValuePair const& sub) {
    add(attrName, sub);
    return *this;
  }

  // Syntactic sugar for add:
  Builder& operator()(std::string_view attrName, IStringFromParts const& sub) {
    add(attrName, sub);
    return *this;
  }

  // Syntactic sugar for add:
  Builder& operator()(std::string_view attrName, Slice sub) {
    add(attrName, sub);
    return *this;
  }

  // Syntactic sugar for add:
  Builder& operator()(Value const& sub) {
    add(sub);
    return *this;
  }

  // Syntactic sugar for add:
  Builder& operator()(ValuePair const& sub) {
    add(sub);
    return *this;
  }

  // Syntactic sugar for add:
  Builder& operator()(IStringFromParts const& sub) {
    add(sub);
    return *this;
  }

  // Syntactic sugar for add:
  Builder& operator()(Slice sub) {
    add(sub);
    return *this;
  }

  // Syntactic sugar for close:
  Builder& operator()() {
    close();
    return *this;
  }
  
  void resetTo(std::size_t value) {
    _pos = value;
    VELOCYPACK_ASSERT(_bufferPtr != nullptr);
    _bufferPtr->resetTo(value);
  }

  // move byte position x bytes ahead
  void advance(std::size_t value) noexcept {
    _pos += value;
    VELOCYPACK_ASSERT(_bufferPtr != nullptr);
    _bufferPtr->advance(value);
  }

 private:
  void closeLevel() noexcept;

  void sortObjectIndexShort(uint8_t* objBase,
                            std::vector<ValueLength>::iterator indexStart,
                            std::vector<ValueLength>::iterator indexEnd) const;

  void sortObjectIndexLong(uint8_t* objBase,
                           std::vector<ValueLength>::iterator indexStart,
                           std::vector<ValueLength>::iterator indexEnd) const;

  void sortObjectIndex(uint8_t* objBase,
                       std::vector<ValueLength>::iterator indexStart,
                       std::vector<ValueLength>::iterator indexEnd) {
    std::size_t const n = std::distance(indexStart, indexEnd);

    if (n > 32) {
      sortObjectIndexLong(objBase, indexStart, indexEnd);
    } else {
      sortObjectIndexShort(objBase, indexStart, indexEnd);
    }
  }

  // close for the empty case:
  Builder& closeEmptyArrayOrObject(ValueLength pos, bool isArray);

  // close for the compact case:
  bool closeCompactArrayOrObject(ValueLength pos, bool isArray,
                                 std::vector<ValueLength>::iterator indexStart,
                                 std::vector<ValueLength>::iterator indexEnd);

  // close for the array case:
  Builder& closeArray(ValueLength pos,
                      std::vector<ValueLength>::iterator indexStart,
                      std::vector<ValueLength>::iterator indexEnd);

  void addNull() { appendByte(0x18); }

  void addFalse() { appendByte(0x19); }

  void addTrue() { appendByte(0x1a); }

  void addDouble(double v) {
    uint64_t dv;
    ValueLength vSize = sizeof(double);
    memcpy(&dv, &v, vSize);
    reserve(1 + vSize);
    appendByteUnchecked(0x1b);
    for (uint64_t x = dv; vSize > 0; vSize--) {
      appendByteUnchecked(x & 0xff);
      x >>= 8;
    }
  }

  void addInt(int64_t v) {
    if (v >= 0 && v <= 9) {
      // SmallInt
      appendByte(static_cast<uint8_t>(0x30 + v));
    } else if (v < 0 && v >= -6) {
      // SmallInt
      appendByte(static_cast<uint8_t>(0x40 + v));
    } else {
      // regular int
      appendInt(v, 0x1f);
    }
  }

  void addUInt(uint64_t v) {
    if (v <= 9) {
      // SmallInt
      appendByte(static_cast<uint8_t>(0x30 + v));
    } else {
      // regular UInt
      appendUInt(v, 0x27);
    }
  }

  void addBCD(int8_t sign, int32_t exponent, char* mantissa,
              uint64_t mantissaLength) {
    if (options->disallowBCD) {
      // BCD values explicitly disallowed
      throw Exception(Exception::BuilderBCDDisallowed);
    }

    bool isOdd = mantissaLength % 2 != 0;
    uint64_t byteLen = mantissaLength / 2 + (isOdd ? 1 : 0);

    uint64_t n = 0;
    for (uint64_t x = byteLen; x != 0; x >>= 8) {
      n++;
    }

    reserve(1 + n + 4 + byteLen);

    appendByte(static_cast<uint8_t>((sign >= 0 ? 0xc8 : 0xd0) + n - 1));

    uint64_t v = byteLen;
    for (uint64_t i = 0; i < n; ++i) {
      appendByteUnchecked(v & 0xff);
      v >>= 8;
    }

    appendLengthUnchecked<4>(static_cast<ValueLength>(exponent));

    uint64_t i = 0;
    while (i < mantissaLength) {
      if (isOdd && i == 0) {
        appendByte(mantissa[i]);
        i++;
        continue;
      }

      appendByte((mantissa[i] << 4) | mantissa[i + 1]);
      i += 2;
    }
  }

  void addUTCDate(int64_t v) {
    constexpr uint8_t vSize = sizeof(int64_t);  // is always 8
    reserve(1 + vSize);
    appendByteUnchecked(0x1c);
    appendLengthUnchecked<vSize>(toUInt64(v));
  }

  void appendTag(uint64_t tag);

  inline void checkKeyHasValidType(bool isValid) {
    if (!_stack.empty()) {
      ValueLength const pos = _stack.back().startPos;
      if (_start[pos] == 0x0b || _start[pos] == 0x14) {
        if (VELOCYPACK_UNLIKELY(!_keyWritten && !isValid)) {
          throw Exception(Exception::BuilderKeyMustBeString);
        }
        _keyWritten = !_keyWritten;
      }
    }
  }

  inline void checkKeyHasValidType(Slice item) {
    if (!_stack.empty()) {
      ValueLength pos = _stack.back().startPos;
      if (_start[pos] == 0x0b || _start[pos] == 0x14) {
        if (VELOCYPACK_UNLIKELY(!_keyWritten && !item.isString() &&
                                !item.isSmallInt() && !item.isUInt())) {
          throw Exception(Exception::BuilderKeyMustBeString);
        }
        _keyWritten = !_keyWritten;
      }
    }
  }

  inline void addArray(bool unindexed = false) {
    addCompoundValue(unindexed ? 0x13 : 0x06);
  }

  inline void addObject(bool unindexed = false) {
    addCompoundValue(unindexed ? 0x14 : 0x0b);
  }

  // add without a tag
  template<typename T>
  uint8_t* addInternal(T const& sub) {
    if (_stack.empty() || std::is_same<T, Serializable>::value) {
      return set(sub);
    }

    if (!_keyWritten) {
      reportAdd();
    }
    try {
      return set(sub);
    } catch (...) {
      // clean up in case of an exception
      cleanupAdd();
      throw;
    }
  }

  // add with a tag
  template<typename T>
  uint8_t* addInternalTagged(uint64_t tag, T const& sub) {
    if (_stack.empty()) {
      if (tag != 0) {
        appendTag(tag);
      }
      return set(sub);
    }

    if (!_keyWritten) {
      reportAdd();
    }
    try {
      if (tag != 0) {
        appendTag(tag);
      }
      return set(sub);
    } catch (...) {
      // clean up in case of an exception
      cleanupAdd();
      throw;
    }
  }

  template<typename T>
  uint8_t* addInternal(std::string_view attrName, T const& sub) {
    bool haveReported = false;
    if (!_stack.empty()) {
      ValueLength const to = _stack.back().startPos;
      if (VELOCYPACK_UNLIKELY(_start[to] != 0x0b && _start[to] != 0x14)) {
        throw Exception(Exception::BuilderNeedOpenObject);
      }
      if (VELOCYPACK_UNLIKELY(_keyWritten)) {
        throw Exception(Exception::BuilderKeyAlreadyWritten);
      }
      reportAdd();
      haveReported = true;
    }

    try {
      if (options->attributeTranslator != nullptr) {
        // check if a translation for the attribute name exists
        uint8_t const* translated =
            options->attributeTranslator->translate(attrName);

        if (translated != nullptr) {
          Slice item(translated);
          ValueLength const l = item.byteSize();
          reserve(l);
          memcpy(_start + _pos, translated, checkOverflow(l));
          advance(l);
          return writeValue(sub);
        }
        // otherwise fall through to regular behavior
      }

      set(ValuePair(attrName.data(), attrName.size(), ValueType::String));
      return writeValue(sub);
    } catch (...) {
      // clean up in case of an exception
      if (haveReported) {
        cleanupAdd();
      }
      throw;
    }
  }

  template<typename T>
  uint8_t* addInternalTagged(std::string_view attrName, uint64_t tag,
                             T const& sub) {
    bool haveReported = false;
    if (!_stack.empty()) {
      ValueLength const to = _stack.back().startPos;
      if (VELOCYPACK_UNLIKELY(_start[to] != 0x0b && _start[to] != 0x14)) {
        throw Exception(Exception::BuilderNeedOpenObject);
      }
      if (VELOCYPACK_UNLIKELY(_keyWritten)) {
        throw Exception(Exception::BuilderKeyAlreadyWritten);
      }
      reportAdd();
      haveReported = true;
    }

    try {
      if (options->attributeTranslator != nullptr) {
        // check if a translation for the attribute name exists
        uint8_t const* translated =
            options->attributeTranslator->translate(attrName);

        if (translated != nullptr) {
          Slice item(translated);
          ValueLength const l = item.byteSize();
          reserve(l);
          memcpy(_start + _pos, translated, checkOverflow(l));
          advance(l);
          return writeValueTagged(tag, sub);
        }
        // otherwise fall through to regular behavior
      }

      set(ValuePair(attrName.data(), attrName.size(), ValueType::String));
      return writeValueTagged(tag, sub);
    } catch (...) {
      // clean up in case of an exception
      if (haveReported) {
        cleanupAdd();
      }
      throw;
    }
  }

  template<typename T>
  inline uint8_t* writeValue(T const& sub) {
    _keyWritten = true;
    return set(sub);
  }

  template<typename T>
  inline uint8_t* writeValueTagged(uint64_t tag, T const& sub) {
    _keyWritten = true;
    if (VELOCYPACK_UNLIKELY(tag != 0)) {
      appendTag(tag);
    }
    return set(sub);
  }

  void addCompoundValue(uint8_t type) {
    reserve(9);
    // an Array or Object is started:
    _stack.push_back(CompoundInfo{_pos, _indexes.size()});
    appendByteUnchecked(type);
    std::memset(_start + _pos, 0, 8);
    advance(8);  // Will be filled later with bytelength and nr subs
  }

  void openCompoundValue(uint8_t type) {
    if (_stack.empty()) {
      addCompoundValue(type);
    } else if (_keyWritten) {
      _keyWritten = false;
      addCompoundValue(type);
    } else {
      ValueLength const to = _stack.back().startPos;
      if (VELOCYPACK_UNLIKELY(_start[to] != 0x06 && _start[to] != 0x13)) {
        throw Exception(Exception::BuilderNeedOpenArray);
      }
      reportAdd();
      try {
        addCompoundValue(type);
      } catch (...) {
        cleanupAdd();
        throw;
      }
    }
  }

  uint8_t* set(Value const& item);

  uint8_t* set(ValuePair const& pair);

  uint8_t* set(IStringFromParts const& parts);

  uint8_t* set(Slice const& item);

  uint8_t* set(Serializable const& sable) {
    auto const oldPos = _pos;

    sable.toVelocyPack(*this);
    return _start + oldPos;
  }

  void cleanupAdd() noexcept;

  void reportAdd();

  template<uint64_t n>
  void appendLengthUnchecked(ValueLength v) {
    for (uint64_t i = 0; i < n; ++i) {
      appendByteUnchecked(v & 0xff);
      v >>= 8;
    }
  }

  void appendUInt(uint64_t v, uint8_t base) {
    reserve(9);
    ValueLength save = _pos;
    advance(1);
    uint8_t vSize = 0;
    do {
      vSize++;
      appendByteUnchecked(static_cast<uint8_t>(v & 0xff));
      v >>= 8;
    } while (v != 0);
    _start[save] = base + vSize;
  }

  // returns number of bytes required to store the value in 2s-complement
  static inline uint8_t intLength(int64_t value) {
    if (value >= -0x80 && value <= 0x7f) {
      // shortcut for the common case
      return 1;
    }
    uint64_t x = value >= 0 ? static_cast<uint64_t>(value)
                            : static_cast<uint64_t>(-(value + 1));
    uint8_t xSize = 0;
    do {
      xSize++;
      x >>= 8;
    } while (x >= 0x80);
    return xSize + 1;
  }

  void appendInt(int64_t v, uint8_t base) {
    uint8_t vSize = intLength(v);
    uint64_t x;
    if (vSize == 8) {
      x = toUInt64(v);
    } else {
      int64_t shift = 1LL << (vSize * 8 - 1);  // will never overflow!
      x = v >= 0 ? static_cast<uint64_t>(v)
                 : static_cast<uint64_t>(v + shift) + shift;
    }
    reserve(1 + vSize);
    appendByteUnchecked(base + vSize);
    while (vSize-- > 0) {
      appendByteUnchecked(x & 0xff);
      x >>= 8;
    }
  }

  inline void appendByte(uint8_t value) {
    reserve(1);
    appendByteUnchecked(value);
  }

  inline void appendByteUnchecked(uint8_t value) {
    _start[_pos++] = value;
    VELOCYPACK_ASSERT(_bufferPtr != nullptr);
    _bufferPtr->advance();
  }

  // move byte position x bytes back
  inline void rollback(std::size_t value) noexcept {
    _pos -= value;
    VELOCYPACK_ASSERT(_bufferPtr != nullptr);
    _bufferPtr->rollback(value);
  }

  bool checkAttributeUniqueness(Slice obj) const;
  bool checkAttributeUniquenessSorted(Slice obj) const;
  bool checkAttributeUniquenessUnsorted(Slice obj) const;

  ValueLength effectivePaddingForOneByteMembers() const noexcept;
  ValueLength effectivePaddingForTwoByteMembers() const noexcept;
};

struct BuilderNonDeleter {
  void operator()(Builder*) {}
};

struct BuilderContainer {
  BuilderContainer(Builder* builder) : builder(builder) {}

  Builder& operator*() { return *builder; }
  Builder const& operator*() const { return *builder; }
  Builder* operator->() { return builder; }
  Builder const* operator->() const { return builder; }
  Builder* builder;
};

// convenience class scope guard for building objects
struct ObjectBuilder final : public BuilderContainer,
                             private NonHeapAllocatable,
                             NonCopyable {
  explicit ObjectBuilder(Builder* builder) : BuilderContainer(builder) {
    builder->openObject(/*allowUnindexed*/ false);
  }
  // this stunt is only necessary to prevent implicit conversions
  // from char const* to bool for the second call argument
  template<class T,
           class = typename std::enable_if<std::is_same<bool, T>::value>::type>
  ObjectBuilder(Builder* builder, T allowUnindexed)
      : BuilderContainer(builder) {
    builder->openObject(allowUnindexed);
  }

  ObjectBuilder(Builder* builder, std::string_view attributeName,
                bool allowUnindexed = false)
      : BuilderContainer(builder) {
    builder->add(attributeName, Value(ValueType::Object, allowUnindexed));
  }
  ~ObjectBuilder() {
    try {
      if (!builder->isClosed()) {
        builder->close();
      }
    } catch (...) {
      // destructors must not throw. however, we can at least
      // signal something is very wrong in debug mode
      VELOCYPACK_ASSERT(builder->isClosed());
    }
  }
};

// convenience class scope guard for building arrays
struct ArrayBuilder final : public BuilderContainer,
                            private NonHeapAllocatable,
                            NonCopyable {
  explicit ArrayBuilder(Builder* builder) : BuilderContainer(builder) {
    builder->openArray(/*allowUnindexed*/ false);
  }
  // this stunt is only necessary to prevent implicit conversions
  // from char const* to bool for the second call argument
  template<class T,
           class = typename std::enable_if<std::is_same<bool, T>::value>::type>
  ArrayBuilder(Builder* builder, T allowUnindexed) : BuilderContainer(builder) {
    builder->openArray(allowUnindexed);
  }
  ArrayBuilder(Builder* builder, std::string_view attributeName,
               bool allowUnindexed = false)
      : BuilderContainer(builder) {
    builder->add(attributeName, Value(ValueType::Array, allowUnindexed));
  }

  ~ArrayBuilder() {
    try {
      if (!builder->isClosed()) {
        builder->close();
      }
    } catch (...) {
      // destructors must not throw. however, we can at least
      // signal something is very wrong in debug mode
      VELOCYPACK_ASSERT(builder->isClosed());
    }
  }
};

}  // namespace arangodb::velocypack

using VPackBuilder = arangodb::velocypack::Builder;
using VPackBuilderNonDeleter = arangodb::velocypack::BuilderNonDeleter;
using VPackBuilderContainer = arangodb::velocypack::BuilderContainer;
using VPackObjectBuilder = arangodb::velocypack::ObjectBuilder;
using VPackArrayBuilder = arangodb::velocypack::ArrayBuilder;
