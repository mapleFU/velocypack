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

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include "velocypack/velocypack-common.h"
#include "velocypack/Slice.h"
#include "velocypack/Value.h"
#include "velocypack/ValueType.h"

namespace arangodb::velocypack {

struct HexDump {
  HexDump() = delete;

  HexDump(Slice slice, int valuesPerLine = 16,
          std::string const& separator = " ", std::string const& header = "0x")
      : data(slice.start()),
        length(slice.byteSize()),
        valuesPerLine(valuesPerLine),
        separator(separator),
        header(header) {}

  [[deprecated]] HexDump(Slice const* slice, int valuesPerLine = 16,
                         std::string const& separator = " ", std::string const& header = "0x")
      : HexDump(*slice, valuesPerLine, separator, header) {}

  HexDump(uint8_t const* data, ValueLength length, int valuesPerLine = 16,
          std::string const& separator = " ", std::string const& header = "0x")
      : data(data),
        length(length),
        valuesPerLine(valuesPerLine),
        separator(separator),
        header(header) {}

  static std::string toHex(uint8_t value, std::string_view header = "0x");
  static void appendHex(std::string& result, uint8_t value);
  std::string toString() const;

  friend std::ostream& operator<<(std::ostream&, HexDump const&);

  uint8_t const* data;
  ValueLength length;
  int valuesPerLine;
  std::string separator;
  std::string header;
};

}  // namespace arangodb::velocypack

using VPackHexDump = arangodb::velocypack::HexDump;
