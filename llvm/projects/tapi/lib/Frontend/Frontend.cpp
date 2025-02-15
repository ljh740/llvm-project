//===- lib/Frontend/Frontend.cpp - TAPI Frontend ----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implements the TAPI Frontend
///
//===----------------------------------------------------------------------===//

#include "tapi/Frontend/Frontend.h"
#include "APIVisitor.h"
#include "tapi/Defines.h"
#include "tapi/Frontend/FrontendContext.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/Tool.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Path.h"

using namespace llvm;
using namespace clang;

TAPI_NAMESPACE_INTERNAL_BEGIN

static StringRef getLanguageOptions(clang::Language lang) {
  switch (lang) {
  default:
    return "";
  case clang::Language::C:
    return "-xc";
  case clang::Language::CXX:
    return "-xc++";
  case clang::Language::ObjC:
    return "-xobjective-c";
  case clang::Language::ObjCXX:
    return "-xobjective-c++";
  }
}

static StringRef getFileExtension(clang::Language lang) {
  switch (lang) {
  default:
    llvm_unreachable("Unexpected language option.");
  case clang::Language::C:
    return ".c";
  case clang::Language::CXX:
    return ".cpp";
  case clang::Language::ObjC:
    return ".m";
  case clang::Language::ObjCXX:
    return ".mm";
  }
}

static SmallVectorImpl<char> &operator+=(SmallVectorImpl<char> &includes,
                                         StringRef rhs) {
  includes.append(rhs.begin(), rhs.end());
  return includes;
}

static void addHeaderInclude(StringRef headerName,
                             clang::Language lang,
                             SmallVectorImpl<char> &includes) {
  SmallString<PATH_MAX> name;
  if (!(headerName.startswith("\"") && headerName.endswith("\"")) &&
      !(headerName.startswith("<") && headerName.endswith(">"))) {
    name += "\"";
    name += headerName;
    name += "\"";
  } else
    name += headerName;

  if (lang == clang::Language::C || lang == clang::Language::CXX)
    includes += "#include ";
  else
    includes += "#import ";
  includes += name;
  includes += "\n";
}

static const opt::ArgStringList *
getCC1Arguments(DiagnosticsEngine *diagnostics,
                driver::Compilation *compilation) {
  const auto &jobs = compilation->getJobs();
  if (jobs.size() != 1 || !isa<driver::Command>(*jobs.begin())) {
    SmallString<256> error_msg;
    raw_svector_ostream error_stream(error_msg);
    jobs.Print(error_stream, "; ", true);
    diagnostics->Report(diag::err_fe_expected_compiler_job)
        << error_stream.str();
    return nullptr;
  }

  // The one job we find should be to invoke clang again.
  const auto &cmd = cast<driver::Command>(*jobs.begin());
  if (StringRef(cmd.getCreator().getName()) != "clang") {
    diagnostics->Report(diag::err_fe_expected_clang_command);
    return nullptr;
  }

  return &cmd.getArguments();
}

CompilerInvocation *newInvocation(DiagnosticsEngine *diagnostics,
                                  const opt::ArgStringList &cc1Args) {
  assert(!cc1Args.empty() && "Must at least contain the program name!");
  CompilerInvocation *invocation = new CompilerInvocation;
  CompilerInvocation::CreateFromArgs(*invocation, cc1Args, *diagnostics);
  invocation->getFrontendOpts().DisableFree = false;
  invocation->getCodeGenOpts().DisableFree = false;
  return invocation;
}

static bool runClang(FrontendContext &context, ArrayRef<std::string> options,
                     std::unique_ptr<llvm::MemoryBuffer> input) {
  context.compiler = std::make_unique<CompilerInstance>();
  IntrusiveRefCntPtr<DiagnosticIDs> diagID(new DiagnosticIDs());
  IntrusiveRefCntPtr<DiagnosticOptions> diagOpts(new DiagnosticOptions());
  const llvm::opt::OptTable *opts = &driver::getDriverOptTable();

  std::vector<const char *> argv;
  for (const std::string &str : options)
    argv.push_back(str.c_str());
  const char *const binaryName = argv[0];

  unsigned MissingArgIndex, MissingArgCount;
  llvm::opt::InputArgList parsedArgs = opts->ParseArgs(
      ArrayRef<const char *>(argv).slice(1), MissingArgIndex, MissingArgCount);
  ParseDiagnosticArgs(*diagOpts, parsedArgs);
  TextDiagnosticPrinter diagnosticPrinter(llvm::errs(), &*diagOpts);
  DiagnosticsEngine diagnosticsEngine(diagID, &*diagOpts, &diagnosticPrinter,
                                      false);

  IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS(&(context.fileManager->getVirtualFileSystem()));
  const std::unique_ptr<clang::driver::Driver> driver(new clang::driver::Driver(
      binaryName, llvm::sys::getDefaultTargetTriple(), diagnosticsEngine,
      VFS));
  driver->setTitle("tapi");
  // Since the input might only be virtual, don't check whether it exists.
  driver->setCheckInputsExist(false);
  const std::unique_ptr<clang::driver::Compilation> compilation(
      driver->BuildCompilation(llvm::makeArrayRef(argv)));
  if (!compilation)
    return false;
  const llvm::opt::ArgStringList *const cc1Args =
      getCC1Arguments(&diagnosticsEngine, compilation.get());
  if (!cc1Args)
    return false;

  std::unique_ptr<clang::CompilerInvocation> invocation(
      newInvocation(&diagnosticsEngine, *cc1Args));

  // Show the invocation, with -v.
  if (invocation->getHeaderSearchOpts().Verbose) {
    llvm::errs() << "clang Invocation:\n";
    compilation->getJobs().Print(llvm::errs(), "\n", true);
    llvm::errs() << "\n";
  }

  if (input)
    invocation->getPreprocessorOpts().addRemappedFile(
        input->getBufferIdentifier(), input.release());

  // Create a compiler instance to handle the actual work.
  context.compiler->setInvocation(std::move(invocation));
  context.compiler->setFileManager(&*(context.fileManager));
  auto action = std::make_unique<APIVisitorAction>(context);

  // Create the compiler's actual diagnostics engine.
  context.compiler->createDiagnostics();
  if (!context.compiler->hasDiagnostics())
    return false;

  context.compiler->createSourceManager(*(context.fileManager));

  return context.compiler->ExecuteAction(*action);
}

