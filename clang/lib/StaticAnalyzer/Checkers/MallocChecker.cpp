//=== MallocChecker.cpp - A malloc/free checker -------------------*- C++ -*--//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a variety of memory management related checkers, such as
// leak, double free, and use-after-free.
//
// The following checkers are defined here:
//
//   * MallocChecker
//       Despite its name, it models all sorts of memory allocations and
//       de- or reallocation, including but not limited to malloc, free,
//       relloc, new, delete. It also reports on a variety of memory misuse
//       errors.
//       Many other checkers interact very closely with this checker, in fact,
//       most are merely options to this one. Other checkers may register
//       MallocChecker, but do not enable MallocChecker's reports (more details
//       to follow around its field, ChecksEnabled).
//       It also has a boolean "Optimistic" checker option, which if set to true
//       will cause the checker to model user defined memory management related
//       functions annotated via the attribute ownership_takes, ownership_holds
//       and ownership_returns.
//
//   * NewDeleteChecker
//       Enables the modeling of new, new[], delete, delete[] in MallocChecker,
//       and checks for related double-free and use-after-free errors.
//
//   * NewDeleteLeaksChecker
//       Checks for leaks related to new, new[], delete, delete[].
//       Depends on NewDeleteChecker.
//
//   * MismatchedDeallocatorChecker
//       Enables checking whether memory is deallocated with the correspending
//       allocation function in MallocChecker, such as malloc() allocated
//       regions are only freed by free(), new by delete, new[] by delete[].
//
//  InnerPointerChecker interacts very closely with MallocChecker, but unlike
//  the above checkers, it has it's own file, hence the many InnerPointerChecker
//  related headers and non-static functions.
//
//===----------------------------------------------------------------------===//

#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "InterCheckerAPI.h"
#include "clang/AST/Attr.h"
#include "clang/AST/ParentMap.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Lexer.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/BugReporter/CommonBugCategories.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SymbolManager.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "AllocationState.h"
#include <climits>
#include <utility>

using namespace clang;
using namespace ento;

//===----------------------------------------------------------------------===//
// The types of allocation we're modeling.
//===----------------------------------------------------------------------===//

namespace {

// Used to check correspondence between allocators and deallocators.
enum AllocationFamily {
  AF_None,
  AF_Malloc,
  AF_CXXNew,
  AF_CXXNewArray,
  AF_IfNameIndex,
  AF_Alloca,
  AF_InnerBuffer
};

struct MemFunctionInfoTy;

} // end of anonymous namespace

/// Determine family of a deallocation expression.
static AllocationFamily
getAllocationFamily(const MemFunctionInfoTy &MemFunctionInfo, CheckerContext &C,
                    const Stmt *S);

/// Print names of allocators and deallocators.
///
/// \returns true on success.
static bool printAllocDeallocName(raw_ostream &os, CheckerContext &C,
                                  const Expr *E);

/// Print expected name of an allocator based on the deallocator's
/// family derived from the DeallocExpr.
static void printExpectedAllocName(raw_ostream &os,
                                   const MemFunctionInfoTy &MemFunctionInfo,
                                   CheckerContext &C, const Expr *E);

/// Print expected name of a deallocator based on the allocator's
/// family.
static void printExpectedDeallocName(raw_ostream &os, AllocationFamily Family);

//===----------------------------------------------------------------------===//
// The state of a symbol, in terms of memory management.
//===----------------------------------------------------------------------===//

namespace {

class RefState {
  enum Kind {
    // Reference to allocated memory.
    Allocated,
    // Reference to zero-allocated memory.
    AllocatedOfSizeZero,
    // Reference to released/freed memory.
    Released,
    // The responsibility for freeing resources has transferred from
    // this reference. A relinquished symbol should not be freed.
    Relinquished,
    // We are no longer guaranteed to have observed all manipulations
    // of this pointer/memory. For example, it could have been
    // passed as a parameter to an opaque function.
    Escaped
  };

  const Stmt *S;

  Kind K;
  AllocationFamily Family;

  RefState(Kind k, const Stmt *s, AllocationFamily family)
      : S(s), K(k), Family(family) {
    assert(family != AF_None);
  }

public:
  bool isAllocated() const { return K == Allocated; }
  bool isAllocatedOfSizeZero() const { return K == AllocatedOfSizeZero; }
  bool isReleased() const { return K == Released; }
  bool isRelinquished() const { return K == Relinquished; }
  bool isEscaped() const { return K == Escaped; }
  AllocationFamily getAllocationFamily() const { return Family; }
  const Stmt *getStmt() const { return S; }

  bool operator==(const RefState &X) const {
    return K == X.K && S == X.S && Family == X.Family;
  }

  static RefState getAllocated(AllocationFamily family, const Stmt *s) {
    return RefState(Allocated, s, family);
  }
  static RefState getAllocatedOfSizeZero(const RefState *RS) {
    return RefState(AllocatedOfSizeZero, RS->getStmt(),
                    RS->getAllocationFamily());
  }
  static RefState getReleased(AllocationFamily family, const Stmt *s) {
    return RefState(Released, s, family);
  }
  static RefState getRelinquished(AllocationFamily family, const Stmt *s) {
    return RefState(Relinquished, s, family);
  }
  static RefState getEscaped(const RefState *RS) {
    return RefState(Escaped, RS->getStmt(), RS->getAllocationFamily());
  }

  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(K);
    ID.AddPointer(S);
    ID.AddInteger(Family);
  }

  LLVM_DUMP_METHOD void dump(raw_ostream &OS) const {
    switch (K) {
#define CASE(ID) case ID: OS << #ID; break;
    CASE(Allocated)
    CASE(AllocatedOfSizeZero)
    CASE(Released)
    CASE(Relinquished)
    CASE(Escaped)
    }
  }

  LLVM_DUMP_METHOD void dump() const { dump(llvm::errs()); }
};

} // end of anonymous namespace

REGISTER_MAP_WITH_PROGRAMSTATE(RegionState, SymbolRef, RefState)

/// Check if the memory associated with this symbol was released.
static bool isReleased(SymbolRef Sym, CheckerContext &C);

/// Update the RefState to reflect the new memory allocation.
/// The optional \p RetVal parameter specifies the newly allocated pointer
/// value; if unspecified, the value of expression \p E is used.
static ProgramStateRef MallocUpdateRefState(CheckerContext &C, const Expr *E,
                                            ProgramStateRef State,
                                            AllocationFamily Family = AF_Malloc,
                                            Optional<SVal> RetVal = None);

//===----------------------------------------------------------------------===//
// The modeling of memory reallocation.
//
// The terminology 'toPtr' and 'fromPtr' will be used:
//   toPtr = realloc(fromPtr, 20);
//===----------------------------------------------------------------------===//

REGISTER_SET_WITH_PROGRAMSTATE(ReallocSizeZeroSymbols, SymbolRef)

namespace {

/// The state of 'fromPtr' after reallocation is known to have failed.
enum OwnershipAfterReallocKind {
  // The symbol needs to be freed (e.g.: realloc)
  OAR_ToBeFreedAfterFailure,
  // The symbol has been freed (e.g.: reallocf)
  OAR_FreeOnFailure,
  // The symbol doesn't have to freed (e.g.: we aren't sure if, how and where
  // 'fromPtr' was allocated:
  //    void Haha(int *ptr) {
  //      ptr = realloc(ptr, 67);
  //      // ...
  //    }
  // ).
  OAR_DoNotTrackAfterFailure
};

/// Stores information about the 'fromPtr' symbol after reallocation.
///
/// This is important because realloc may fail, and that needs special modeling.
/// Whether reallocation failed or not will not be known until later, so we'll
/// store whether upon failure 'fromPtr' will be freed, or needs to be freed
/// later, etc.
struct ReallocPair {

  // The 'fromPtr'.
  SymbolRef ReallocatedSym;
  OwnershipAfterReallocKind Kind;

  ReallocPair(SymbolRef S, OwnershipAfterReallocKind K)
      : ReallocatedSym(S), Kind(K) {}
  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(Kind);
    ID.AddPointer(ReallocatedSym);
  }
  bool operator==(const ReallocPair &X) const {
    return ReallocatedSym == X.ReallocatedSym &&
           Kind == X.Kind;
  }
};

} // end of anonymous namespace

REGISTER_MAP_WITH_PROGRAMSTATE(ReallocPairs, SymbolRef, ReallocPair)

//===----------------------------------------------------------------------===//
// Kinds of memory operations, information about resource managing functions.
//===----------------------------------------------------------------------===//

namespace {

enum class MemoryOperationKind { MOK_Allocate, MOK_Free, MOK_Any };

struct MemFunctionInfoTy {
  /// The value of the MallocChecker:Optimistic is stored in this variable.
  ///
  /// In pessimistic mode, the checker assumes that it does not know which
  /// functions might free the memory.
  /// In optimistic mode, the checker assumes that all user-defined functions
  /// which might free a pointer are annotated.
  DefaultBool ShouldIncludeOwnershipAnnotatedFunctions;

  // TODO: Change these to CallDescription, and get rid of lazy initialization.
  mutable IdentifierInfo *II_alloca = nullptr, *II_win_alloca = nullptr,
                         *II_malloc = nullptr, *II_free = nullptr,
                         *II_realloc = nullptr, *II_calloc = nullptr,
                         *II_valloc = nullptr, *II_reallocf = nullptr,
                         *II_strndup = nullptr, *II_strdup = nullptr,
                         *II_win_strdup = nullptr, *II_kmalloc = nullptr,
                         *II_if_nameindex = nullptr,
                         *II_if_freenameindex = nullptr, *II_wcsdup = nullptr,
                         *II_win_wcsdup = nullptr, *II_g_malloc = nullptr,
                         *II_g_malloc0 = nullptr, *II_g_realloc = nullptr,
                         *II_g_try_malloc = nullptr,
                         *II_g_try_malloc0 = nullptr,
                         *II_g_try_realloc = nullptr, *II_g_free = nullptr,
                         *II_g_memdup = nullptr, *II_g_malloc_n = nullptr,
                         *II_g_malloc0_n = nullptr, *II_g_realloc_n = nullptr,
                         *II_g_try_malloc_n = nullptr,
                         *II_g_try_malloc0_n = nullptr, *II_kfree = nullptr,
                         *II_g_try_realloc_n = nullptr;

  void initIdentifierInfo(ASTContext &C) const;

  ///@{
  /// Check if this is one of the functions which can allocate/reallocate
  /// memory pointed to by one of its arguments.
  bool isMemFunction(const FunctionDecl *FD, ASTContext &C) const;
  bool isCMemFunction(const FunctionDecl *FD, ASTContext &C,
                      AllocationFamily Family,
                      MemoryOperationKind MemKind) const;

  /// Tells if the callee is one of the builtin new/delete operators, including
  /// placement operators and other standard overloads.
  bool isStandardNewDelete(const FunctionDecl *FD, ASTContext &C) const;
  ///@}
};

} // end of anonymous namespace

//===----------------------------------------------------------------------===//
// Definition of the MallocChecker class.
//===----------------------------------------------------------------------===//

