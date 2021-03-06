#pragma once

#include "common/protobuf/protobuf.h"

namespace Envoy {
namespace Config {

class VersionConverter {
public:
  /**
   * Upgrade a message from an earlier to later version of the Envoy API. This
   * uses reflection techniques to perform the vN -> v(N+1) protobuf C++ type
   * lifting which is the moral equivalent of what protoxform does on a
   * migration. That is, most fields are by default unchanged in name, type,
   * etc. and so can be trivially copied and then a series of mechanical
   * transforms allows for a stylized, information preserving upgrade via
   * reflection. For example, the UnknownFieldSet in next_message generated by
   * protoxform is used to provide access to deprecated fields in the previous
   * API version.
   *
   * @param prev_message previous version message input.
   * @param next_nmessage next version message to generate.
   */
  static void upgrade(const Protobuf::Message& prev_message, Protobuf::Message& next_message);

  /**
   * Access the deprecated fields stored in the UnknownFieldSet of a message
   * generated via VersionConverter::upgrade().
   *
   * @param upggraded_message the result of VersionConverter::upgrade().
   * @param deprecated_message a copy of the deprecated fields.
   */
  static void unpackDeprecated(const Protobuf::Message& upgraded_message,
                               Protobuf::Message& deprecated_message);
};

} // namespace Config
} // namespace Envoy