extern Optional<FrontendContext> runFrontend(const FrontendJob &job,
                                             StringRef inputFilename) {
  FrontendContext context(job.workingDirectory, job.cacheFactory, job.vfs);
  context.target = job.target;

  std::unique_ptr<llvm::MemoryBuffer> input;
  std::string inputFilePath;
  if (inputFilename.empty()) {
    SmallString<4096> headerContents;
    for (const auto &header : job.headerFiles) {
      if (header.isExcluded)
        continue;

      if (header.type != job.type)
        continue;

      addHeaderInclude(header.includeName.empty() ? header.fullPath
                                                  : header.includeName,
                       job.language, headerContents);

      auto fileOrError = context.fileManager->getFile(header.fullPath);
      if (fileOrError) {
          const auto *file = fileOrError.get();
          context.files.emplace(file, header.type);
      }
    }

    inputFilePath =
        ("tapi_include_headers" + getFileExtension(job.language)).str();
    input = llvm::MemoryBuffer::getMemBufferCopy(headerContents, inputFilePath);
  } else {
    inputFilePath = inputFilename;
    auto fileOrErr = context.fileManager->getFile(inputFilename);
    if (fileOrErr) {
        const auto *file = fileOrErr.get();
        context.files.emplace(file, HeaderType::Public);
    }
  }

  std::vector<std::string> args;
  args.emplace_back("tapi");
  args.emplace_back("-fsyntax-only");
  args.emplace_back(getLanguageOptions(job.language));
  args.emplace_back("-target");
  args.emplace_back(job.target.str());

  if (!job.clangResourcePath.empty()) {
    args.emplace_back("-resource-dir");
    args.emplace_back(job.clangResourcePath);
  }

  if (!job.language_std.empty())
    args.emplace_back("-std=" + job.language_std);

  if (!job.useRTTI)
    args.emplace_back("-fno-rtti");

  if (!job.visibility.empty())
    args.emplace_back("-fvisibility=" + job.visibility);

  if (job.enableModules)
    args.emplace_back("-fmodules");

  if (!job.moduleCachePath.empty())
    args.emplace_back("-fmodules-cache-path=" + job.moduleCachePath);

  if (job.validateSystemHeaders)
    args.emplace_back("-fmodules-validate-system-headers");

  if (job.useObjectiveCARC)
    args.emplace_back("-fobjc-arc");

  if (job.useObjectiveCWeakARC)
    args.emplace_back("-fobjc-weak");

  // Add a default macro for TAPI.
  args.emplace_back("-D__clang_tapi__=1");

  for (auto &macro : job.macros) {
    if (macro.second)
      args.emplace_back("-U" + macro.first);
    else
      args.emplace_back("-D" + macro.first);
  }

  if (!job.isysroot.empty())
    args.emplace_back("-isysroot" + job.isysroot);

  // Add SYSTEM framework search paths.
  for (const auto &path : job.systemFrameworkPaths)
    args.emplace_back("-iframework" + path);

  // Add SYSTEM header search paths.
  for (const auto &path : job.systemIncludePaths)
    args.emplace_back("-isystem" + path);

  // Add the framework search paths.
  for (const auto &path : job.frameworkPaths)
    args.emplace_back("-F" + path);

  // Add the header search paths.
  for (const auto &path : job.includePaths)
    args.emplace_back("-I" + path);

  // Also add the private framework path, since it is not added by default.
  if (job.isysroot.empty())
    args.emplace_back("-iframework /System/Library/PrivateFrameworks");
  else {
    SmallString<PATH_MAX> path(job.isysroot);
    sys::path::append(path, "/System/Library/PrivateFrameworks");
    std::string tmp("-iframework");
    tmp += path.str();
    args.emplace_back(tmp);
  }

  // For c++ and objective-c++, add default stdlib to be libc++.
  if (job.language == clang::Language::CXX ||
      job.language == clang::Language::ObjCXX)
    args.emplace_back("-stdlib=libc++");

  // Add extra clang arguments.
  for (const auto &arg : job.clangExtraArgs)
    args.emplace_back(arg);

  args.emplace_back(inputFilePath);
  if (!runClang(context, args, std::move(input)))
    return llvm::None;
  return context;
}

TAPI_NAMESPACE_INTERNAL_END
