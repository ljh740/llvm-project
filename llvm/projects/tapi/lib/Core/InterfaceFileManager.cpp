//===- InterfaceFileManager.cpp - TAPI Interface File Manager ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implements the TAPI Interface File Manager.
///
//===----------------------------------------------------------------------===//

#include "tapi/Core/InterfaceFileManager.h"
#include "tapi/Core/FileManager.h"
#include "tapi/Core/Registry.h"
#include "tapi/Defines.h"
#include "llvm/Support/Error.h"

using namespace llvm;

TAPI_NAMESPACE_INTERNAL_BEGIN

InterfaceFileManager::InterfaceFileManager(FileManager &fm) : _fm(fm) {
  _registry.addYAMLReaders();
  _registry.addYAMLWriters();
  _registry.addBinaryReaders();
}

Expected<InterfaceFileBase *>
InterfaceFileManager::readFile(const std::string &path) {
  auto fileOrErr = _fm.getFile(path);
  if (!fileOrErr)
    return errorCodeToError(fileOrErr.getError());

  auto bufferOrErr = _fm.getBufferForFile(fileOrErr.get());
  if (!bufferOrErr)
    return errorCodeToError(bufferOrErr.getError());

  auto file2 =
      _registry.readFile(std::move(bufferOrErr.get()), ReadFlags::Symbols);
  if (!file2)
    return file2.takeError();

  auto *interface = cast<InterfaceFileBase>(file2.get().get());
  auto it = _libraries.find(interface->getInstallName());
  if (it != _libraries.end())
    return it->second.get();

  file2.get().release();
  _libraries.emplace(interface->getInstallName(),
                     std::unique_ptr<InterfaceFileBase>(interface));

  return interface;
}

Error InterfaceFileManager::writeFile(const InterfaceFileBase *file,
                                      const std::string &path) const {
  return _registry.writeFile(file, path);
}

TAPI_NAMESPACE_INTERNAL_END
