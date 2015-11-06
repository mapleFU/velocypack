////////////////////////////////////////////////////////////////////////////////
/// @brief Library to build up VPack documents.
///
/// DISCLAIMER
///
/// Copyright 2015 ArangoDB GmbH, Cologne, Germany
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
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "velocypack/velocypack-common.h"
#include "velocypack/Collection.h"
#include "velocypack/Iterator.h"
#include "velocypack/Slice.h"
#include "velocypack/Value.h"
#include "velocypack/ValueType.h"

using namespace arangodb::velocypack;

void Collection::forEach (Slice const& slice, std::function<bool(Slice const&, ValueLength)> const& cb) {
  ArrayIterator it(slice);
  ValueLength index = 0;

  while (it.valid()) {
    if (! cb(it.value(), index)) {
      // abort
      return;
    }
    it.next();
    ++index;
  }
}
        
Builder Collection::filter (Slice const& slice, std::function<bool(Slice const&, ValueLength)> const& cb) {
  // construct a new Array
  Builder b;
  b.add(Value(ValueType::Array));

  ArrayIterator it(slice);
  ValueLength index = 0;

  while (it.valid()) {
    if (cb(it.value(), index)) {
      b.add(it.value());
    }
    it.next();
    ++index;
  }
  b.close();
  return b;
}
        
Builder Collection::map (Slice const& slice, std::function<Value(Slice const&, ValueLength)> const& cb) {
  // construct a new Array
  Builder b;
  b.add(Value(ValueType::Array));

  ArrayIterator it(slice);
  ValueLength index = 0;

  while (it.valid()) {
    Value v = cb(it.value(), index);
    b.add(v);
    it.next();
    ++index;
  }
  b.close();
  return b;
}

std::vector<std::string> Collection::keys (Slice const& slice) {
  std::vector<std::string> result;

  keys(slice, result);
  
  return result;
}
        
void Collection::keys (Slice const& slice, std::vector<std::string>& result) {
  // pre-allocate result vector
  result.reserve(slice.length());

  ObjectIterator it(slice);

  while (it.valid()) {
    result.emplace_back(it.key().copyString());
    it.next();
  }
}
        
void Collection::keys (Slice const& slice, std::unordered_set<std::string>& result) {
  ObjectIterator it(slice);

  while (it.valid()) {
    result.emplace(it.key().copyString());
    it.next();
  }
}