namespace {

class MallocChecker
    : public Checker<check::DeadSymbols, check::PointerEscape,
                     check::ConstPointerEscape, check::PreStmt<ReturnStmt>,
                     check::EndFunction, check::PreCall,
                     check::PostStmt<CallExpr>, check::PostStmt<CXXNewExpr>,
                     check::NewAllocator, check::PreStmt<CXXDeleteExpr>,
                     check::PostStmt<BlockExpr>, check::PostObjCMessage,
                     check::Location, eval::Assume> {
public:
  MemFunctionInfoTy MemFunctionInfo;

  /// Many checkers are essentially built into this one, so enabling them will
  /// make MallocChecker perform additional modeling and reporting.
  enum CheckKind {
    /// When a subchecker is enabled but MallocChecker isn't, model memory
    /// management but do not emit warnings emitted with MallocChecker only
    /// enabled.
    CK_MallocChecker,
    CK_NewDeleteChecker,
    CK_NewDeleteLeaksChecker,
    CK_MismatchedDeallocatorChecker,
    CK_InnerPointerChecker,
    CK_NumCheckKinds
  };

  using LeakInfo = std::pair<const ExplodedNode *, const MemRegion *>;

  DefaultBool ChecksEnabled[CK_NumCheckKinds];
  CheckerNameRef CheckNames[CK_NumCheckKinds];

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const;
  void checkPostStmt(const CallExpr *CE, CheckerContext &C) const;
  void checkPostStmt(const CXXNewExpr *NE, CheckerContext &C) const;
  void checkNewAllocator(const CXXNewExpr *NE, SVal Target,
                         CheckerContext &C) const;
  void checkPreStmt(const CXXDeleteExpr *DE, CheckerContext &C) const;
  void checkPostObjCMessage(const ObjCMethodCall &Call, CheckerContext &C) const;
  void checkPostStmt(const BlockExpr *BE, CheckerContext &C) const;
  void checkDeadSymbols(SymbolReaper &SymReaper, CheckerContext &C) const;
  void checkPreStmt(const ReturnStmt *S, CheckerContext &C) const;
  void checkEndFunction(const ReturnStmt *S, CheckerContext &C) const;
  ProgramStateRef evalAssume(ProgramStateRef state, SVal Cond,
                            bool Assumption) const;
  void checkLocation(SVal l, bool isLoad, const Stmt *S,
                     CheckerContext &C) const;

  ProgramStateRef checkPointerEscape(ProgramStateRef State,
                                    const InvalidatedSymbols &Escaped,
                                    const CallEvent *Call,
                                    PointerEscapeKind Kind) const;
  ProgramStateRef checkConstPointerEscape(ProgramStateRef State,
                                          const InvalidatedSymbols &Escaped,
                                          const CallEvent *Call,
                                          PointerEscapeKind Kind) const;

  void printState(raw_ostream &Out, ProgramStateRef State,
                  const char *NL, const char *Sep) const override;

private:
  mutable std::unique_ptr<BugType> BT_DoubleFree[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_DoubleDelete;
  mutable std::unique_ptr<BugType> BT_Leak[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_UseFree[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_BadFree[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_FreeAlloca[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_MismatchedDealloc;
  mutable std::unique_ptr<BugType> BT_OffsetFree[CK_NumCheckKinds];
  mutable std::unique_ptr<BugType> BT_UseZerroAllocated[CK_NumCheckKinds];

  // TODO: Remove mutable by moving the initializtaion to the registry function.
  mutable Optional<uint64_t> KernelZeroFlagVal;

  /// Process C++ operator new()'s allocation, which is the part of C++
  /// new-expression that goes before the constructor.
  void processNewAllocation(const CXXNewExpr *NE, CheckerContext &C,
                            SVal Target) const;

  /// Perform a zero-allocation check.
  ///
  /// \param [in] E The expression that allocates memory.
  /// \param [in] IndexOfSizeArg Index of the argument that specifies the size
  ///   of the memory that needs to be allocated. E.g. for malloc, this would be
  ///   0.
  /// \param [in] RetVal Specifies the newly allocated pointer value;
  ///   if unspecified, the value of expression \p E is used.
  static ProgramStateRef ProcessZeroAllocCheck(CheckerContext &C, const Expr *E,
                                               const unsigned IndexOfSizeArg,
                                               ProgramStateRef State,
                                               Optional<SVal> RetVal = None);

  /// Model functions with the ownership_returns attribute.
  ///
  /// User-defined function may have the ownership_returns attribute, which
  /// annotates that the function returns with an object that was allocated on
  /// the heap, and passes the ownertship to the callee.
  ///
  ///   void __attribute((ownership_returns(malloc, 1))) *my_malloc(size_t);
  ///
  /// It has two parameters:
  ///   - first: name of the resource (e.g. 'malloc')
  ///   - (OPTIONAL) second: size of the allocated region
  ///
  /// \param [in] CE The expression that allocates memory.
  /// \param [in] Att The ownership_returns attribute.
  /// \param [in] State The \c ProgramState right before allocation.
  /// \returns The ProgramState right after allocation.
  ProgramStateRef MallocMemReturnsAttr(CheckerContext &C,
                                       const CallExpr *CE,
                                       const OwnershipAttr* Att,
                                       ProgramStateRef State) const;

  /// Models memory allocation.
  ///
  /// \param [in] CE The expression that allocates memory.
  /// \param [in] SizeEx Size of the memory that needs to be allocated.
  /// \param [in] Init The value the allocated memory needs to be initialized.
  /// with. For example, \c calloc initializes the allocated memory to 0,
  /// malloc leaves it undefined.
  /// \param [in] State The \c ProgramState right before allocation.
  /// \returns The ProgramState right after allocation.
  static ProgramStateRef MallocMemAux(CheckerContext &C, const CallExpr *CE,
                                      const Expr *SizeEx, SVal Init,
                                      ProgramStateRef State,
                                      AllocationFamily Family = AF_Malloc);

  /// Models memory allocation.
  ///
  /// \param [in] CE The expression that allocates memory.
  /// \param [in] Size Size of the memory that needs to be allocated.
  /// \param [in] Init The value the allocated memory needs to be initialized.
  /// with. For example, \c calloc initializes the allocated memory to 0,
  /// malloc leaves it undefined.
  /// \param [in] State The \c ProgramState right before allocation.
  /// \returns The ProgramState right after allocation.
  static ProgramStateRef MallocMemAux(CheckerContext &C, const CallExpr *CE,
                                      SVal Size, SVal Init,
                                      ProgramStateRef State,
                                      AllocationFamily Family = AF_Malloc);

  static ProgramStateRef addExtentSize(CheckerContext &C, const CXXNewExpr *NE,
                                       ProgramStateRef State, SVal Target);

  // Check if this malloc() for special flags. At present that means M_ZERO or
  // __GFP_ZERO (in which case, treat it like calloc).
  llvm::Optional<ProgramStateRef>
  performKernelMalloc(const CallExpr *CE, CheckerContext &C,
                      const ProgramStateRef &State) const;

  /// Model functions with the ownership_takes and ownership_holds attributes.
  ///
  /// User-defined function may have the ownership_takes and/or ownership_holds
  /// attributes, which annotates that the function frees the memory passed as a
  /// parameter.
  ///
  ///   void __attribute((ownership_takes(malloc, 1))) my_free(void *);
  ///   void __attribute((ownership_holds(malloc, 1))) my_hold(void *);
  ///
  /// They have two parameters:
  ///   - first: name of the resource (e.g. 'malloc')
  ///   - second: index of the parameter the attribute applies to
  ///
  /// \param [in] CE The expression that frees memory.
  /// \param [in] Att The ownership_takes or ownership_holds attribute.
  /// \param [in] State The \c ProgramState right before allocation.
  /// \returns The ProgramState right after deallocation.
  ProgramStateRef FreeMemAttr(CheckerContext &C, const CallExpr *CE,
                              const OwnershipAttr* Att,
                              ProgramStateRef State) const;

  /// Models memory deallocation.
  ///
  /// \param [in] CE The expression that frees memory.
  /// \param [in] State The \c ProgramState right before allocation.
  /// \param [in] Num Index of the argument that needs to be freed. This is
  ///   normally 0, but for custom free functions it may be different.
  /// \param [in] Hold Whether the parameter at \p Index has the ownership_holds
  ///   attribute.
  /// \param [out] IsKnownToBeAllocated Whether the memory to be freed is known
  ///   to have been allocated, or in other words, the symbol to be freed was
  ///   registered as allocated by this checker. In the following case, \c ptr
  ///   isn't known to be allocated.
  ///      void Haha(int *ptr) {
  ///        ptr = realloc(ptr, 67);
  ///        // ...
  ///      }
  /// \param [in] ReturnsNullOnFailure Whether the memory deallocation function
  ///   we're modeling returns with Null on failure.
  /// \returns The ProgramState right after deallocation.
  ProgramStateRef FreeMemAux(CheckerContext &C, const CallExpr *CE,
                             ProgramStateRef State, unsigned Num, bool Hold,
                             bool &IsKnownToBeAllocated,
                             bool ReturnsNullOnFailure = false) const;

  /// Models memory deallocation.
  ///
  /// \param [in] ArgExpr The variable who's pointee needs to be freed.
  /// \param [in] ParentExpr The expression that frees the memory.
  /// \param [in] State The \c ProgramState right before allocation.
  ///   normally 0, but for custom free functions it may be different.
  /// \param [in] Hold Whether the parameter at \p Index has the ownership_holds
  ///   attribute.
  /// \param [out] IsKnownToBeAllocated Whether the memory to be freed is known
  ///   to have been allocated, or in other words, the symbol to be freed was
  ///   registered as allocated by this checker. In the following case, \c ptr
  ///   isn't known to be allocated.
  ///      void Haha(int *ptr) {
  ///        ptr = realloc(ptr, 67);
  ///        // ...
  ///      }
  /// \param [in] ReturnsNullOnFailure Whether the memory deallocation function
  ///   we're modeling returns with Null on failure.
  /// \returns The ProgramState right after deallocation.
  ProgramStateRef FreeMemAux(CheckerContext &C, const Expr *ArgExpr,
                             const Expr *ParentExpr, ProgramStateRef State,
                             bool Hold, bool &IsKnownToBeAllocated,
                             bool ReturnsNullOnFailure = false) const;

  // TODO: Needs some refactoring, as all other deallocation modeling
  // functions are suffering from out parameters and messy code due to how
  // realloc is handled.
  //
  /// Models memory reallocation.
  ///
  /// \param [in] CE The expression that reallocated memory
  /// \param [in] ShouldFreeOnFail Whether if reallocation fails, the supplied
  ///   memory should be freed.
  /// \param [in] State The \c ProgramState right before reallocation.
  /// \param [in] SuffixWithN Whether the reallocation function we're modeling
  ///   has an '_n' suffix, such as g_realloc_n.
  /// \returns The ProgramState right after reallocation.
  ProgramStateRef ReallocMemAux(CheckerContext &C, const CallExpr *CE,
                                bool ShouldFreeOnFail, ProgramStateRef State,
                                bool SuffixWithN = false) const;

  /// Evaluates the buffer size that needs to be allocated.
  ///
  /// \param [in] Blocks The amount of blocks that needs to be allocated.
  /// \param [in] BlockBytes The size of a block.
  /// \returns The symbolic value of \p Blocks * \p BlockBytes.
  static SVal evalMulForBufferSize(CheckerContext &C, const Expr *Blocks,
                                   const Expr *BlockBytes);

  /// Models zero initialized array allocation.
  ///
  /// \param [in] CE The expression that reallocated memory
  /// \param [in] State The \c ProgramState right before reallocation.
  /// \returns The ProgramState right after allocation.
  static ProgramStateRef CallocMem(CheckerContext &C, const CallExpr *CE,
                                   ProgramStateRef State);

  /// See if deallocation happens in a suspicious context. If so, escape the
  /// pointers that otherwise would have been deallocated and return true.
  bool suppressDeallocationsInSuspiciousContexts(const CallExpr *CE,
                                                 CheckerContext &C) const;

  /// If in \p S  \p Sym is used, check whether \p Sym was already freed.
  bool checkUseAfterFree(SymbolRef Sym, CheckerContext &C, const Stmt *S) const;

  /// If in \p S \p Sym is used, check whether \p Sym was allocated as a zero
  /// sized memory region.
  void checkUseZeroAllocated(SymbolRef Sym, CheckerContext &C,
                             const Stmt *S) const;

  /// If in \p S \p Sym is being freed, check whether \p Sym was already freed.
  bool checkDoubleDelete(SymbolRef Sym, CheckerContext &C) const;

  /// Check if the function is known to free memory, or if it is
  /// "interesting" and should be modeled explicitly.
  ///
  /// \param [out] EscapingSymbol A function might not free memory in general,
  ///   but could be known to free a particular symbol. In this case, false is
  ///   returned and the single escaping symbol is returned through the out
  ///   parameter.
  ///
  /// We assume that pointers do not escape through calls to system functions
  /// not handled by this checker.
  bool mayFreeAnyEscapedMemoryOrIsModeledExplicitly(const CallEvent *Call,
                                   ProgramStateRef State,
                                   SymbolRef &EscapingSymbol) const;

  /// Implementation of the checkPointerEscape callbacks.
  ProgramStateRef checkPointerEscapeAux(ProgramStateRef State,
                                        const InvalidatedSymbols &Escaped,
                                        const CallEvent *Call,
                                        PointerEscapeKind Kind,
                                        bool IsConstPointerEscape) const;

  // Implementation of the checkPreStmt and checkEndFunction callbacks.
  void checkEscapeOnReturn(const ReturnStmt *S, CheckerContext &C) const;

  ///@{
  /// Tells if a given family/call/symbol is tracked by the current checker.
  /// Sets CheckKind to the kind of the checker responsible for this
  /// family/call/symbol.
  Optional<CheckKind> getCheckIfTracked(AllocationFamily Family,
                                        bool IsALeakCheck = false) const;
  Optional<CheckKind> getCheckIfTracked(CheckerContext &C,
                                        const Stmt *AllocDeallocStmt,
                                        bool IsALeakCheck = false) const;
  Optional<CheckKind> getCheckIfTracked(CheckerContext &C, SymbolRef Sym,
                                        bool IsALeakCheck = false) const;
  ///@}
  static bool SummarizeValue(raw_ostream &os, SVal V);
  static bool SummarizeRegion(raw_ostream &os, const MemRegion *MR);

  void ReportBadFree(CheckerContext &C, SVal ArgVal, SourceRange Range,
                     const Expr *DeallocExpr) const;
  void ReportFreeAlloca(CheckerContext &C, SVal ArgVal,
                        SourceRange Range) const;
  void ReportMismatchedDealloc(CheckerContext &C, SourceRange Range,
                               const Expr *DeallocExpr, const RefState *RS,
                               SymbolRef Sym, bool OwnershipTransferred) const;
  void ReportOffsetFree(CheckerContext &C, SVal ArgVal, SourceRange Range,
                        const Expr *DeallocExpr,
                        const Expr *AllocExpr = nullptr) const;
  void ReportUseAfterFree(CheckerContext &C, SourceRange Range,
                          SymbolRef Sym) const;
  void ReportDoubleFree(CheckerContext &C, SourceRange Range, bool Released,
                        SymbolRef Sym, SymbolRef PrevSym) const;

  void ReportDoubleDelete(CheckerContext &C, SymbolRef Sym) const;

  void ReportUseZeroAllocated(CheckerContext &C, SourceRange Range,
                              SymbolRef Sym) const;

  void ReportFunctionPointerFree(CheckerContext &C, SVal ArgVal,
                                 SourceRange Range, const Expr *FreeExpr) const;

  /// Find the location of the allocation for Sym on the path leading to the
  /// exploded node N.
  static LeakInfo getAllocationSite(const ExplodedNode *N, SymbolRef Sym,
                                    CheckerContext &C);

  void reportLeak(SymbolRef Sym, ExplodedNode *N, CheckerContext &C) const;
};

//===----------------------------------------------------------------------===//
// Definition of MallocBugVisitor.
//===----------------------------------------------------------------------===//

/// The bug visitor which allows us to print extra diagnostics along the
/// BugReport path. For example, showing the allocation site of the leaked
/// region.
class MallocBugVisitor final : public BugReporterVisitor {
protected:
  enum NotificationMode { Normal, ReallocationFailed };

  // The allocated region symbol tracked by the main analysis.
  SymbolRef Sym;

  // The mode we are in, i.e. what kind of diagnostics will be emitted.
  NotificationMode Mode;

  // A symbol from when the primary region should have been reallocated.
  SymbolRef FailedReallocSymbol;

  // A C++ destructor stack frame in which memory was released. Used for
  // miscellaneous false positive suppression.
  const StackFrameContext *ReleaseDestructorLC;

  bool IsLeak;

public:
  MallocBugVisitor(SymbolRef S, bool isLeak = false)
      : Sym(S), Mode(Normal), FailedReallocSymbol(nullptr),
        ReleaseDestructorLC(nullptr), IsLeak(isLeak) {}

  static void *getTag() {
    static int Tag = 0;
    return &Tag;
  }

  void Profile(llvm::FoldingSetNodeID &ID) const override {
    ID.AddPointer(getTag());
    ID.AddPointer(Sym);
  }

  /// Did not track -> allocated. Other state (released) -> allocated.
  static inline bool isAllocated(const RefState *RSCurr, const RefState *RSPrev,
                                 const Stmt *Stmt) {
    return (Stmt && (isa<CallExpr>(Stmt) || isa<CXXNewExpr>(Stmt)) &&
            (RSCurr &&
             (RSCurr->isAllocated() || RSCurr->isAllocatedOfSizeZero())) &&
            (!RSPrev ||
             !(RSPrev->isAllocated() || RSPrev->isAllocatedOfSizeZero())));
  }

  /// Did not track -> released. Other state (allocated) -> released.
  /// The statement associated with the release might be missing.
  static inline bool isReleased(const RefState *RSCurr, const RefState *RSPrev,
                                const Stmt *Stmt) {
    bool IsReleased =
        (RSCurr && RSCurr->isReleased()) && (!RSPrev || !RSPrev->isReleased());
    assert(!IsReleased ||
           (Stmt && (isa<CallExpr>(Stmt) || isa<CXXDeleteExpr>(Stmt))) ||
           (!Stmt && RSCurr->getAllocationFamily() == AF_InnerBuffer));
    return IsReleased;
  }

  /// Did not track -> relinquished. Other state (allocated) -> relinquished.
  static inline bool isRelinquished(const RefState *RSCurr,
                                    const RefState *RSPrev, const Stmt *Stmt) {
    return (Stmt &&
            (isa<CallExpr>(Stmt) || isa<ObjCMessageExpr>(Stmt) ||
             isa<ObjCPropertyRefExpr>(Stmt)) &&
            (RSCurr && RSCurr->isRelinquished()) &&
            (!RSPrev || !RSPrev->isRelinquished()));
  }

  /// If the expression is not a call, and the state change is
  /// released -> allocated, it must be the realloc return value
  /// check. If we have to handle more cases here, it might be cleaner just
  /// to track this extra bit in the state itself.
  static inline bool hasReallocFailed(const RefState *RSCurr,
                                      const RefState *RSPrev,
                                      const Stmt *Stmt) {
    return ((!Stmt || !isa<CallExpr>(Stmt)) &&
            (RSCurr &&
             (RSCurr->isAllocated() || RSCurr->isAllocatedOfSizeZero())) &&
            (RSPrev &&
             !(RSPrev->isAllocated() || RSPrev->isAllocatedOfSizeZero())));
  }

  PathDiagnosticPieceRef VisitNode(const ExplodedNode *N,
                                   BugReporterContext &BRC,
                                   PathSensitiveBugReport &BR) override;

  PathDiagnosticPieceRef getEndPath(BugReporterContext &BRC,
                                    const ExplodedNode *EndPathNode,
                                    PathSensitiveBugReport &BR) override {
    if (!IsLeak)
      return nullptr;

    PathDiagnosticLocation L = BR.getLocation();
    // Do not add the statement itself as a range in case of leak.
    return std::make_shared<PathDiagnosticEventPiece>(L, BR.getDescription(),
                                                      false);
  }

private:
  class StackHintGeneratorForReallocationFailed
      : public StackHintGeneratorForSymbol {
  public:
    StackHintGeneratorForReallocationFailed(SymbolRef S, StringRef M)
        : StackHintGeneratorForSymbol(S, M) {}

    std::string getMessageForArg(const Expr *ArgE, unsigned ArgIndex) override {
      // Printed parameters start at 1, not 0.
      ++ArgIndex;

      SmallString<200> buf;
      llvm::raw_svector_ostream os(buf);

      os << "Reallocation of " << ArgIndex << llvm::getOrdinalSuffix(ArgIndex)
         << " parameter failed";

      return os.str();
    }

    std::string getMessageForReturn(const CallExpr *CallExpr) override {
      return "Reallocation of returned value failed";
    }
  };
};

} // end anonymous namespace

// A map from the freed symbol to the symbol representing the return value of
// the free function.
REGISTER_MAP_WITH_PROGRAMSTATE(FreeReturnValue, SymbolRef, SymbolRef)

namespace {
class StopTrackingCallback final : public SymbolVisitor {
  ProgramStateRef state;
public:
  StopTrackingCallback(ProgramStateRef st) : state(std::move(st)) {}
  ProgramStateRef getState() const { return state; }

  bool VisitSymbol(SymbolRef sym) override {
    state = state->remove<RegionState>(sym);
    return true;
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Methods of MemFunctionInfoTy.
//===----------------------------------------------------------------------===//

void MemFunctionInfoTy::initIdentifierInfo(ASTContext &Ctx) const {
  if (II_malloc)
    return;
  II_alloca = &Ctx.Idents.get("alloca");
  II_malloc = &Ctx.Idents.get("malloc");
  II_free = &Ctx.Idents.get("free");
  II_realloc = &Ctx.Idents.get("realloc");
  II_reallocf = &Ctx.Idents.get("reallocf");
  II_calloc = &Ctx.Idents.get("calloc");
  II_valloc = &Ctx.Idents.get("valloc");
  II_strdup = &Ctx.Idents.get("strdup");
  II_strndup = &Ctx.Idents.get("strndup");
  II_wcsdup = &Ctx.Idents.get("wcsdup");
  II_kmalloc = &Ctx.Idents.get("kmalloc");
  II_kfree = &Ctx.Idents.get("kfree");
  II_if_nameindex = &Ctx.Idents.get("if_nameindex");
  II_if_freenameindex = &Ctx.Idents.get("if_freenameindex");

  //MSVC uses `_`-prefixed instead, so we check for them too.
  II_win_strdup = &Ctx.Idents.get("_strdup");
  II_win_wcsdup = &Ctx.Idents.get("_wcsdup");
  II_win_alloca = &Ctx.Idents.get("_alloca");

  // Glib
  II_g_malloc = &Ctx.Idents.get("g_malloc");
  II_g_malloc0 = &Ctx.Idents.get("g_malloc0");
  II_g_realloc = &Ctx.Idents.get("g_realloc");
  II_g_try_malloc = &Ctx.Idents.get("g_try_malloc");
  II_g_try_malloc0 = &Ctx.Idents.get("g_try_malloc0");
  II_g_try_realloc = &Ctx.Idents.get("g_try_realloc");
  II_g_free = &Ctx.Idents.get("g_free");
  II_g_memdup = &Ctx.Idents.get("g_memdup");
  II_g_malloc_n = &Ctx.Idents.get("g_malloc_n");
  II_g_malloc0_n = &Ctx.Idents.get("g_malloc0_n");
  II_g_realloc_n = &Ctx.Idents.get("g_realloc_n");
  II_g_try_malloc_n = &Ctx.Idents.get("g_try_malloc_n");
  II_g_try_malloc0_n = &Ctx.Idents.get("g_try_malloc0_n");
  II_g_try_realloc_n = &Ctx.Idents.get("g_try_realloc_n");
}

bool MemFunctionInfoTy::isMemFunction(const FunctionDecl *FD,
                                      ASTContext &C) const {
  if (isCMemFunction(FD, C, AF_Malloc, MemoryOperationKind::MOK_Any))
    return true;

  if (isCMemFunction(FD, C, AF_IfNameIndex, MemoryOperationKind::MOK_Any))
    return true;

  if (isCMemFunction(FD, C, AF_Alloca, MemoryOperationKind::MOK_Any))
    return true;

  if (isStandardNewDelete(FD, C))
    return true;

  return false;
}

bool MemFunctionInfoTy::isCMemFunction(const FunctionDecl *FD, ASTContext &C,
                                       AllocationFamily Family,
                                       MemoryOperationKind MemKind) const {
  if (!FD)
    return false;

  bool CheckFree = (MemKind == MemoryOperationKind::MOK_Any ||
                    MemKind == MemoryOperationKind::MOK_Free);
  bool CheckAlloc = (MemKind == MemoryOperationKind::MOK_Any ||
                     MemKind == MemoryOperationKind::MOK_Allocate);

  if (FD->getKind() == Decl::Function) {
    const IdentifierInfo *FunI = FD->getIdentifier();
    initIdentifierInfo(C);

    if (Family == AF_Malloc && CheckFree) {
      if (FunI == II_free || FunI == II_realloc || FunI == II_reallocf ||
          FunI == II_g_free || FunI == II_kfree)
        return true;
    }

    if (Family == AF_Malloc && CheckAlloc) {
      if (FunI == II_malloc || FunI == II_realloc || FunI == II_reallocf ||
          FunI == II_calloc || FunI == II_valloc || FunI == II_strdup ||
          FunI == II_win_strdup || FunI == II_strndup || FunI == II_wcsdup ||
          FunI == II_win_wcsdup || FunI == II_kmalloc ||
          FunI == II_g_malloc || FunI == II_g_malloc0 ||
          FunI == II_g_realloc || FunI == II_g_try_malloc ||
          FunI == II_g_try_malloc0 || FunI == II_g_try_realloc ||
          FunI == II_g_memdup || FunI == II_g_malloc_n ||
          FunI == II_g_malloc0_n || FunI == II_g_realloc_n ||
          FunI == II_g_try_malloc_n || FunI == II_g_try_malloc0_n ||
          FunI == II_g_try_realloc_n)
        return true;
    }

    if (Family == AF_IfNameIndex && CheckFree) {
      if (FunI == II_if_freenameindex)
        return true;
    }

    if (Family == AF_IfNameIndex && CheckAlloc) {
      if (FunI == II_if_nameindex)
        return true;
    }

    if (Family == AF_Alloca && CheckAlloc) {
      if (FunI == II_alloca || FunI == II_win_alloca)
        return true;
    }
  }

  if (Family != AF_Malloc)
    return false;

  if (ShouldIncludeOwnershipAnnotatedFunctions && FD->hasAttrs()) {
    for (const auto *I : FD->specific_attrs<OwnershipAttr>()) {
      OwnershipAttr::OwnershipKind OwnKind = I->getOwnKind();
      if(OwnKind == OwnershipAttr::Takes || OwnKind == OwnershipAttr::Holds) {
        if (CheckFree)
          return true;
      } else if (OwnKind == OwnershipAttr::Returns) {
        if (CheckAlloc)
          return true;
      }
    }
  }

  return false;
}
bool MemFunctionInfoTy::isStandardNewDelete(const FunctionDecl *FD,
                                            ASTContext &C) const {
  if (!FD)
    return false;

  OverloadedOperatorKind Kind = FD->getOverloadedOperator();
  if (Kind != OO_New && Kind != OO_Array_New &&
      Kind != OO_Delete && Kind != OO_Array_Delete)
    return false;

  // This is standard if and only if it's not defined in a user file.
  SourceLocation L = FD->getLocation();
  // If the header for operator delete is not included, it's still defined
  // in an invalid source location. Check to make sure we don't crash.
  return !L.isValid() || C.getSourceManager().isInSystemHeader(L);
}

//===----------------------------------------------------------------------===//
// Methods of MallocChecker and MallocBugVisitor.
//===----------------------------------------------------------------------===//

llvm::Optional<ProgramStateRef> MallocChecker::performKernelMalloc(
  const CallExpr *CE, CheckerContext &C, const ProgramStateRef &State) const {
  // 3-argument malloc(), as commonly used in {Free,Net,Open}BSD Kernels:
  //
  // void *malloc(unsigned long size, struct malloc_type *mtp, int flags);
  //
  // One of the possible flags is M_ZERO, which means 'give me back an
  // allocation which is already zeroed', like calloc.

  // 2-argument kmalloc(), as used in the Linux kernel:
  //
  // void *kmalloc(size_t size, gfp_t flags);
  //
  // Has the similar flag value __GFP_ZERO.

  // This logic is largely cloned from O_CREAT in UnixAPIChecker, maybe some
  // code could be shared.

  ASTContext &Ctx = C.getASTContext();
  llvm::Triple::OSType OS = Ctx.getTargetInfo().getTriple().getOS();

  if (!KernelZeroFlagVal.hasValue()) {
    if (OS == llvm::Triple::FreeBSD)
      KernelZeroFlagVal = 0x0100;
    else if (OS == llvm::Triple::NetBSD)
      KernelZeroFlagVal = 0x0002;
    else if (OS == llvm::Triple::OpenBSD)
      KernelZeroFlagVal = 0x0008;
    else if (OS == llvm::Triple::Linux)
      // __GFP_ZERO
      KernelZeroFlagVal = 0x8000;
    else
      // FIXME: We need a more general way of getting the M_ZERO value.
      // See also: O_CREAT in UnixAPIChecker.cpp.

      // Fall back to normal malloc behavior on platforms where we don't
      // know M_ZERO.
      return None;
  }

  // We treat the last argument as the flags argument, and callers fall-back to
  // normal malloc on a None return. This works for the FreeBSD kernel malloc
  // as well as Linux kmalloc.
  if (CE->getNumArgs() < 2)
    return None;

  const Expr *FlagsEx = CE->getArg(CE->getNumArgs() - 1);
  const SVal V = C.getSVal(FlagsEx);
  if (!V.getAs<NonLoc>()) {
    // The case where 'V' can be a location can only be due to a bad header,
    // so in this case bail out.
    return None;
  }

  NonLoc Flags = V.castAs<NonLoc>();
  NonLoc ZeroFlag = C.getSValBuilder()
      .makeIntVal(KernelZeroFlagVal.getValue(), FlagsEx->getType())
      .castAs<NonLoc>();
  SVal MaskedFlagsUC = C.getSValBuilder().evalBinOpNN(State, BO_And,
                                                      Flags, ZeroFlag,
                                                      FlagsEx->getType());
  if (MaskedFlagsUC.isUnknownOrUndef())
    return None;
  DefinedSVal MaskedFlags = MaskedFlagsUC.castAs<DefinedSVal>();

  // Check if maskedFlags is non-zero.
  ProgramStateRef TrueState, FalseState;
  std::tie(TrueState, FalseState) = State->assume(MaskedFlags);

  // If M_ZERO is set, treat this like calloc (initialized).
  if (TrueState && !FalseState) {
    SVal ZeroVal = C.getSValBuilder().makeZeroVal(Ctx.CharTy);
    return MallocMemAux(C, CE, CE->getArg(0), ZeroVal, TrueState);
  }

  return None;
}

SVal MallocChecker::evalMulForBufferSize(CheckerContext &C, const Expr *Blocks,
                                         const Expr *BlockBytes) {
  SValBuilder &SB = C.getSValBuilder();
  SVal BlocksVal = C.getSVal(Blocks);
  SVal BlockBytesVal = C.getSVal(BlockBytes);
  ProgramStateRef State = C.getState();
  SVal TotalSize = SB.evalBinOp(State, BO_Mul, BlocksVal, BlockBytesVal,
                                SB.getContext().getSizeType());
  return TotalSize;
}

void MallocChecker::checkPostStmt(const CallExpr *CE, CheckerContext &C) const {
  if (C.wasInlined)
    return;

  const FunctionDecl *FD = C.getCalleeDecl(CE);
  if (!FD)
    return;

  ProgramStateRef State = C.getState();
  bool IsKnownToBeAllocatedMemory = false;

  if (FD->getKind() == Decl::Function) {
    MemFunctionInfo.initIdentifierInfo(C.getASTContext());
    IdentifierInfo *FunI = FD->getIdentifier();

    if (FunI == MemFunctionInfo.II_malloc ||
        FunI == MemFunctionInfo.II_g_malloc ||
        FunI == MemFunctionInfo.II_g_try_malloc) {
      switch (CE->getNumArgs()) {
      default:
        return;
      case 1:
        State = MallocMemAux(C, CE, CE->getArg(0), UndefinedVal(), State);
        State = ProcessZeroAllocCheck(C, CE, 0, State);
        break;
      case 2:
        State = MallocMemAux(C, CE, CE->getArg(0), UndefinedVal(), State);
        break;
      case 3:
        llvm::Optional<ProgramStateRef> MaybeState =
          performKernelMalloc(CE, C, State);
        if (MaybeState.hasValue())
          State = MaybeState.getValue();
        else
          State = MallocMemAux(C, CE, CE->getArg(0), UndefinedVal(), State);
        break;
      }
    } else if (FunI == MemFunctionInfo.II_kmalloc) {
      if (CE->getNumArgs() < 1)
        return;
      llvm::Optional<ProgramStateRef> MaybeState =
        performKernelMalloc(CE, C, State);
      if (MaybeState.hasValue())
        State = MaybeState.getValue();
      else
        State = MallocMemAux(C, CE, CE->getArg(0), UndefinedVal(), State);
    } else if (FunI == MemFunctionInfo.II_valloc) {
      if (CE->getNumArgs() < 1)
        return;
      State = MallocMemAux(C, CE, CE->getArg(0), UndefinedVal(), State);
      State = ProcessZeroAllocCheck(C, CE, 0, State);
    } else if (FunI == MemFunctionInfo.II_realloc ||
               FunI == MemFunctionInfo.II_g_realloc ||
               FunI == MemFunctionInfo.II_g_try_realloc) {
      State = ReallocMemAux(C, CE, /*ShouldFreeOnFail*/ false, State);
      State = ProcessZeroAllocCheck(C, CE, 1, State);
    } else if (FunI == MemFunctionInfo.II_reallocf) {
      State = ReallocMemAux(C, CE, /*ShouldFreeOnFail*/ true, State);
      State = ProcessZeroAllocCheck(C, CE, 1, State);
    } else if (FunI == MemFunctionInfo.II_calloc) {
      State = CallocMem(C, CE, State);
      State = ProcessZeroAllocCheck(C, CE, 0, State);
      State = ProcessZeroAllocCheck(C, CE, 1, State);
    } else if (FunI == MemFunctionInfo.II_free ||
               FunI == MemFunctionInfo.II_g_free ||
               FunI == MemFunctionInfo.II_kfree) {
      if (suppressDeallocationsInSuspiciousContexts(CE, C))
        return;

      State = FreeMemAux(C, CE, State, 0, false, IsKnownToBeAllocatedMemory);
    } else if (FunI == MemFunctionInfo.II_strdup ||
               FunI == MemFunctionInfo.II_win_strdup ||
               FunI == MemFunctionInfo.II_wcsdup ||
               FunI == MemFunctionInfo.II_win_wcsdup) {
      State = MallocUpdateRefState(C, CE, State);
    } else if (FunI == MemFunctionInfo.II_strndup) {
      State = MallocUpdateRefState(C, CE, State);
    } else if (FunI == MemFunctionInfo.II_alloca ||
               FunI == MemFunctionInfo.II_win_alloca) {
      if (CE->getNumArgs() < 1)
        return;
      State = MallocMemAux(C, CE, CE->getArg(0), UndefinedVal(), State,
                           AF_Alloca);
      State = ProcessZeroAllocCheck(C, CE, 0, State);
    } else if (MemFunctionInfo.isStandardNewDelete(FD, C.getASTContext())) {
      // Process direct calls to operator new/new[]/delete/delete[] functions
      // as distinct from new/new[]/delete/delete[] expressions that are
      // processed by the checkPostStmt callbacks for CXXNewExpr and
      // CXXDeleteExpr.
      switch (FD->getOverloadedOperator()) {
      case OO_New:
        State = MallocMemAux(C, CE, CE->getArg(0), UndefinedVal(), State,
                             AF_CXXNew);
        State = ProcessZeroAllocCheck(C, CE, 0, State);
        break;
      case OO_Array_New:
        State = MallocMemAux(C, CE, CE->getArg(0), UndefinedVal(), State,
                             AF_CXXNewArray);
        State = ProcessZeroAllocCheck(C, CE, 0, State);
        break;
      case OO_Delete:
      case OO_Array_Delete:
        State = FreeMemAux(C, CE, State, 0, false, IsKnownToBeAllocatedMemory);
        break;
      default:
        llvm_unreachable("not a new/delete operator");
      }
    } else if (FunI == MemFunctionInfo.II_if_nameindex) {
      // Should we model this differently? We can allocate a fixed number of
      // elements with zeros in the last one.
      State = MallocMemAux(C, CE, UnknownVal(), UnknownVal(), State,
                           AF_IfNameIndex);
    } else if (FunI == MemFunctionInfo.II_if_freenameindex) {
      State = FreeMemAux(C, CE, State, 0, false, IsKnownToBeAllocatedMemory);
    } else if (FunI == MemFunctionInfo.II_g_malloc0 ||
               FunI == MemFunctionInfo.II_g_try_malloc0) {
      if (CE->getNumArgs() < 1)
        return;
      SValBuilder &svalBuilder = C.getSValBuilder();
      SVal zeroVal = svalBuilder.makeZeroVal(svalBuilder.getContext().CharTy);
      State = MallocMemAux(C, CE, CE->getArg(0), zeroVal, State);
      State = ProcessZeroAllocCheck(C, CE, 0, State);
    } else if (FunI == MemFunctionInfo.II_g_memdup) {
      if (CE->getNumArgs() < 2)
        return;
      State = MallocMemAux(C, CE, CE->getArg(1), UndefinedVal(), State);
      State = ProcessZeroAllocCheck(C, CE, 1, State);
    } else if (FunI == MemFunctionInfo.II_g_malloc_n ||
               FunI == MemFunctionInfo.II_g_try_malloc_n ||
               FunI == MemFunctionInfo.II_g_malloc0_n ||
               FunI == MemFunctionInfo.II_g_try_malloc0_n) {
      if (CE->getNumArgs() < 2)
        return;
      SVal Init = UndefinedVal();
      if (FunI == MemFunctionInfo.II_g_malloc0_n ||
          FunI == MemFunctionInfo.II_g_try_malloc0_n) {
        SValBuilder &SB = C.getSValBuilder();
        Init = SB.makeZeroVal(SB.getContext().CharTy);
      }
      SVal TotalSize = evalMulForBufferSize(C, CE->getArg(0), CE->getArg(1));
      State = MallocMemAux(C, CE, TotalSize, Init, State);
      State = ProcessZeroAllocCheck(C, CE, 0, State);
      State = ProcessZeroAllocCheck(C, CE, 1, State);
    } else if (FunI == MemFunctionInfo.II_g_realloc_n ||
               FunI == MemFunctionInfo.II_g_try_realloc_n) {
      if (CE->getNumArgs() < 3)
        return;
      State = ReallocMemAux(C, CE, /*ShouldFreeOnFail*/ false, State,
                            /*SuffixWithN*/ true);
      State = ProcessZeroAllocCheck(C, CE, 1, State);
      State = ProcessZeroAllocCheck(C, CE, 2, State);
    }
  }

  if (MemFunctionInfo.ShouldIncludeOwnershipAnnotatedFunctions ||
      ChecksEnabled[CK_MismatchedDeallocatorChecker]) {
    // Check all the attributes, if there are any.
    // There can be multiple of these attributes.
    if (FD->hasAttrs())
      for (const auto *I : FD->specific_attrs<OwnershipAttr>()) {
        switch (I->getOwnKind()) {
        case OwnershipAttr::Returns:
          State = MallocMemReturnsAttr(C, CE, I, State);
          break;
        case OwnershipAttr::Takes:
        case OwnershipAttr::Holds:
          State = FreeMemAttr(C, CE, I, State);
          break;
        }
      }
  }
  C.addTransition(State);
}

// Performs a 0-sized allocations check.
ProgramStateRef MallocChecker::ProcessZeroAllocCheck(
    CheckerContext &C, const Expr *E, const unsigned IndexOfSizeArg,
    ProgramStateRef State, Optional<SVal> RetVal) {
  if (!State)
    return nullptr;

  if (!RetVal)
    RetVal = C.getSVal(E);

  const Expr *Arg = nullptr;

  if (const CallExpr *CE = dyn_cast<CallExpr>(E)) {
    Arg = CE->getArg(IndexOfSizeArg);
  }
  else if (const CXXNewExpr *NE = dyn_cast<CXXNewExpr>(E)) {
    if (NE->isArray())
      Arg = *NE->getArraySize();
    else
      return State;
  }
  else
    llvm_unreachable("not a CallExpr or CXXNewExpr");

  assert(Arg);

  Optional<DefinedSVal> DefArgVal = C.getSVal(Arg).getAs<DefinedSVal>();

  if (!DefArgVal)
    return State;

  // Check if the allocation size is 0.
  ProgramStateRef TrueState, FalseState;
  SValBuilder &SvalBuilder = C.getSValBuilder();
  DefinedSVal Zero =
      SvalBuilder.makeZeroVal(Arg->getType()).castAs<DefinedSVal>();

  std::tie(TrueState, FalseState) =
      State->assume(SvalBuilder.evalEQ(State, *DefArgVal, Zero));

  if (TrueState && !FalseState) {
    SymbolRef Sym = RetVal->getAsLocSymbol();
    if (!Sym)
      return State;

    const RefState *RS = State->get<RegionState>(Sym);
    if (RS) {
      if (RS->isAllocated())
        return TrueState->set<RegionState>(Sym,
                                          RefState::getAllocatedOfSizeZero(RS));
      else
        return State;
    } else {
      // Case of zero-size realloc. Historically 'realloc(ptr, 0)' is treated as
      // 'free(ptr)' and the returned value from 'realloc(ptr, 0)' is not
      // tracked. Add zero-reallocated Sym to the state to catch references
      // to zero-allocated memory.
      return TrueState->add<ReallocSizeZeroSymbols>(Sym);
    }
  }

  // Assume the value is non-zero going forward.
  assert(FalseState);
  return FalseState;
}

static QualType getDeepPointeeType(QualType T) {
  QualType Result = T, PointeeType = T->getPointeeType();
  while (!PointeeType.isNull()) {
    Result = PointeeType;
    PointeeType = PointeeType->getPointeeType();
  }
  return Result;
}

/// \returns true if the constructor invoked by \p NE has an argument of a
/// pointer/reference to a record type.
static bool hasNonTrivialConstructorCall(const CXXNewExpr *NE) {

  const CXXConstructExpr *ConstructE = NE->getConstructExpr();
  if (!ConstructE)
    return false;

  if (!NE->getAllocatedType()->getAsCXXRecordDecl())
    return false;

  const CXXConstructorDecl *CtorD = ConstructE->getConstructor();

  // Iterate over the constructor parameters.
  for (const auto *CtorParam : CtorD->parameters()) {

    QualType CtorParamPointeeT = CtorParam->getType()->getPointeeType();
    if (CtorParamPointeeT.isNull())
      continue;

    CtorParamPointeeT = getDeepPointeeType(CtorParamPointeeT);

    if (CtorParamPointeeT->getAsCXXRecordDecl())
      return true;
  }

  return false;
}

void MallocChecker::processNewAllocation(const CXXNewExpr *NE,
                                         CheckerContext &C,
                                         SVal Target) const {
  if (!MemFunctionInfo.isStandardNewDelete(NE->getOperatorNew(),
                                           C.getASTContext()))
    return;

  const ParentMap &PM = C.getLocationContext()->getParentMap();

  // Non-trivial constructors have a chance to escape 'this', but marking all
  // invocations of trivial constructors as escaped would cause too great of
  // reduction of true positives, so let's just do that for constructors that
  // have an argument of a pointer-to-record type.
  if (!PM.isConsumedExpr(NE) && hasNonTrivialConstructorCall(NE))
    return;

  ProgramStateRef State = C.getState();
  // The return value from operator new is bound to a specified initialization
  // value (if any) and we don't want to loose this value. So we call
  // MallocUpdateRefState() instead of MallocMemAux() which breaks the
  // existing binding.
  State = MallocUpdateRefState(C, NE, State, NE->isArray() ? AF_CXXNewArray
                                                           : AF_CXXNew, Target);
  State = addExtentSize(C, NE, State, Target);
  State = ProcessZeroAllocCheck(C, NE, 0, State, Target);
  C.addTransition(State);
}

void MallocChecker::checkPostStmt(const CXXNewExpr *NE,
                                  CheckerContext &C) const {
  if (!C.getAnalysisManager().getAnalyzerOptions().MayInlineCXXAllocator)
    processNewAllocation(NE, C, C.getSVal(NE));
}

void MallocChecker::checkNewAllocator(const CXXNewExpr *NE, SVal Target,
                                      CheckerContext &C) const {
  if (!C.wasInlined)
    processNewAllocation(NE, C, Target);
}

// Sets the extent value of the MemRegion allocated by
// new expression NE to its size in Bytes.
//
ProgramStateRef MallocChecker::addExtentSize(CheckerContext &C,
                                             const CXXNewExpr *NE,
                                             ProgramStateRef State,
                                             SVal Target) {
  if (!State)
    return nullptr;
  SValBuilder &svalBuilder = C.getSValBuilder();
  SVal ElementCount;
  const SubRegion *Region;
  if (NE->isArray()) {
    const Expr *SizeExpr = *NE->getArraySize();
    ElementCount = C.getSVal(SizeExpr);
    // Store the extent size for the (symbolic)region
    // containing the elements.
    Region = Target.getAsRegion()
                 ->castAs<SubRegion>()
                 ->StripCasts()
                 ->castAs<SubRegion>();
  } else {
    ElementCount = svalBuilder.makeIntVal(1, true);
    Region = Target.getAsRegion()->castAs<SubRegion>();
  }

  // Set the region's extent equal to the Size in Bytes.
  QualType ElementType = NE->getAllocatedType();
  ASTContext &AstContext = C.getASTContext();
  CharUnits TypeSize = AstContext.getTypeSizeInChars(ElementType);

  if (ElementCount.getAs<NonLoc>()) {
    DefinedOrUnknownSVal Extent = Region->getExtent(svalBuilder);
    // size in Bytes = ElementCount*TypeSize
    SVal SizeInBytes = svalBuilder.evalBinOpNN(
        State, BO_Mul, ElementCount.castAs<NonLoc>(),
        svalBuilder.makeArrayIndex(TypeSize.getQuantity()),
        svalBuilder.getArrayIndexType());
    DefinedOrUnknownSVal extentMatchesSize = svalBuilder.evalEQ(
        State, Extent, SizeInBytes.castAs<DefinedOrUnknownSVal>());
    State = State->assume(extentMatchesSize, true);
  }
  return State;
}

void MallocChecker::checkPreStmt(const CXXDeleteExpr *DE,
                                 CheckerContext &C) const {

  if (!ChecksEnabled[CK_NewDeleteChecker])
    if (SymbolRef Sym = C.getSVal(DE->getArgument()).getAsSymbol())
      checkUseAfterFree(Sym, C, DE->getArgument());

  if (!MemFunctionInfo.isStandardNewDelete(DE->getOperatorDelete(),
                                           C.getASTContext()))
    return;

  ProgramStateRef State = C.getState();
  bool IsKnownToBeAllocated;
  State = FreeMemAux(C, DE->getArgument(), DE, State,
                     /*Hold*/ false, IsKnownToBeAllocated);

  C.addTransition(State);
}

static bool isKnownDeallocObjCMethodName(const ObjCMethodCall &Call) {
  // If the first selector piece is one of the names below, assume that the
  // object takes ownership of the memory, promising to eventually deallocate it
  // with free().
  // Ex:  [NSData dataWithBytesNoCopy:bytes length:10];
  // (...unless a 'freeWhenDone' parameter is false, but that's checked later.)
  StringRef FirstSlot = Call.getSelector().getNameForSlot(0);
  return FirstSlot == "dataWithBytesNoCopy" ||
         FirstSlot == "initWithBytesNoCopy" ||
         FirstSlot == "initWithCharactersNoCopy";
}

static Optional<bool> getFreeWhenDoneArg(const ObjCMethodCall &Call) {
  Selector S = Call.getSelector();

  // FIXME: We should not rely on fully-constrained symbols being folded.
  for (unsigned i = 1; i < S.getNumArgs(); ++i)
    if (S.getNameForSlot(i).equals("freeWhenDone"))
      return !Call.getArgSVal(i).isZeroConstant();

  return None;
}

void MallocChecker::checkPostObjCMessage(const ObjCMethodCall &Call,
                                         CheckerContext &C) const {
  if (C.wasInlined)
    return;

  if (!isKnownDeallocObjCMethodName(Call))
    return;

  if (Optional<bool> FreeWhenDone = getFreeWhenDoneArg(Call))
    if (!*FreeWhenDone)
      return;

  if (Call.hasNonZeroCallbackArg())
    return;

  bool IsKnownToBeAllocatedMemory;
  ProgramStateRef State =
      FreeMemAux(C, Call.getArgExpr(0), Call.getOriginExpr(), C.getState(),
                 /*Hold=*/true, IsKnownToBeAllocatedMemory,
                 /*RetNullOnFailure=*/true);

  C.addTransition(State);
}

ProgramStateRef
MallocChecker::MallocMemReturnsAttr(CheckerContext &C, const CallExpr *CE,
                                    const OwnershipAttr *Att,
                                    ProgramStateRef State) const {
  if (!State)
    return nullptr;

  if (Att->getModule() != MemFunctionInfo.II_malloc)
    return nullptr;

  OwnershipAttr::args_iterator I = Att->args_begin(), E = Att->args_end();
  if (I != E) {
    return MallocMemAux(C, CE, CE->getArg(I->getASTIndex()), UndefinedVal(),
                        State);
  }
  return MallocMemAux(C, CE, UnknownVal(), UndefinedVal(), State);
}

ProgramStateRef MallocChecker::MallocMemAux(CheckerContext &C,
                                            const CallExpr *CE,
                                            const Expr *SizeEx, SVal Init,
                                            ProgramStateRef State,
                                            AllocationFamily Family) {
  if (!State)
    return nullptr;

  return MallocMemAux(C, CE, C.getSVal(SizeEx), Init, State, Family);
}

ProgramStateRef MallocChecker::MallocMemAux(CheckerContext &C,
                                           const CallExpr *CE,
                                           SVal Size, SVal Init,
                                           ProgramStateRef State,
                                           AllocationFamily Family) {
  if (!State)
    return nullptr;

  // We expect the malloc functions to return a pointer.
  if (!Loc::isLocType(CE->getType()))
    return nullptr;

  // Bind the return value to the symbolic value from the heap region.
  // TODO: We could rewrite post visit to eval call; 'malloc' does not have
  // side effects other than what we model here.
  unsigned Count = C.blockCount();
  SValBuilder &svalBuilder = C.getSValBuilder();
  const LocationContext *LCtx = C.getPredecessor()->getLocationContext();
  DefinedSVal RetVal = svalBuilder.getConjuredHeapSymbolVal(CE, LCtx, Count)
      .castAs<DefinedSVal>();
  State = State->BindExpr(CE, C.getLocationContext(), RetVal);

  // Fill the region with the initialization value.
  State = State->bindDefaultInitial(RetVal, Init, LCtx);

  // Set the region's extent equal to the Size parameter.
  const SymbolicRegion *R =
      dyn_cast_or_null<SymbolicRegion>(RetVal.getAsRegion());
  if (!R)
    return nullptr;
  if (Optional<DefinedOrUnknownSVal> DefinedSize =
          Size.getAs<DefinedOrUnknownSVal>()) {
    SValBuilder &svalBuilder = C.getSValBuilder();
    DefinedOrUnknownSVal Extent = R->getExtent(svalBuilder);
    DefinedOrUnknownSVal extentMatchesSize =
        svalBuilder.evalEQ(State, Extent, *DefinedSize);

    State = State->assume(extentMatchesSize, true);
    assert(State);
  }

  return MallocUpdateRefState(C, CE, State, Family);
}

static ProgramStateRef MallocUpdateRefState(CheckerContext &C, const Expr *E,
                                            ProgramStateRef State,
                                            AllocationFamily Family,
                                            Optional<SVal> RetVal) {
  if (!State)
    return nullptr;

  // Get the return value.
  if (!RetVal)
    RetVal = C.getSVal(E);

  // We expect the malloc functions to return a pointer.
  if (!RetVal->getAs<Loc>())
    return nullptr;

  SymbolRef Sym = RetVal->getAsLocSymbol();
  // This is a return value of a function that was not inlined, such as malloc()
  // or new(). We've checked that in the caller. Therefore, it must be a symbol.
  assert(Sym);

  // Set the symbol's state to Allocated.
  return State->set<RegionState>(Sym, RefState::getAllocated(Family, E));
}

ProgramStateRef MallocChecker::FreeMemAttr(CheckerContext &C,
                                           const CallExpr *CE,
                                           const OwnershipAttr *Att,
                                           ProgramStateRef State) const {
  if (!State)
    return nullptr;

  if (Att->getModule() != MemFunctionInfo.II_malloc)
    return nullptr;

  bool IsKnownToBeAllocated = false;

  for (const auto &Arg : Att->args()) {
    ProgramStateRef StateI = FreeMemAux(
        C, CE, State, Arg.getASTIndex(),
        Att->getOwnKind() == OwnershipAttr::Holds, IsKnownToBeAllocated);
    if (StateI)
      State = StateI;
  }
  return State;
}

ProgramStateRef MallocChecker::FreeMemAux(CheckerContext &C, const CallExpr *CE,
                                          ProgramStateRef State, unsigned Num,
                                          bool Hold, bool &IsKnownToBeAllocated,
                                          bool ReturnsNullOnFailure) const {
  if (!State)
    return nullptr;

  if (CE->getNumArgs() < (Num + 1))
    return nullptr;

  return FreeMemAux(C, CE->getArg(Num), CE, State, Hold, IsKnownToBeAllocated,
                    ReturnsNullOnFailure);
}

/// Checks if the previous call to free on the given symbol failed - if free
/// failed, returns true. Also, returns the corresponding return value symbol.
static bool didPreviousFreeFail(ProgramStateRef State,
                                SymbolRef Sym, SymbolRef &RetStatusSymbol) {
  const SymbolRef *Ret = State->get<FreeReturnValue>(Sym);
  if (Ret) {
    assert(*Ret && "We should not store the null return symbol");
    ConstraintManager &CMgr = State->getConstraintManager();
    ConditionTruthVal FreeFailed = CMgr.isNull(State, *Ret);
    RetStatusSymbol = *Ret;
    return FreeFailed.isConstrainedTrue();
  }
  return false;
}

static AllocationFamily
getAllocationFamily(const MemFunctionInfoTy &MemFunctionInfo, CheckerContext &C,
                    const Stmt *S) {

  if (!S)
    return AF_None;

  if (const CallExpr *CE = dyn_cast<CallExpr>(S)) {
    const FunctionDecl *FD = C.getCalleeDecl(CE);

    if (!FD)
      FD = dyn_cast<FunctionDecl>(CE->getCalleeDecl());

    ASTContext &Ctx = C.getASTContext();

    if (MemFunctionInfo.isCMemFunction(FD, Ctx, AF_Malloc,
                                       MemoryOperationKind::MOK_Any))
      return AF_Malloc;

    if (MemFunctionInfo.isStandardNewDelete(FD, Ctx)) {
      OverloadedOperatorKind Kind = FD->getOverloadedOperator();
      if (Kind == OO_New || Kind == OO_Delete)
        return AF_CXXNew;
      else if (Kind == OO_Array_New || Kind == OO_Array_Delete)
        return AF_CXXNewArray;
    }

    if (MemFunctionInfo.isCMemFunction(FD, Ctx, AF_IfNameIndex,
                                       MemoryOperationKind::MOK_Any))
      return AF_IfNameIndex;

    if (MemFunctionInfo.isCMemFunction(FD, Ctx, AF_Alloca,
                                       MemoryOperationKind::MOK_Any))
      return AF_Alloca;

    return AF_None;
  }

  if (const CXXNewExpr *NE = dyn_cast<CXXNewExpr>(S))
    return NE->isArray() ? AF_CXXNewArray : AF_CXXNew;

  if (const CXXDeleteExpr *DE = dyn_cast<CXXDeleteExpr>(S))
    return DE->isArrayForm() ? AF_CXXNewArray : AF_CXXNew;

  if (isa<ObjCMessageExpr>(S))
    return AF_Malloc;

  return AF_None;
}

static bool printAllocDeallocName(raw_ostream &os, CheckerContext &C,
                                  const Expr *E) {
  if (const CallExpr *CE = dyn_cast<CallExpr>(E)) {
    // FIXME: This doesn't handle indirect calls.
    const FunctionDecl *FD = CE->getDirectCallee();
    if (!FD)
      return false;

    os << *FD;
    if (!FD->isOverloadedOperator())
      os << "()";
    return true;
  }

  if (const ObjCMessageExpr *Msg = dyn_cast<ObjCMessageExpr>(E)) {
    if (Msg->isInstanceMessage())
      os << "-";
    else
      os << "+";
    Msg->getSelector().print(os);
    return true;
  }

  if (const CXXNewExpr *NE = dyn_cast<CXXNewExpr>(E)) {
    os << "'"
       << getOperatorSpelling(NE->getOperatorNew()->getOverloadedOperator())
       << "'";
    return true;
  }

  if (const CXXDeleteExpr *DE = dyn_cast<CXXDeleteExpr>(E)) {
    os << "'"
       << getOperatorSpelling(DE->getOperatorDelete()->getOverloadedOperator())
       << "'";
    return true;
  }

  return false;
}

static void printExpectedAllocName(raw_ostream &os,
                                   const MemFunctionInfoTy &MemFunctionInfo,
                                   CheckerContext &C, const Expr *E) {
  AllocationFamily Family = getAllocationFamily(MemFunctionInfo, C, E);

  switch(Family) {
    case AF_Malloc: os << "malloc()"; return;
    case AF_CXXNew: os << "'new'"; return;
    case AF_CXXNewArray: os << "'new[]'"; return;
    case AF_IfNameIndex: os << "'if_nameindex()'"; return;
    case AF_InnerBuffer: os << "container-specific allocator"; return;
    case AF_Alloca:
    case AF_None: llvm_unreachable("not a deallocation expression");
  }
}

static void printExpectedDeallocName(raw_ostream &os, AllocationFamily Family) {
  switch(Family) {
    case AF_Malloc: os << "free()"; return;
    case AF_CXXNew: os << "'delete'"; return;
    case AF_CXXNewArray: os << "'delete[]'"; return;
    case AF_IfNameIndex: os << "'if_freenameindex()'"; return;
    case AF_InnerBuffer: os << "container-specific deallocator"; return;
    case AF_Alloca:
    case AF_None: llvm_unreachable("suspicious argument");
  }
}

ProgramStateRef MallocChecker::FreeMemAux(CheckerContext &C,
                                          const Expr *ArgExpr,
                                          const Expr *ParentExpr,
                                          ProgramStateRef State, bool Hold,
                                          bool &IsKnownToBeAllocated,
                                          bool ReturnsNullOnFailure) const {

  if (!State)
    return nullptr;

  SVal ArgVal = C.getSVal(ArgExpr);
  if (!ArgVal.getAs<DefinedOrUnknownSVal>())
    return nullptr;
  DefinedOrUnknownSVal location = ArgVal.castAs<DefinedOrUnknownSVal>();

  // Check for null dereferences.
  if (!location.getAs<Loc>())
    return nullptr;

  // The explicit NULL case, no operation is performed.
  ProgramStateRef notNullState, nullState;
  std::tie(notNullState, nullState) = State->assume(location);
  if (nullState && !notNullState)
    return nullptr;

  // Unknown values could easily be okay
  // Undefined values are handled elsewhere
  if (ArgVal.isUnknownOrUndef())
    return nullptr;

  const MemRegion *R = ArgVal.getAsRegion();

  // Nonlocs can't be freed, of course.
  // Non-region locations (labels and fixed addresses) also shouldn't be freed.
  if (!R) {
    ReportBadFree(C, ArgVal, ArgExpr->getSourceRange(), ParentExpr);
    return nullptr;
  }

  R = R->StripCasts();

  // Blocks might show up as heap data, but should not be free()d
  if (isa<BlockDataRegion>(R)) {
    ReportBadFree(C, ArgVal, ArgExpr->getSourceRange(), ParentExpr);
    return nullptr;
  }

  const MemSpaceRegion *MS = R->getMemorySpace();

  // Parameters, locals, statics, globals, and memory returned by
  // __builtin_alloca() shouldn't be freed.
  if (!(isa<UnknownSpaceRegion>(MS) || isa<HeapSpaceRegion>(MS))) {
    // FIXME: at the time this code was written, malloc() regions were
    // represented by conjured symbols, which are all in UnknownSpaceRegion.
    // This means that there isn't actually anything from HeapSpaceRegion
    // that should be freed, even though we allow it here.
    // Of course, free() can work on memory allocated outside the current
    // function, so UnknownSpaceRegion is always a possibility.
    // False negatives are better than false positives.

    if (isa<AllocaRegion>(R))
      ReportFreeAlloca(C, ArgVal, ArgExpr->getSourceRange());
    else
      ReportBadFree(C, ArgVal, ArgExpr->getSourceRange(), ParentExpr);

    return nullptr;
  }

  const SymbolicRegion *SrBase = dyn_cast<SymbolicRegion>(R->getBaseRegion());
  // Various cases could lead to non-symbol values here.
  // For now, ignore them.
  if (!SrBase)
    return nullptr;

  SymbolRef SymBase = SrBase->getSymbol();
  const RefState *RsBase = State->get<RegionState>(SymBase);
  SymbolRef PreviousRetStatusSymbol = nullptr;

  IsKnownToBeAllocated =
      RsBase && (RsBase->isAllocated() || RsBase->isAllocatedOfSizeZero());

  if (RsBase) {

    // Memory returned by alloca() shouldn't be freed.
    if (RsBase->getAllocationFamily() == AF_Alloca) {
      ReportFreeAlloca(C, ArgVal, ArgExpr->getSourceRange());
      return nullptr;
    }

    // Check for double free first.
    if ((RsBase->isReleased() || RsBase->isRelinquished()) &&
        !didPreviousFreeFail(State, SymBase, PreviousRetStatusSymbol)) {
      ReportDoubleFree(C, ParentExpr->getSourceRange(), RsBase->isReleased(),
                       SymBase, PreviousRetStatusSymbol);
      return nullptr;

    // If the pointer is allocated or escaped, but we are now trying to free it,
    // check that the call to free is proper.
    } else if (RsBase->isAllocated() || RsBase->isAllocatedOfSizeZero() ||
               RsBase->isEscaped()) {

      // Check if an expected deallocation function matches the real one.
      bool DeallocMatchesAlloc =
          RsBase->getAllocationFamily() ==
          getAllocationFamily(MemFunctionInfo, C, ParentExpr);
      if (!DeallocMatchesAlloc) {
        ReportMismatchedDealloc(C, ArgExpr->getSourceRange(),
                                ParentExpr, RsBase, SymBase, Hold);
        return nullptr;
      }

      // Check if the memory location being freed is the actual location
      // allocated, or an offset.
      RegionOffset Offset = R->getAsOffset();
      if (Offset.isValid() &&
          !Offset.hasSymbolicOffset() &&
          Offset.getOffset() != 0) {
        const Expr *AllocExpr = cast<Expr>(RsBase->getStmt());
        ReportOffsetFree(C, ArgVal, ArgExpr->getSourceRange(), ParentExpr,
                         AllocExpr);
        return nullptr;
      }
    }
  }

  if (SymBase->getType()->isFunctionPointerType()) {
    ReportFunctionPointerFree(C, ArgVal, ArgExpr->getSourceRange(), ParentExpr);
    return nullptr;
  }

  // Clean out the info on previous call to free return info.
  State = State->remove<FreeReturnValue>(SymBase);

  // Keep track of the return value. If it is NULL, we will know that free
  // failed.
  if (ReturnsNullOnFailure) {
    SVal RetVal = C.getSVal(ParentExpr);
    SymbolRef RetStatusSymbol = RetVal.getAsSymbol();
    if (RetStatusSymbol) {
      C.getSymbolManager().addSymbolDependency(SymBase, RetStatusSymbol);
      State = State->set<FreeReturnValue>(SymBase, RetStatusSymbol);
    }
  }

  AllocationFamily Family =
      RsBase ? RsBase->getAllocationFamily()
             : getAllocationFamily(MemFunctionInfo, C, ParentExpr);
  // Normal free.
  if (Hold)
    return State->set<RegionState>(SymBase,
                                   RefState::getRelinquished(Family,
                                                             ParentExpr));

  return State->set<RegionState>(SymBase,
                                 RefState::getReleased(Family, ParentExpr));
}

Optional<MallocChecker::CheckKind>
MallocChecker::getCheckIfTracked(AllocationFamily Family,
                                 bool IsALeakCheck) const {
  switch (Family) {
  case AF_Malloc:
  case AF_Alloca:
  case AF_IfNameIndex: {
    if (ChecksEnabled[CK_MallocChecker])
      return CK_MallocChecker;
    return None;
  }
  case AF_CXXNew:
  case AF_CXXNewArray: {
    if (IsALeakCheck) {
      if (ChecksEnabled[CK_NewDeleteLeaksChecker])
        return CK_NewDeleteLeaksChecker;
    }
    else {
      if (ChecksEnabled[CK_NewDeleteChecker])
        return CK_NewDeleteChecker;
    }
    return None;
  }
  case AF_InnerBuffer: {
    if (ChecksEnabled[CK_InnerPointerChecker])
      return CK_InnerPointerChecker;
    return None;
  }
  case AF_None: {
    llvm_unreachable("no family");
  }
  }
  llvm_unreachable("unhandled family");
}

Optional<MallocChecker::CheckKind>
MallocChecker::getCheckIfTracked(CheckerContext &C,
                                 const Stmt *AllocDeallocStmt,
                                 bool IsALeakCheck) const {
  return getCheckIfTracked(
      getAllocationFamily(MemFunctionInfo, C, AllocDeallocStmt), IsALeakCheck);
}

Optional<MallocChecker::CheckKind>
MallocChecker::getCheckIfTracked(CheckerContext &C, SymbolRef Sym,
                                 bool IsALeakCheck) const {
  if (C.getState()->contains<ReallocSizeZeroSymbols>(Sym))
    return CK_MallocChecker;

  const RefState *RS = C.getState()->get<RegionState>(Sym);
  assert(RS);
  return getCheckIfTracked(RS->getAllocationFamily(), IsALeakCheck);
}

bool MallocChecker::SummarizeValue(raw_ostream &os, SVal V) {
  if (Optional<nonloc::ConcreteInt> IntVal = V.getAs<nonloc::ConcreteInt>())
    os << "an integer (" << IntVal->getValue() << ")";
  else if (Optional<loc::ConcreteInt> ConstAddr = V.getAs<loc::ConcreteInt>())
    os << "a constant address (" << ConstAddr->getValue() << ")";
  else if (Optional<loc::GotoLabel> Label = V.getAs<loc::GotoLabel>())
    os << "the address of the label '" << Label->getLabel()->getName() << "'";
  else
    return false;

  return true;
}

bool MallocChecker::SummarizeRegion(raw_ostream &os,
                                    const MemRegion *MR) {
  switch (MR->getKind()) {
  case MemRegion::FunctionCodeRegionKind: {
    const NamedDecl *FD = cast<FunctionCodeRegion>(MR)->getDecl();
    if (FD)
      os << "the address of the function '" << *FD << '\'';
    else
      os << "the address of a function";
    return true;
  }
  case MemRegion::BlockCodeRegionKind:
    os << "block text";
    return true;
  case MemRegion::BlockDataRegionKind:
    // FIXME: where the block came from?
    os << "a block";
    return true;
  default: {
    const MemSpaceRegion *MS = MR->getMemorySpace();

    if (isa<StackLocalsSpaceRegion>(MS)) {
      const VarRegion *VR = dyn_cast<VarRegion>(MR);
      const VarDecl *VD;
      if (VR)
        VD = VR->getDecl();
      else
        VD = nullptr;

      if (VD)
        os << "the address of the local variable '" << VD->getName() << "'";
      else
        os << "the address of a local stack variable";
      return true;
    }

    if (isa<StackArgumentsSpaceRegion>(MS)) {
      const VarRegion *VR = dyn_cast<VarRegion>(MR);
      const VarDecl *VD;
      if (VR)
        VD = VR->getDecl();
      else
        VD = nullptr;

      if (VD)
        os << "the address of the parameter '" << VD->getName() << "'";
      else
        os << "the address of a parameter";
      return true;
    }

    if (isa<GlobalsSpaceRegion>(MS)) {
      const VarRegion *VR = dyn_cast<VarRegion>(MR);
      const VarDecl *VD;
      if (VR)
        VD = VR->getDecl();
      else
        VD = nullptr;

      if (VD) {
        if (VD->isStaticLocal())
          os << "the address of the static variable '" << VD->getName() << "'";
        else
          os << "the address of the global variable '" << VD->getName() << "'";
      } else
        os << "the address of a global variable";
      return true;
    }

    return false;
  }
  }
}

void MallocChecker::ReportBadFree(CheckerContext &C, SVal ArgVal,
                                  SourceRange Range,
                                  const Expr *DeallocExpr) const {

  if (!ChecksEnabled[CK_MallocChecker] &&
      !ChecksEnabled[CK_NewDeleteChecker])
    return;

  Optional<MallocChecker::CheckKind> CheckKind =
      getCheckIfTracked(C, DeallocExpr);
  if (!CheckKind.hasValue())
    return;

  if (ExplodedNode *N = C.generateErrorNode()) {
    if (!BT_BadFree[*CheckKind])
      BT_BadFree[*CheckKind].reset(new BugType(
          CheckNames[*CheckKind], "Bad free", categories::MemoryError));

    SmallString<100> buf;
    llvm::raw_svector_ostream os(buf);

    const MemRegion *MR = ArgVal.getAsRegion();
    while (const ElementRegion *ER = dyn_cast_or_null<ElementRegion>(MR))
      MR = ER->getSuperRegion();

    os << "Argument to ";
    if (!printAllocDeallocName(os, C, DeallocExpr))
      os << "deallocator";

    os << " is ";
    bool Summarized = MR ? SummarizeRegion(os, MR)
                         : SummarizeValue(os, ArgVal);
    if (Summarized)
      os << ", which is not memory allocated by ";
    else
      os << "not memory allocated by ";

    printExpectedAllocName(os, MemFunctionInfo, C, DeallocExpr);

    auto R = std::make_unique<PathSensitiveBugReport>(*BT_BadFree[*CheckKind],
                                                      os.str(), N);
    R->markInteresting(MR);
    R->addRange(Range);
    C.emitReport(std::move(R));
  }
}

void MallocChecker::ReportFreeAlloca(CheckerContext &C, SVal ArgVal,
                                     SourceRange Range) const {

  Optional<MallocChecker::CheckKind> CheckKind;

  if (ChecksEnabled[CK_MallocChecker])
    CheckKind = CK_MallocChecker;
  else if (ChecksEnabled[CK_MismatchedDeallocatorChecker])
    CheckKind = CK_MismatchedDeallocatorChecker;
  else
    return;

  if (ExplodedNode *N = C.generateErrorNode()) {
    if (!BT_FreeAlloca[*CheckKind])
      BT_FreeAlloca[*CheckKind].reset(new BugType(
          CheckNames[*CheckKind], "Free alloca()", categories::MemoryError));

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT_FreeAlloca[*CheckKind],
        "Memory allocated by alloca() should not be deallocated", N);
    R->markInteresting(ArgVal.getAsRegion());
    R->addRange(Range);
    C.emitReport(std::move(R));
  }
}

void MallocChecker::ReportMismatchedDealloc(CheckerContext &C,
                                            SourceRange Range,
                                            const Expr *DeallocExpr,
                                            const RefState *RS,
                                            SymbolRef Sym,
                                            bool OwnershipTransferred) const {

  if (!ChecksEnabled[CK_MismatchedDeallocatorChecker])
    return;

  if (ExplodedNode *N = C.generateErrorNode()) {
    if (!BT_MismatchedDealloc)
      BT_MismatchedDealloc.reset(
          new BugType(CheckNames[CK_MismatchedDeallocatorChecker],
                      "Bad deallocator", categories::MemoryError));

    SmallString<100> buf;
    llvm::raw_svector_ostream os(buf);

    const Expr *AllocExpr = cast<Expr>(RS->getStmt());
    SmallString<20> AllocBuf;
    llvm::raw_svector_ostream AllocOs(AllocBuf);
    SmallString<20> DeallocBuf;
    llvm::raw_svector_ostream DeallocOs(DeallocBuf);

    if (OwnershipTransferred) {
      if (printAllocDeallocName(DeallocOs, C, DeallocExpr))
        os << DeallocOs.str() << " cannot";
      else
        os << "Cannot";

      os << " take ownership of memory";

      if (printAllocDeallocName(AllocOs, C, AllocExpr))
        os << " allocated by " << AllocOs.str();
    } else {
      os << "Memory";
      if (printAllocDeallocName(AllocOs, C, AllocExpr))
        os << " allocated by " << AllocOs.str();

      os << " should be deallocated by ";
        printExpectedDeallocName(os, RS->getAllocationFamily());

      if (printAllocDeallocName(DeallocOs, C, DeallocExpr))
        os << ", not " << DeallocOs.str();
    }

    auto R = std::make_unique<PathSensitiveBugReport>(*BT_MismatchedDealloc,
                                                      os.str(), N);
    R->markInteresting(Sym);
    R->addRange(Range);
    R->addVisitor(std::make_unique<MallocBugVisitor>(Sym));
    C.emitReport(std::move(R));
  }
}

void MallocChecker::ReportOffsetFree(CheckerContext &C, SVal ArgVal,
                                     SourceRange Range, const Expr *DeallocExpr,
                                     const Expr *AllocExpr) const {


  if (!ChecksEnabled[CK_MallocChecker] &&
      !ChecksEnabled[CK_NewDeleteChecker])
    return;

  Optional<MallocChecker::CheckKind> CheckKind =
      getCheckIfTracked(C, AllocExpr);
  if (!CheckKind.hasValue())
    return;

  ExplodedNode *N = C.generateErrorNode();
  if (!N)
    return;

  if (!BT_OffsetFree[*CheckKind])
    BT_OffsetFree[*CheckKind].reset(new BugType(
        CheckNames[*CheckKind], "Offset free", categories::MemoryError));

  SmallString<100> buf;
  llvm::raw_svector_ostream os(buf);
  SmallString<20> AllocNameBuf;
  llvm::raw_svector_ostream AllocNameOs(AllocNameBuf);

  const MemRegion *MR = ArgVal.getAsRegion();
  assert(MR && "Only MemRegion based symbols can have offset free errors");

  RegionOffset Offset = MR->getAsOffset();
  assert((Offset.isValid() &&
          !Offset.hasSymbolicOffset() &&
          Offset.getOffset() != 0) &&
         "Only symbols with a valid offset can have offset free errors");

  int offsetBytes = Offset.getOffset() / C.getASTContext().getCharWidth();

  os << "Argument to ";
  if (!printAllocDeallocName(os, C, DeallocExpr))
    os << "deallocator";
  os << " is offset by "
     << offsetBytes
     << " "
     << ((abs(offsetBytes) > 1) ? "bytes" : "byte")
     << " from the start of ";
  if (AllocExpr && printAllocDeallocName(AllocNameOs, C, AllocExpr))
    os << "memory allocated by " << AllocNameOs.str();
  else
    os << "allocated memory";

  auto R = std::make_unique<PathSensitiveBugReport>(*BT_OffsetFree[*CheckKind],
                                                    os.str(), N);
  R->markInteresting(MR->getBaseRegion());
  R->addRange(Range);
  C.emitReport(std::move(R));
}

void MallocChecker::ReportUseAfterFree(CheckerContext &C, SourceRange Range,
                                       SymbolRef Sym) const {

  if (!ChecksEnabled[CK_MallocChecker] &&
      !ChecksEnabled[CK_NewDeleteChecker] &&
      !ChecksEnabled[CK_InnerPointerChecker])
    return;

  Optional<MallocChecker::CheckKind> CheckKind = getCheckIfTracked(C, Sym);
  if (!CheckKind.hasValue())
    return;

  if (ExplodedNode *N = C.generateErrorNode()) {
    if (!BT_UseFree[*CheckKind])
      BT_UseFree[*CheckKind].reset(new BugType(
          CheckNames[*CheckKind], "Use-after-free", categories::MemoryError));

    AllocationFamily AF =
        C.getState()->get<RegionState>(Sym)->getAllocationFamily();

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT_UseFree[*CheckKind],
        AF == AF_InnerBuffer
            ? "Inner pointer of container used after re/deallocation"
            : "Use of memory after it is freed",
        N);

    R->markInteresting(Sym);
    R->addRange(Range);
    R->addVisitor(std::make_unique<MallocBugVisitor>(Sym));

    if (AF == AF_InnerBuffer)
      R->addVisitor(allocation_state::getInnerPointerBRVisitor(Sym));

    C.emitReport(std::move(R));
  }
}

void MallocChecker::ReportDoubleFree(CheckerContext &C, SourceRange Range,
                                     bool Released, SymbolRef Sym,
                                     SymbolRef PrevSym) const {

  if (!ChecksEnabled[CK_MallocChecker] &&
      !ChecksEnabled[CK_NewDeleteChecker])
    return;

  Optional<MallocChecker::CheckKind> CheckKind = getCheckIfTracked(C, Sym);
  if (!CheckKind.hasValue())
    return;

  if (ExplodedNode *N = C.generateErrorNode()) {
    if (!BT_DoubleFree[*CheckKind])
      BT_DoubleFree[*CheckKind].reset(new BugType(
          CheckNames[*CheckKind], "Double free", categories::MemoryError));

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT_DoubleFree[*CheckKind],
        (Released ? "Attempt to free released memory"
                  : "Attempt to free non-owned memory"),
        N);
    R->addRange(Range);
    R->markInteresting(Sym);
    if (PrevSym)
      R->markInteresting(PrevSym);
    R->addVisitor(std::make_unique<MallocBugVisitor>(Sym));
    C.emitReport(std::move(R));
  }
}

void MallocChecker::ReportDoubleDelete(CheckerContext &C, SymbolRef Sym) const {

  if (!ChecksEnabled[CK_NewDeleteChecker])
    return;

  Optional<MallocChecker::CheckKind> CheckKind = getCheckIfTracked(C, Sym);
  if (!CheckKind.hasValue())
    return;

  if (ExplodedNode *N = C.generateErrorNode()) {
    if (!BT_DoubleDelete)
      BT_DoubleDelete.reset(new BugType(CheckNames[CK_NewDeleteChecker],
                                        "Double delete",
                                        categories::MemoryError));

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT_DoubleDelete, "Attempt to delete released memory", N);

    R->markInteresting(Sym);
    R->addVisitor(std::make_unique<MallocBugVisitor>(Sym));
    C.emitReport(std::move(R));
  }
}

void MallocChecker::ReportUseZeroAllocated(CheckerContext &C,
                                           SourceRange Range,
                                           SymbolRef Sym) const {

  if (!ChecksEnabled[CK_MallocChecker] &&
      !ChecksEnabled[CK_NewDeleteChecker])
    return;

  Optional<MallocChecker::CheckKind> CheckKind = getCheckIfTracked(C, Sym);

  if (!CheckKind.hasValue())
    return;

  if (ExplodedNode *N = C.generateErrorNode()) {
    if (!BT_UseZerroAllocated[*CheckKind])
      BT_UseZerroAllocated[*CheckKind].reset(
          new BugType(CheckNames[*CheckKind], "Use of zero allocated",
                      categories::MemoryError));

    auto R = std::make_unique<PathSensitiveBugReport>(
        *BT_UseZerroAllocated[*CheckKind], "Use of zero-allocated memory", N);

    R->addRange(Range);
    if (Sym) {
      R->markInteresting(Sym);
      R->addVisitor(std::make_unique<MallocBugVisitor>(Sym));
    }
    C.emitReport(std::move(R));
  }
}

void MallocChecker::ReportFunctionPointerFree(CheckerContext &C, SVal ArgVal,
                                              SourceRange Range,
                                              const Expr *FreeExpr) const {
  if (!ChecksEnabled[CK_MallocChecker])
    return;

  Optional<MallocChecker::CheckKind> CheckKind = getCheckIfTracked(C, FreeExpr);
  if (!CheckKind.hasValue())
    return;

  if (ExplodedNode *N = C.generateErrorNode()) {
    if (!BT_BadFree[*CheckKind])
      BT_BadFree[*CheckKind].reset(new BugType(
          CheckNames[*CheckKind], "Bad free", categories::MemoryError));

    SmallString<100> Buf;
    llvm::raw_svector_ostream Os(Buf);

    const MemRegion *MR = ArgVal.getAsRegion();
    while (const ElementRegion *ER = dyn_cast_or_null<ElementRegion>(MR))
      MR = ER->getSuperRegion();

    Os << "Argument to ";
    if (!printAllocDeallocName(Os, C, FreeExpr))
      Os << "deallocator";

    Os << " is a function pointer";

    auto R = std::make_unique<PathSensitiveBugReport>(*BT_BadFree[*CheckKind],
                                                      Os.str(), N);
    R->markInteresting(MR);
    R->addRange(Range);
    C.emitReport(std::move(R));
  }
}

ProgramStateRef MallocChecker::ReallocMemAux(CheckerContext &C,
                                             const CallExpr *CE,
                                             bool ShouldFreeOnFail,
                                             ProgramStateRef State,
                                             bool SuffixWithN) const {
  if (!State)
    return nullptr;

  if (SuffixWithN && CE->getNumArgs() < 3)
    return nullptr;
  else if (CE->getNumArgs() < 2)
    return nullptr;

  const Expr *arg0Expr = CE->getArg(0);
  SVal Arg0Val = C.getSVal(arg0Expr);
  if (!Arg0Val.getAs<DefinedOrUnknownSVal>())
    return nullptr;
  DefinedOrUnknownSVal arg0Val = Arg0Val.castAs<DefinedOrUnknownSVal>();

  SValBuilder &svalBuilder = C.getSValBuilder();

  DefinedOrUnknownSVal PtrEQ =
    svalBuilder.evalEQ(State, arg0Val, svalBuilder.makeNull());

  // Get the size argument.
  const Expr *Arg1 = CE->getArg(1);

  // Get the value of the size argument.
  SVal TotalSize = C.getSVal(Arg1);
  if (SuffixWithN)
    TotalSize = evalMulForBufferSize(C, Arg1, CE->getArg(2));
  if (!TotalSize.getAs<DefinedOrUnknownSVal>())
    return nullptr;

  // Compare the size argument to 0.
  DefinedOrUnknownSVal SizeZero =
    svalBuilder.evalEQ(State, TotalSize.castAs<DefinedOrUnknownSVal>(),
                       svalBuilder.makeIntValWithPtrWidth(0, false));

  ProgramStateRef StatePtrIsNull, StatePtrNotNull;
  std::tie(StatePtrIsNull, StatePtrNotNull) = State->assume(PtrEQ);
  ProgramStateRef StateSizeIsZero, StateSizeNotZero;
  std::tie(StateSizeIsZero, StateSizeNotZero) = State->assume(SizeZero);
  // We only assume exceptional states if they are definitely true; if the
  // state is under-constrained, assume regular realloc behavior.
  bool PrtIsNull = StatePtrIsNull && !StatePtrNotNull;
  bool SizeIsZero = StateSizeIsZero && !StateSizeNotZero;

  // If the ptr is NULL and the size is not 0, the call is equivalent to
  // malloc(size).
  if (PrtIsNull && !SizeIsZero) {
    ProgramStateRef stateMalloc = MallocMemAux(C, CE, TotalSize,
                                               UndefinedVal(), StatePtrIsNull);
    return stateMalloc;
  }

  if (PrtIsNull && SizeIsZero)
    return State;

  // Get the from and to pointer symbols as in toPtr = realloc(fromPtr, size).
  assert(!PrtIsNull);
  SymbolRef FromPtr = arg0Val.getAsSymbol();
  SVal RetVal = C.getSVal(CE);
  SymbolRef ToPtr = RetVal.getAsSymbol();
  if (!FromPtr || !ToPtr)
    return nullptr;

  bool IsKnownToBeAllocated = false;

  // If the size is 0, free the memory.
  if (SizeIsZero)
    // The semantics of the return value are:
    // If size was equal to 0, either NULL or a pointer suitable to be passed
    // to free() is returned. We just free the input pointer and do not add
    // any constrains on the output pointer.
    if (ProgramStateRef stateFree =
            FreeMemAux(C, CE, StateSizeIsZero, 0, false, IsKnownToBeAllocated))
      return stateFree;

  // Default behavior.
  if (ProgramStateRef stateFree =
          FreeMemAux(C, CE, State, 0, false, IsKnownToBeAllocated)) {

    ProgramStateRef stateRealloc = MallocMemAux(C, CE, TotalSize,
                                                UnknownVal(), stateFree);
    if (!stateRealloc)
      return nullptr;

    OwnershipAfterReallocKind Kind = OAR_ToBeFreedAfterFailure;
    if (ShouldFreeOnFail)
      Kind = OAR_FreeOnFailure;
    else if (!IsKnownToBeAllocated)
      Kind = OAR_DoNotTrackAfterFailure;

    // Record the info about the reallocated symbol so that we could properly
    // process failed reallocation.
    stateRealloc = stateRealloc->set<ReallocPairs>(ToPtr,
                                                   ReallocPair(FromPtr, Kind));
    // The reallocated symbol should stay alive for as long as the new symbol.
    C.getSymbolManager().addSymbolDependency(ToPtr, FromPtr);
    return stateRealloc;
  }
  return nullptr;
}

ProgramStateRef MallocChecker::CallocMem(CheckerContext &C, const CallExpr *CE,
                                         ProgramStateRef State) {
  if (!State)
    return nullptr;

  if (CE->getNumArgs() < 2)
    return nullptr;

  SValBuilder &svalBuilder = C.getSValBuilder();
  SVal zeroVal = svalBuilder.makeZeroVal(svalBuilder.getContext().CharTy);
  SVal TotalSize = evalMulForBufferSize(C, CE->getArg(0), CE->getArg(1));

  return MallocMemAux(C, CE, TotalSize, zeroVal, State);
}

MallocChecker::LeakInfo MallocChecker::getAllocationSite(const ExplodedNode *N,
                                                         SymbolRef Sym,
                                                         CheckerContext &C) {
  const LocationContext *LeakContext = N->getLocationContext();
  // Walk the ExplodedGraph backwards and find the first node that referred to
  // the tracked symbol.
  const ExplodedNode *AllocNode = N;
  const MemRegion *ReferenceRegion = nullptr;

  while (N) {
    ProgramStateRef State = N->getState();
    if (!State->get<RegionState>(Sym))
      break;

    // Find the most recent expression bound to the symbol in the current
    // context.
      if (!ReferenceRegion) {
        if (const MemRegion *MR = C.getLocationRegionIfPostStore(N)) {
          SVal Val = State->getSVal(MR);
          if (Val.getAsLocSymbol() == Sym) {
            const VarRegion* VR = MR->getBaseRegion()->getAs<VarRegion>();
            // Do not show local variables belonging to a function other than
            // where the error is reported.
            if (!VR ||
                (VR->getStackFrame() == LeakContext->getStackFrame()))
              ReferenceRegion = MR;
          }
        }
      }

    // Allocation node, is the last node in the current or parent context in
    // which the symbol was tracked.
    const LocationContext *NContext = N->getLocationContext();
    if (NContext == LeakContext ||
        NContext->isParentOf(LeakContext))
      AllocNode = N;
    N = N->pred_empty() ? nullptr : *(N->pred_begin());
  }

  return LeakInfo(AllocNode, ReferenceRegion);
}

void MallocChecker::reportLeak(SymbolRef Sym, ExplodedNode *N,
                               CheckerContext &C) const {

  if (!ChecksEnabled[CK_MallocChecker] &&
      !ChecksEnabled[CK_NewDeleteLeaksChecker])
    return;

  const RefState *RS = C.getState()->get<RegionState>(Sym);
  assert(RS && "cannot leak an untracked symbol");
  AllocationFamily Family = RS->getAllocationFamily();

  if (Family == AF_Alloca)
    return;

  Optional<MallocChecker::CheckKind>
      CheckKind = getCheckIfTracked(Family, true);

  if (!CheckKind.hasValue())
    return;

  assert(N);
  if (!BT_Leak[*CheckKind]) {
    // Leaks should not be reported if they are post-dominated by a sink:
    // (1) Sinks are higher importance bugs.
    // (2) NoReturnFunctionChecker uses sink nodes to represent paths ending
    //     with __noreturn functions such as assert() or exit(). We choose not
    //     to report leaks on such paths.
    BT_Leak[*CheckKind].reset(new BugType(CheckNames[*CheckKind], "Memory leak",
                                          categories::MemoryError,
                                          /*SuppressOnSink=*/true));
  }

  // Most bug reports are cached at the location where they occurred.
  // With leaks, we want to unique them by the location where they were
  // allocated, and only report a single path.
  PathDiagnosticLocation LocUsedForUniqueing;
  const ExplodedNode *AllocNode = nullptr;
  const MemRegion *Region = nullptr;
  std::tie(AllocNode, Region) = getAllocationSite(N, Sym, C);

  const Stmt *AllocationStmt = AllocNode->getStmtForDiagnostics();
  if (AllocationStmt)
    LocUsedForUniqueing = PathDiagnosticLocation::createBegin(AllocationStmt,
                                              C.getSourceManager(),
                                              AllocNode->getLocationContext());

  SmallString<200> buf;
  llvm::raw_svector_ostream os(buf);
  if (Region && Region->canPrintPretty()) {
    os << "Potential leak of memory pointed to by ";
    Region->printPretty(os);
  } else {
    os << "Potential memory leak";
  }

  auto R = std::make_unique<PathSensitiveBugReport>(
      *BT_Leak[*CheckKind], os.str(), N, LocUsedForUniqueing,
      AllocNode->getLocationContext()->getDecl());
  R->markInteresting(Sym);
  R->addVisitor(std::make_unique<MallocBugVisitor>(Sym, true));
  C.emitReport(std::move(R));
}

void MallocChecker::checkDeadSymbols(SymbolReaper &SymReaper,
                                     CheckerContext &C) const
{
  ProgramStateRef state = C.getState();
  RegionStateTy OldRS = state->get<RegionState>();
  RegionStateTy::Factory &F = state->get_context<RegionState>();

  RegionStateTy RS = OldRS;
  SmallVector<SymbolRef, 2> Errors;
  for (RegionStateTy::iterator I = RS.begin(), E = RS.end(); I != E; ++I) {
    if (SymReaper.isDead(I->first)) {
      if (I->second.isAllocated() || I->second.isAllocatedOfSizeZero())
        Errors.push_back(I->first);
      // Remove the dead symbol from the map.
      RS = F.remove(RS, I->first);
    }
  }

  if (RS == OldRS) {
    // We shouldn't have touched other maps yet.
    assert(state->get<ReallocPairs>() ==
           C.getState()->get<ReallocPairs>());
    assert(state->get<FreeReturnValue>() ==
           C.getState()->get<FreeReturnValue>());
    return;
  }

  // Cleanup the Realloc Pairs Map.
  ReallocPairsTy RP = state->get<ReallocPairs>();
  for (ReallocPairsTy::iterator I = RP.begin(), E = RP.end(); I != E; ++I) {
    if (SymReaper.isDead(I->first) ||
        SymReaper.isDead(I->second.ReallocatedSym)) {
      state = state->remove<ReallocPairs>(I->first);
    }
  }

  // Cleanup the FreeReturnValue Map.
  FreeReturnValueTy FR = state->get<FreeReturnValue>();
  for (FreeReturnValueTy::iterator I = FR.begin(), E = FR.end(); I != E; ++I) {
    if (SymReaper.isDead(I->first) ||
        SymReaper.isDead(I->second)) {
      state = state->remove<FreeReturnValue>(I->first);
    }
  }

  // Generate leak node.
  ExplodedNode *N = C.getPredecessor();
  if (!Errors.empty()) {
    static CheckerProgramPointTag Tag("MallocChecker", "DeadSymbolsLeak");
    N = C.generateNonFatalErrorNode(C.getState(), &Tag);
    if (N) {
      for (SmallVectorImpl<SymbolRef>::iterator
           I = Errors.begin(), E = Errors.end(); I != E; ++I) {
        reportLeak(*I, N, C);
      }
    }
  }

  C.addTransition(state->set<RegionState>(RS), N);
}

void MallocChecker::checkPreCall(const CallEvent &Call,
                                 CheckerContext &C) const {

  if (const CXXDestructorCall *DC = dyn_cast<CXXDestructorCall>(&Call)) {
    SymbolRef Sym = DC->getCXXThisVal().getAsSymbol();
    if (!Sym || checkDoubleDelete(Sym, C))
      return;
  }

  // We will check for double free in the post visit.
  if (const AnyFunctionCall *FC = dyn_cast<AnyFunctionCall>(&Call)) {
    const FunctionDecl *FD = FC->getDecl();
    if (!FD)
      return;

    ASTContext &Ctx = C.getASTContext();
    if (ChecksEnabled[CK_MallocChecker] &&
        (MemFunctionInfo.isCMemFunction(FD, Ctx, AF_Malloc,
                                        MemoryOperationKind::MOK_Free) ||
         MemFunctionInfo.isCMemFunction(FD, Ctx, AF_IfNameIndex,
                                        MemoryOperationKind::MOK_Free)))
      return;
  }

  // Check if the callee of a method is deleted.
  if (const CXXInstanceCall *CC = dyn_cast<CXXInstanceCall>(&Call)) {
    SymbolRef Sym = CC->getCXXThisVal().getAsSymbol();
    if (!Sym || checkUseAfterFree(Sym, C, CC->getCXXThisExpr()))
      return;
  }

  // Check arguments for being used after free.
  for (unsigned I = 0, E = Call.getNumArgs(); I != E; ++I) {
    SVal ArgSVal = Call.getArgSVal(I);
    if (ArgSVal.getAs<Loc>()) {
      SymbolRef Sym = ArgSVal.getAsSymbol();
      if (!Sym)
        continue;
      if (checkUseAfterFree(Sym, C, Call.getArgExpr(I)))
        return;
    }
  }
}

void MallocChecker::checkPreStmt(const ReturnStmt *S,
                                 CheckerContext &C) const {
  checkEscapeOnReturn(S, C);
}

// In the CFG, automatic destructors come after the return statement.
// This callback checks for returning memory that is freed by automatic
// destructors, as those cannot be reached in checkPreStmt().
void MallocChecker::checkEndFunction(const ReturnStmt *S,
                                     CheckerContext &C) const {
  checkEscapeOnReturn(S, C);
}

void MallocChecker::checkEscapeOnReturn(const ReturnStmt *S,
                                        CheckerContext &C) const {
  if (!S)
    return;

  const Expr *E = S->getRetValue();
  if (!E)
    return;

  // Check if we are returning a symbol.
  ProgramStateRef State = C.getState();
  SVal RetVal = C.getSVal(E);
  SymbolRef Sym = RetVal.getAsSymbol();
  if (!Sym)
    // If we are returning a field of the allocated struct or an array element,
    // the callee could still free the memory.
    // TODO: This logic should be a part of generic symbol escape callback.
    if (const MemRegion *MR = RetVal.getAsRegion())
      if (isa<FieldRegion>(MR) || isa<ElementRegion>(MR))
        if (const SymbolicRegion *BMR =
              dyn_cast<SymbolicRegion>(MR->getBaseRegion()))
          Sym = BMR->getSymbol();

  // Check if we are returning freed memory.
  if (Sym)
    checkUseAfterFree(Sym, C, E);
}

// TODO: Blocks should be either inlined or should call invalidate regions
// upon invocation. After that's in place, special casing here will not be
// needed.
void MallocChecker::checkPostStmt(const BlockExpr *BE,
                                  CheckerContext &C) const {

  // Scan the BlockDecRefExprs for any object the retain count checker
  // may be tracking.
  if (!BE->getBlockDecl()->hasCaptures())
    return;

  ProgramStateRef state = C.getState();
  const BlockDataRegion *R =
    cast<BlockDataRegion>(C.getSVal(BE).getAsRegion());

  BlockDataRegion::referenced_vars_iterator I = R->referenced_vars_begin(),
                                            E = R->referenced_vars_end();

  if (I == E)
    return;

  SmallVector<const MemRegion*, 10> Regions;
  const LocationContext *LC = C.getLocationContext();
  MemRegionManager &MemMgr = C.getSValBuilder().getRegionManager();

  for ( ; I != E; ++I) {
    const VarRegion *VR = I.getCapturedRegion();
    if (VR->getSuperRegion() == R) {
      VR = MemMgr.getVarRegion(VR->getDecl(), LC);
    }
    Regions.push_back(VR);
  }

  state =
    state->scanReachableSymbols<StopTrackingCallback>(Regions).getState();
  C.addTransition(state);
}

static bool isReleased(SymbolRef Sym, CheckerContext &C) {
  assert(Sym);
  const RefState *RS = C.getState()->get<RegionState>(Sym);
  return (RS && RS->isReleased());
}

bool MallocChecker::suppressDeallocationsInSuspiciousContexts(
    const CallExpr *CE, CheckerContext &C) const {
  if (CE->getNumArgs() == 0)
    return false;

  StringRef FunctionStr = "";
  if (const auto *FD = dyn_cast<FunctionDecl>(C.getStackFrame()->getDecl()))
    if (const Stmt *Body = FD->getBody())
      if (Body->getBeginLoc().isValid())
        FunctionStr =
            Lexer::getSourceText(CharSourceRange::getTokenRange(
                                     {FD->getBeginLoc(), Body->getBeginLoc()}),
                                 C.getSourceManager(), C.getLangOpts());

  // We do not model the Integer Set Library's retain-count based allocation.
  if (!FunctionStr.contains("__isl_"))
    return false;

  ProgramStateRef State = C.getState();

  for (const Expr *Arg : CE->arguments())
    if (SymbolRef Sym = C.getSVal(Arg).getAsSymbol())
      if (const RefState *RS = State->get<RegionState>(Sym))
        State = State->set<RegionState>(Sym, RefState::getEscaped(RS));

  C.addTransition(State);
  return true;
}

bool MallocChecker::checkUseAfterFree(SymbolRef Sym, CheckerContext &C,
                                      const Stmt *S) const {

  if (isReleased(Sym, C)) {
    ReportUseAfterFree(C, S->getSourceRange(), Sym);
    return true;
  }

  return false;
}

void MallocChecker::checkUseZeroAllocated(SymbolRef Sym, CheckerContext &C,
                                          const Stmt *S) const {
  assert(Sym);

  if (const RefState *RS = C.getState()->get<RegionState>(Sym)) {
    if (RS->isAllocatedOfSizeZero())
      ReportUseZeroAllocated(C, RS->getStmt()->getSourceRange(), Sym);
  }
  else if (C.getState()->contains<ReallocSizeZeroSymbols>(Sym)) {
    ReportUseZeroAllocated(C, S->getSourceRange(), Sym);
  }
}

bool MallocChecker::checkDoubleDelete(SymbolRef Sym, CheckerContext &C) const {

  if (isReleased(Sym, C)) {
    ReportDoubleDelete(C, Sym);
    return true;
  }
  return false;
}

// Check if the location is a freed symbolic region.
void MallocChecker::checkLocation(SVal l, bool isLoad, const Stmt *S,
                                  CheckerContext &C) const {
  SymbolRef Sym = l.getLocSymbolInBase();
  if (Sym) {
    checkUseAfterFree(Sym, C, S);
    checkUseZeroAllocated(Sym, C, S);
  }
}

// If a symbolic region is assumed to NULL (or another constant), stop tracking
// it - assuming that allocation failed on this path.
ProgramStateRef MallocChecker::evalAssume(ProgramStateRef state,
                                              SVal Cond,
                                              bool Assumption) const {
  RegionStateTy RS = state->get<RegionState>();
  for (RegionStateTy::iterator I = RS.begin(), E = RS.end(); I != E; ++I) {
    // If the symbol is assumed to be NULL, remove it from consideration.
    ConstraintManager &CMgr = state->getConstraintManager();
    ConditionTruthVal AllocFailed = CMgr.isNull(state, I.getKey());
    if (AllocFailed.isConstrainedTrue())
      state = state->remove<RegionState>(I.getKey());
  }

  // Realloc returns 0 when reallocation fails, which means that we should
  // restore the state of the pointer being reallocated.
  ReallocPairsTy RP = state->get<ReallocPairs>();
  for (ReallocPairsTy::iterator I = RP.begin(), E = RP.end(); I != E; ++I) {
    // If the symbol is assumed to be NULL, remove it from consideration.
    ConstraintManager &CMgr = state->getConstraintManager();
    ConditionTruthVal AllocFailed = CMgr.isNull(state, I.getKey());
    if (!AllocFailed.isConstrainedTrue())
      continue;

    SymbolRef ReallocSym = I.getData().ReallocatedSym;
    if (const RefState *RS = state->get<RegionState>(ReallocSym)) {
      if (RS->isReleased()) {
        switch (I.getData().Kind) {
        case OAR_ToBeFreedAfterFailure:
          state = state->set<RegionState>(ReallocSym,
              RefState::getAllocated(RS->getAllocationFamily(), RS->getStmt()));
          break;
        case OAR_DoNotTrackAfterFailure:
          state = state->remove<RegionState>(ReallocSym);
          break;
        default:
          assert(I.getData().Kind == OAR_FreeOnFailure);
        }
      }
    }
    state = state->remove<ReallocPairs>(I.getKey());
  }

  return state;
}

bool MallocChecker::mayFreeAnyEscapedMemoryOrIsModeledExplicitly(
                                              const CallEvent *Call,
                                              ProgramStateRef State,
                                              SymbolRef &EscapingSymbol) const {
  assert(Call);
  EscapingSymbol = nullptr;

  // For now, assume that any C++ or block call can free memory.
  // TODO: If we want to be more optimistic here, we'll need to make sure that
  // regions escape to C++ containers. They seem to do that even now, but for
  // mysterious reasons.
  if (!(isa<SimpleFunctionCall>(Call) || isa<ObjCMethodCall>(Call)))
    return true;

  // Check Objective-C messages by selector name.
  if (const ObjCMethodCall *Msg = dyn_cast<ObjCMethodCall>(Call)) {
    // If it's not a framework call, or if it takes a callback, assume it
    // can free memory.
    if (!Call->isInSystemHeader() || Call->argumentsMayEscape())
      return true;

    // If it's a method we know about, handle it explicitly post-call.
    // This should happen before the "freeWhenDone" check below.
    if (isKnownDeallocObjCMethodName(*Msg))
      return false;

    // If there's a "freeWhenDone" parameter, but the method isn't one we know
    // about, we can't be sure that the object will use free() to deallocate the
    // memory, so we can't model it explicitly. The best we can do is use it to
    // decide whether the pointer escapes.
    if (Optional<bool> FreeWhenDone = getFreeWhenDoneArg(*Msg))
      return *FreeWhenDone;

    // If the first selector piece ends with "NoCopy", and there is no
    // "freeWhenDone" parameter set to zero, we know ownership is being
    // transferred. Again, though, we can't be sure that the object will use
    // free() to deallocate the memory, so we can't model it explicitly.
    StringRef FirstSlot = Msg->getSelector().getNameForSlot(0);
    if (FirstSlot.endswith("NoCopy"))
      return true;

    // If the first selector starts with addPointer, insertPointer,
    // or replacePointer, assume we are dealing with NSPointerArray or similar.
    // This is similar to C++ containers (vector); we still might want to check
    // that the pointers get freed by following the container itself.
    if (FirstSlot.startswith("addPointer") ||
        FirstSlot.startswith("insertPointer") ||
        FirstSlot.startswith("replacePointer") ||
        FirstSlot.equals("valueWithPointer")) {
      return true;
    }

    // We should escape receiver on call to 'init'. This is especially relevant
    // to the receiver, as the corresponding symbol is usually not referenced
    // after the call.
    if (Msg->getMethodFamily() == OMF_init) {
      EscapingSymbol = Msg->getReceiverSVal().getAsSymbol();
      return true;
    }

    // Otherwise, assume that the method does not free memory.
    // Most framework methods do not free memory.
    return false;
  }

  // At this point the only thing left to handle is straight function calls.
  const FunctionDecl *FD = cast<SimpleFunctionCall>(Call)->getDecl();
  if (!FD)
    return true;

  ASTContext &ASTC = State->getStateManager().getContext();

  // If it's one of the allocation functions we can reason about, we model
  // its behavior explicitly.
  if (MemFunctionInfo.isMemFunction(FD, ASTC))
    return false;

  // If it's not a system call, assume it frees memory.
  if (!Call->isInSystemHeader())
    return true;

  // White list the system functions whose arguments escape.
  const IdentifierInfo *II = FD->getIdentifier();
  if (!II)
    return true;
  StringRef FName = II->getName();

  // White list the 'XXXNoCopy' CoreFoundation functions.
  // We specifically check these before
  if (FName.endswith("NoCopy")) {
    // Look for the deallocator argument. We know that the memory ownership
    // is not transferred only if the deallocator argument is
    // 'kCFAllocatorNull'.
    for (unsigned i = 1; i < Call->getNumArgs(); ++i) {
      const Expr *ArgE = Call->getArgExpr(i)->IgnoreParenCasts();
      if (const DeclRefExpr *DE = dyn_cast<DeclRefExpr>(ArgE)) {
        StringRef DeallocatorName = DE->getFoundDecl()->getName();
        if (DeallocatorName == "kCFAllocatorNull")
          return false;
      }
    }
    return true;
  }

  // Associating streams with malloced buffers. The pointer can escape if
  // 'closefn' is specified (and if that function does free memory),
  // but it will not if closefn is not specified.
  // Currently, we do not inspect the 'closefn' function (PR12101).
  if (FName == "funopen")
    if (Call->getNumArgs() >= 4 && Call->getArgSVal(4).isConstant(0))
      return false;

  // Do not warn on pointers passed to 'setbuf' when used with std streams,
  // these leaks might be intentional when setting the buffer for stdio.
  // http://stackoverflow.com/questions/2671151/who-frees-setvbuf-buffer
  if (FName == "setbuf" || FName =="setbuffer" ||
      FName == "setlinebuf" || FName == "setvbuf") {
    if (Call->getNumArgs() >= 1) {
      const Expr *ArgE = Call->getArgExpr(0)->IgnoreParenCasts();
      if (const DeclRefExpr *ArgDRE = dyn_cast<DeclRefExpr>(ArgE))
        if (const VarDecl *D = dyn_cast<VarDecl>(ArgDRE->getDecl()))
          if (D->getCanonicalDecl()->getName().find("std") != StringRef::npos)
            return true;
    }
  }

  // A bunch of other functions which either take ownership of a pointer or
  // wrap the result up in a struct or object, meaning it can be freed later.
  // (See RetainCountChecker.) Not all the parameters here are invalidated,
  // but the Malloc checker cannot differentiate between them. The right way
  // of doing this would be to implement a pointer escapes callback.
  if (FName == "CGBitmapContextCreate" ||
      FName == "CGBitmapContextCreateWithData" ||
      FName == "CVPixelBufferCreateWithBytes" ||
      FName == "CVPixelBufferCreateWithPlanarBytes" ||
      FName == "OSAtomicEnqueue") {
    return true;
  }

  if (FName == "postEvent" &&
      FD->getQualifiedNameAsString() == "QCoreApplication::postEvent") {
    return true;
  }

  if (FName == "postEvent" &&
      FD->getQualifiedNameAsString() == "QCoreApplication::postEvent") {
    return true;
  }

  if (FName == "connectImpl" &&
      FD->getQualifiedNameAsString() == "QObject::connectImpl") {
    return true;
  }

  // Handle cases where we know a buffer's /address/ can escape.
  // Note that the above checks handle some special cases where we know that
  // even though the address escapes, it's still our responsibility to free the
  // buffer.
  if (Call->argumentsMayEscape())
    return true;

  // Otherwise, assume that the function does not free memory.
  // Most system calls do not free the memory.
  return false;
}

ProgramStateRef MallocChecker::checkPointerEscape(ProgramStateRef State,
                                             const InvalidatedSymbols &Escaped,
                                             const CallEvent *Call,
                                             PointerEscapeKind Kind) const {
  return checkPointerEscapeAux(State, Escaped, Call, Kind,
                               /*IsConstPointerEscape*/ false);
}

ProgramStateRef MallocChecker::checkConstPointerEscape(ProgramStateRef State,
                                              const InvalidatedSymbols &Escaped,
                                              const CallEvent *Call,
                                              PointerEscapeKind Kind) const {
  // If a const pointer escapes, it may not be freed(), but it could be deleted.
  return checkPointerEscapeAux(State, Escaped, Call, Kind,
                               /*IsConstPointerEscape*/ true);
}

static bool checkIfNewOrNewArrayFamily(const RefState *RS) {
  return (RS->getAllocationFamily() == AF_CXXNewArray ||
          RS->getAllocationFamily() == AF_CXXNew);
}

ProgramStateRef MallocChecker::checkPointerEscapeAux(
    ProgramStateRef State, const InvalidatedSymbols &Escaped,
    const CallEvent *Call, PointerEscapeKind Kind,
    bool IsConstPointerEscape) const {
  // If we know that the call does not free memory, or we want to process the
  // call later, keep tracking the top level arguments.
  SymbolRef EscapingSymbol = nullptr;
  if (Kind == PSK_DirectEscapeOnCall &&
      !mayFreeAnyEscapedMemoryOrIsModeledExplicitly(Call, State,
                                                    EscapingSymbol) &&
      !EscapingSymbol) {
    return State;
  }

  for (InvalidatedSymbols::const_iterator I = Escaped.begin(),
       E = Escaped.end();
       I != E; ++I) {
    SymbolRef sym = *I;

    if (EscapingSymbol && EscapingSymbol != sym)
      continue;

    if (const RefState *RS = State->get<RegionState>(sym))
      if (RS->isAllocated() || RS->isAllocatedOfSizeZero())
        if (!IsConstPointerEscape || checkIfNewOrNewArrayFamily(RS))
          State = State->set<RegionState>(sym, RefState::getEscaped(RS));
  }
  return State;
}

static SymbolRef findFailedReallocSymbol(ProgramStateRef currState,
                                         ProgramStateRef prevState) {
  ReallocPairsTy currMap = currState->get<ReallocPairs>();
  ReallocPairsTy prevMap = prevState->get<ReallocPairs>();

  for (const ReallocPairsTy::value_type &Pair : prevMap) {
    SymbolRef sym = Pair.first;
    if (!currMap.lookup(sym))
      return sym;
  }

  return nullptr;
}

static bool isReferenceCountingPointerDestructor(const CXXDestructorDecl *DD) {
  if (const IdentifierInfo *II = DD->getParent()->getIdentifier()) {
    StringRef N = II->getName();
    if (N.contains_lower("ptr") || N.contains_lower("pointer")) {
      if (N.contains_lower("ref") || N.contains_lower("cnt") ||
          N.contains_lower("intrusive") || N.contains_lower("shared")) {
        return true;
      }
    }
  }
  return false;
}

PathDiagnosticPieceRef MallocBugVisitor::VisitNode(const ExplodedNode *N,
                                                   BugReporterContext &BRC,
                                                   PathSensitiveBugReport &BR) {
  ProgramStateRef state = N->getState();
  ProgramStateRef statePrev = N->getFirstPred()->getState();

  const RefState *RSCurr = state->get<RegionState>(Sym);
  const RefState *RSPrev = statePrev->get<RegionState>(Sym);

  const Stmt *S = N->getStmtForDiagnostics();
  // When dealing with containers, we sometimes want to give a note
  // even if the statement is missing.
  if (!S && (!RSCurr || RSCurr->getAllocationFamily() != AF_InnerBuffer))
    return nullptr;

  const LocationContext *CurrentLC = N->getLocationContext();

  // If we find an atomic fetch_add or fetch_sub within the destructor in which
  // the pointer was released (before the release), this is likely a destructor
  // of a shared pointer.
  // Because we don't model atomics, and also because we don't know that the
  // original reference count is positive, we should not report use-after-frees
  // on objects deleted in such destructors. This can probably be improved
  // through better shared pointer modeling.
  if (ReleaseDestructorLC) {
    if (const auto *AE = dyn_cast<AtomicExpr>(S)) {
      AtomicExpr::AtomicOp Op = AE->getOp();
      if (Op == AtomicExpr::AO__c11_atomic_fetch_add ||
          Op == AtomicExpr::AO__c11_atomic_fetch_sub) {
        if (ReleaseDestructorLC == CurrentLC ||
            ReleaseDestructorLC->isParentOf(CurrentLC)) {
          BR.markInvalid(getTag(), S);
        }
      }
    }
  }

  // FIXME: We will eventually need to handle non-statement-based events
  // (__attribute__((cleanup))).

  // Find out if this is an interesting point and what is the kind.
  StringRef Msg;
  std::unique_ptr<StackHintGeneratorForSymbol> StackHint = nullptr;
  SmallString<256> Buf;
  llvm::raw_svector_ostream OS(Buf);

  if (Mode == Normal) {
    if (isAllocated(RSCurr, RSPrev, S)) {
      Msg = "Memory is allocated";
      StackHint = std::make_unique<StackHintGeneratorForSymbol>(
          Sym, "Returned allocated memory");
    } else if (isReleased(RSCurr, RSPrev, S)) {
      const auto Family = RSCurr->getAllocationFamily();
      switch (Family) {
        case AF_Alloca:
        case AF_Malloc:
        case AF_CXXNew:
        case AF_CXXNewArray:
        case AF_IfNameIndex:
          Msg = "Memory is released";
          StackHint = std::make_unique<StackHintGeneratorForSymbol>(
              Sym, "Returning; memory was released");
          break;
        case AF_InnerBuffer: {
          const MemRegion *ObjRegion =
              allocation_state::getContainerObjRegion(statePrev, Sym);
          const auto *TypedRegion = cast<TypedValueRegion>(ObjRegion);
          QualType ObjTy = TypedRegion->getValueType();
          OS << "Inner buffer of '" << ObjTy.getAsString() << "' ";

          if (N->getLocation().getKind() == ProgramPoint::PostImplicitCallKind) {
            OS << "deallocated by call to destructor";
            StackHint = std::make_unique<StackHintGeneratorForSymbol>(
                Sym, "Returning; inner buffer was deallocated");
          } else {
            OS << "reallocated by call to '";
            const Stmt *S = RSCurr->getStmt();
            if (const auto *MemCallE = dyn_cast<CXXMemberCallExpr>(S)) {
              OS << MemCallE->getMethodDecl()->getNameAsString();
            } else if (const auto *OpCallE = dyn_cast<CXXOperatorCallExpr>(S)) {
              OS << OpCallE->getDirectCallee()->getNameAsString();
            } else if (const auto *CallE = dyn_cast<CallExpr>(S)) {
              auto &CEMgr = BRC.getStateManager().getCallEventManager();
              CallEventRef<> Call = CEMgr.getSimpleCall(CallE, state, CurrentLC);
              const auto *D = dyn_cast_or_null<NamedDecl>(Call->getDecl());
              OS << (D ? D->getNameAsString() : "unknown");
            }
            OS << "'";
            StackHint = std::make_unique<StackHintGeneratorForSymbol>(
                Sym, "Returning; inner buffer was reallocated");
          }
          Msg = OS.str();
          break;
        }
        case AF_None:
          llvm_unreachable("Unhandled allocation family!");
      }

      // See if we're releasing memory while inlining a destructor
      // (or one of its callees). This turns on various common
      // false positive suppressions.
      bool FoundAnyDestructor = false;
      for (const LocationContext *LC = CurrentLC; LC; LC = LC->getParent()) {
        if (const auto *DD = dyn_cast<CXXDestructorDecl>(LC->getDecl())) {
          if (isReferenceCountingPointerDestructor(DD)) {
            // This immediately looks like a reference-counting destructor.
            // We're bad at guessing the original reference count of the object,
            // so suppress the report for now.
            BR.markInvalid(getTag(), DD);
          } else if (!FoundAnyDestructor) {
            assert(!ReleaseDestructorLC &&
                   "There can be only one release point!");
            // Suspect that it's a reference counting pointer destructor.
            // On one of the next nodes might find out that it has atomic
            // reference counting operations within it (see the code above),
            // and if so, we'd conclude that it likely is a reference counting
            // pointer destructor.
            ReleaseDestructorLC = LC->getStackFrame();
            // It is unlikely that releasing memory is delegated to a destructor
            // inside a destructor of a shared pointer, because it's fairly hard
            // to pass the information that the pointer indeed needs to be
            // released into it. So we're only interested in the innermost
            // destructor.
            FoundAnyDestructor = true;
          }
        }
      }
    } else if (isRelinquished(RSCurr, RSPrev, S)) {
      Msg = "Memory ownership is transferred";
      StackHint = std::make_unique<StackHintGeneratorForSymbol>(Sym, "");
    } else if (hasReallocFailed(RSCurr, RSPrev, S)) {
      Mode = ReallocationFailed;
      Msg = "Reallocation failed";
      StackHint = std::make_unique<StackHintGeneratorForReallocationFailed>(
          Sym, "Reallocation failed");

      if (SymbolRef sym = findFailedReallocSymbol(state, statePrev)) {
        // Is it possible to fail two reallocs WITHOUT testing in between?
        assert((!FailedReallocSymbol || FailedReallocSymbol == sym) &&
          "We only support one failed realloc at a time.");
        BR.markInteresting(sym);
        FailedReallocSymbol = sym;
      }
    }

  // We are in a special mode if a reallocation failed later in the path.
  } else if (Mode == ReallocationFailed) {
    assert(FailedReallocSymbol && "No symbol to look for.");

    // Is this is the first appearance of the reallocated symbol?
    if (!statePrev->get<RegionState>(FailedReallocSymbol)) {
      // We're at the reallocation point.
      Msg = "Attempt to reallocate memory";
      StackHint = std::make_unique<StackHintGeneratorForSymbol>(
          Sym, "Returned reallocated memory");
      FailedReallocSymbol = nullptr;
      Mode = Normal;
    }
  }

  if (Msg.empty()) {
    assert(!StackHint);
    return nullptr;
  }

  assert(StackHint);

  // Generate the extra diagnostic.
  PathDiagnosticLocation Pos;
  if (!S) {
    assert(RSCurr->getAllocationFamily() == AF_InnerBuffer);
    auto PostImplCall = N->getLocation().getAs<PostImplicitCall>();
    if (!PostImplCall)
      return nullptr;
    Pos = PathDiagnosticLocation(PostImplCall->getLocation(),
                                 BRC.getSourceManager());
  } else {
    Pos = PathDiagnosticLocation(S, BRC.getSourceManager(),
                                 N->getLocationContext());
  }

  auto P = std::make_shared<PathDiagnosticEventPiece>(Pos, Msg, true);
  BR.addCallStackHint(P, std::move(StackHint));
  return P;
}

void MallocChecker::printState(raw_ostream &Out, ProgramStateRef State,
                               const char *NL, const char *Sep) const {

  RegionStateTy RS = State->get<RegionState>();

  if (!RS.isEmpty()) {
    Out << Sep << "MallocChecker :" << NL;
    for (RegionStateTy::iterator I = RS.begin(), E = RS.end(); I != E; ++I) {
      const RefState *RefS = State->get<RegionState>(I.getKey());
      AllocationFamily Family = RefS->getAllocationFamily();
      Optional<MallocChecker::CheckKind> CheckKind = getCheckIfTracked(Family);
      if (!CheckKind.hasValue())
         CheckKind = getCheckIfTracked(Family, true);

      I.getKey()->dumpToStream(Out);
      Out << " : ";
      I.getData().dump(Out);
      if (CheckKind.hasValue())
        Out << " (" << CheckNames[*CheckKind].getName() << ")";
      Out << NL;
    }
  }
}

namespace clang {
namespace ento {
namespace allocation_state {

ProgramStateRef
markReleased(ProgramStateRef State, SymbolRef Sym, const Expr *Origin) {
  AllocationFamily Family = AF_InnerBuffer;
  return State->set<RegionState>(Sym, RefState::getReleased(Family, Origin));
}

} // end namespace allocation_state
} // end namespace ento
} // end namespace clang

// Intended to be used in InnerPointerChecker to register the part of
// MallocChecker connected to it.
void ento::registerInnerPointerCheckerAux(CheckerManager &mgr) {
  MallocChecker *checker = mgr.getChecker<MallocChecker>();
  checker->ChecksEnabled[MallocChecker::CK_InnerPointerChecker] = true;
  checker->CheckNames[MallocChecker::CK_InnerPointerChecker] =
      mgr.getCurrentCheckerName();
}

void ento::registerDynamicMemoryModeling(CheckerManager &mgr) {
  auto *checker = mgr.registerChecker<MallocChecker>();
  checker->MemFunctionInfo.ShouldIncludeOwnershipAnnotatedFunctions =
      mgr.getAnalyzerOptions().getCheckerBooleanOption(checker, "Optimistic");
}

bool ento::shouldRegisterDynamicMemoryModeling(const LangOptions &LO) {
  return true;
}

#define REGISTER_CHECKER(name)                                                 \
  void ento::register##name(CheckerManager &mgr) {                             \
    MallocChecker *checker = mgr.getChecker<MallocChecker>();                  \
    checker->ChecksEnabled[MallocChecker::CK_##name] = true;                   \
    checker->CheckNames[MallocChecker::CK_##name] =                            \
        mgr.getCurrentCheckerName();                                           \
  }                                                                            \
                                                                               \
  bool ento::shouldRegister##name(const LangOptions &LO) { return true; }

REGISTER_CHECKER(MallocChecker)
REGISTER_CHECKER(NewDeleteChecker)
REGISTER_CHECKER(NewDeleteLeaksChecker)
REGISTER_CHECKER(MismatchedDeallocatorChecker)
