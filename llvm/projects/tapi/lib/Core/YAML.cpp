//===- lib/Core/YAML.cpp - Common YAML Mappings------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implements common YAML mappings
///
//===----------------------------------------------------------------------===//

#include "tapi/Core/YAML.h"

namespace llvm {
namespace yaml {

using tapi::ObjCConstraint;
void ScalarEnumerationTraits<ObjCConstraint>::enumeration(
    IO &io, ObjCConstraint &constraint) {
  io.enumCase(constraint, "none", ObjCConstraint::None);
  io.enumCase(constraint, "retain_release", ObjCConstraint::Retain_Release);
  io.enumCase(constraint, "retain_release_for_simulator",
              ObjCConstraint::Retain_Release_For_Simulator);
  io.enumCase(constraint, "retain_release_or_gc",
              ObjCConstraint::Retain_Release_Or_GC);
  io.enumCase(constraint, "gc", ObjCConstraint::GC);
}

using TAPI_INTERNAL::Platform;
void ScalarEnumerationTraits<Platform>::enumeration(IO &io,
                                                    Platform &platform) {
  io.enumCase(platform, "unknown", Platform::unknown);
  io.enumCase(platform, "macosx", Platform::macOS);
  io.enumCase(platform, "ios", Platform::iOS);
  io.enumCase(platform, "ios", Platform::iOSSimulator);

  io.enumCase(platform, "watchos", Platform::watchOS);
  io.enumCase(platform, "watchos", Platform::watchOSSimulator);
  io.enumCase(platform, "tvos", Platform::tvOS);
  io.enumCase(platform, "tvos", Platform::tvOSSimulator);
  io.enumCase(platform, "bridgeos", Platform::bridgeOS);

  io.enumCase(platform, "iosmac", Platform::zippered);
  io.enumCase(platform, "zippered", Platform::zippered);
}

using TAPI_INTERNAL::Architecture;
using TAPI_INTERNAL::ArchitectureSet;
void ScalarBitSetTraits<ArchitectureSet>::bitset(IO &io,
                                                 ArchitectureSet &archs) {
#define ARCHINFO(arch, type, subtype)                                          \
  io.bitSetCase(archs, #arch, 1U << static_cast<int>(Architecture::arch));
#include "tapi/Core/Architecture.def"
#undef ARCHINFO
}

using TAPI_INTERNAL::getArchType;
void ScalarTraits<Architecture>::output(const Architecture &value, void *,
                                        raw_ostream &os) {
  os << value;
}
StringRef ScalarTraits<Architecture>::input(StringRef scalar, void *,
                                            Architecture &value) {
  value = getArchType(scalar);
  return {};
}
QuotingType ScalarTraits<Architecture>::mustQuote(StringRef) {
  return QuotingType::None;
}

using TAPI_INTERNAL::PackedVersion;
void ScalarTraits<PackedVersion>::output(const PackedVersion &value, void *,
                                         raw_ostream &os) {
  os << value;
}
StringRef ScalarTraits<PackedVersion>::input(StringRef scalar, void *,
                                             PackedVersion &value) {
  if (!value.parse32(scalar))
    return "invalid packed version string.";
  return {};
}
QuotingType ScalarTraits<PackedVersion>::mustQuote(StringRef) {
  return QuotingType::None;
}

using TAPI_INTERNAL::AvailabilityInfo;
void ScalarTraits<AvailabilityInfo>::output(const AvailabilityInfo &value,
                                            void *, raw_ostream &os) {
  if (value._unavailable) {
    os << "n/a";
    return;
  }

  os << value._introduced;
  if (!value._obsoleted.empty())
    os << ".." << value._obsoleted;
}
StringRef ScalarTraits<AvailabilityInfo>::input(StringRef scalar, void *,
                                                AvailabilityInfo &value) {
  if (scalar == "n/a") {
    value._unavailable = true;
    return {};
  }

  auto split = scalar.split("..");
  auto introduced = split.first.trim();
  auto obsoleted = split.second.trim();

  if (!value._introduced.parse32(introduced))
    return "invalid packed version string.";

  if (obsoleted.empty())
    return StringRef();

  if (!value._obsoleted.parse32(obsoleted))
    return "invalid packed version string.";

  return StringRef();
}
QuotingType ScalarTraits<AvailabilityInfo>::mustQuote(StringRef) {
  return QuotingType::None;
}

void ScalarTraits<UUID>::output(const UUID &value, void *, raw_ostream &os) {
  os << value.first << ": " << value.second;
}
StringRef ScalarTraits<UUID>::input(StringRef scalar, void *, UUID &value) {
  auto split = scalar.split(':');
  auto arch = split.first.trim();
  auto uuid = split.second.trim();
  if (uuid.empty())
    return "invalid uuid string pair";
  value.first = getArchType(arch);
  value.second = uuid;
  return {};
}
QuotingType ScalarTraits<UUID>::mustQuote(StringRef) {
  return QuotingType::Single;
}

using namespace clang;
void ScalarEnumerationTraits<Language>::enumeration(
    IO &io, Language &kind) {
  io.enumCase(kind, "c", Language::C);
  io.enumCase(kind, "cxx", Language::CXX);
  io.enumCase(kind, "objective-c", Language::ObjC);
  io.enumCase(kind, "objc", Language::ObjC); // to keep old snapshots working.
  io.enumCase(kind, "objective-cxx", Language::ObjCXX);
  io.enumCase(kind, "objcxx",
              Language::ObjCXX); // to keep old snapshots working.
}

} // end namespace yaml.
} // end namespace llvm.
