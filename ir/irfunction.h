//===-- ir/irfunction.h - Codegen state for D functions ---------*- C++ -*-===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//
//
// Represents the state of a D function/method/... on its way through the
// codegen process.
//
//===----------------------------------------------------------------------===//


#ifndef LDC_IR_IRFUNCTION_H
#define LDC_IR_IRFUNCTION_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "gen/llvm.h"
#include "gen/irstate.h"
#include "ir/irfuncty.h"
#include <map>
#include <stack>
#include <vector>

class Identifier;
class Statement;

/// Represents a position on the stack of currently active cleanup scopes.
///
/// Since we always need to run a contiguous part of the stack (or all) in
/// order, two cursors (one of which is usually the currently top of the stack)
/// are enough to identify a sequence of cleanups to run.
typedef size_t CleanupCursor;

/// Stores information needed to correctly jump to a given label or loop/switch
/// statement (break/continue can be labeled, but are not necessarily).
struct JumpTarget {
    /// The basic block to ultimately branch to.
    llvm::BasicBlock* targetBlock;

    /// The index of the target in the stack of active cleanup scopes.
    ///
    /// When generating code for a jump to this label, the cleanups between
    /// the current depth and that of the level will be emitted. Note that
    /// we need to handle only one direction (towards the root of the stack)
    /// because D forbids gotos into try or finally blocks.
    // TODO: We might not be able to detect illegal jumps across try-finally
    // blocks by only storing the index.
    CleanupCursor cleanupScope;

    /// Keeps target of the associated loop or switch statement so we can
    /// handle both unlabeled and labeled jumps.
    Statement* targetStatement;
};

/// Keeps track of source and target label of a goto.
///
/// Used if we cannot immediately emit all the code for a jump because we have
/// not generated code for the target yet.
struct GotoJump {
    // The location of the goto instruction, for error reporting.
    Loc sourceLoc;

    /// The basic block which contains the goto as its terminator.
    llvm::BasicBlock* sourceBlock;

    /// While we have not found the actual branch target, we might need to
    /// create a "fake" basic block in order to be able to execute the cleanups
    /// (we do not keep branching information around after leaving the scope).
    llvm::BasicBlock* tentativeTarget;

    /// The label to target with the goto.
    Identifier* targetLabel;
};

/// Describes a particular way to leave a cleanup scope and continue execution
/// with another one.
///
/// In general, there can be multiple ones (normal exit, early returns,
/// breaks/continues, exceptions, and so on).
struct CleanupExitTarget {
    explicit CleanupExitTarget(llvm::BasicBlock* t) : branchTarget(t) {}

    /// The target basic block to branch to after running the cleanup.
    llvm::BasicBlock* branchTarget;

    /// The basic blocks that want to continue with this target after running
    /// the cleanup. We need to keep this information around so we can insert
    /// stores to the branch selector variable when converting from one to two
    /// targets.
    std::vector<llvm::BasicBlock*> sourceBlocks;
};

/// Represents a scope (in abstract terms, not curly braces) that requires a
/// piece of cleanup code to be run whenever it is left, whether as part of
/// normal control flow or exception unwinding.
///
/// This includes finally blocks (which are also generated by the frontend for
/// running the destructors of non-temporary variables) and the destructors of
/// temporaries (which are unfortunately not lowered by the frontend).
///
/// Our goal is to only emit each cleanup once such as to avoid generating an
/// exponential number of basic blocks/landing pads for handling all the
/// different ways of exiting a deeply nested scope (consider e.g. ten
/// local variables with destructors, each of which might throw itself).
class CleanupScope {
public:
    CleanupScope(llvm::BasicBlock* beginBlock, llvm::BasicBlock* endBlock) :
        beginBlock(beginBlock), endBlock(endBlock), branchSelector(0) {}

    /// The basic block to branch to for running the cleanup.
    llvm::BasicBlock* beginBlock;

    /// The basic block that contains the end of the cleanup code (is different
    /// from beginBlock if the cleanup contains control flow).
    llvm::BasicBlock* endBlock;

    /// The branch selector variable, or null if not created yet.
    llvm::AllocaInst* branchSelector;

    /// Stores all possible targets blocks after running this cleanup, along
    /// with what predecessors want to continue at that target. The index in
    /// the vector corresponds to the branch selector value for that target.
    // Note: This is of course a bad choice of data structure for many targets
    // complexity-wise. However, situations where this matters should be
    // exceedingly rare in both hand-written as well as generated code.
    std::vector<CleanupExitTarget> exitTargets;

    /// Keeps track of all the gotos originating from somewhere inside this
    /// scope for which we have not found the label yet (because it occurs
    /// lexically later in the function).
    // Note: Should also be a dense map from source block to the rest of the
    // data if we expect many gotos.
    std::vector<GotoJump> unresolvedGotos;

    /// Caches landing pads generated for catches at this cleanup scope level.
    ///
    /// One element is pushed to the back on each time a catch block is entered,
    /// and popped again once it is left. If the corresponding landing pad has
    /// not been generated yet (this is done lazily), the pointer is null.
    std::vector<llvm::BasicBlock*> landingPads;
};

/// Stores information to be able to branch to a catch clause if it matches.
///
/// Each catch body is emitted only once, but may be target from many landing
/// pads (in case of nested catch or cleanup scopes).
struct CatchScope {
    /// The ClassInfo reference corresponding to the type to match the
    /// exception object against.
    llvm::Constant* classInfoPtr;

    /// The block to branch to if the exception type matches.
    llvm::BasicBlock* bodyBlock;

    /// The cleanup scope stack level corresponding to this catch.
    CleanupCursor cleanupScope;
};

/// Keeps track of active (abstract) scopes in a function that influence code
/// generation of their contents. This includes cleanups (finally blocks,
/// destructors), try/catch blocks and labels for goto/break/continue.
///
/// Note that the entire code generation process, and this class in particular,
/// depends heavily on the fact that we visit the statement/expression tree in
/// its natural order, i.e. depth-first and in lexical order. In other words,
/// the code here expects that after a cleanup/catch/loop/etc. has been pushed,
/// the contents of the block are generated, and it is then popped again
/// afterwards. This is also encoded in the fact that none of the methods for
/// branching/running cleanups take a cursor for describing the "source" scope,
/// it is always assumed to be the current one.
///
/// Handling of break/continue could be moved into a separate layer that uses
/// the rest of the ScopeStack API, as it (in contrast to goto) never requires
/// resolving forward references across cleanup scopes.
class ScopeStack {
public:
    ScopeStack(IRState* irs) : catchBlockCount(0), irs(irs) {}
    ~ScopeStack();

    /// Registers a piece of cleanup code to be run.
    ///
    /// The end block is expected not to contain a terminator yet. It will be
    /// added by ScopeStack as needed, based on what follow-up blocks code from
    /// within this scope will branch to.
    void pushCleanup(llvm::BasicBlock* beginBlock, llvm::BasicBlock* endBlock);

    /// Terminates the current basic block with a branch to the cleanups needed
    /// for leaving the current scope and continuing execution at the target
    /// scope stack level.
    ///
    /// After running them, execution will branch to the given basic block.
    void runCleanups(CleanupCursor targetScope, llvm::BasicBlock* continueWith) {
        runCleanups(currentCleanupScope(), targetScope, continueWith);
    }

    /// Like #runCleanups(), but runs all of them until the top-level scope is
    /// reached.
    void runAllCleanups(llvm::BasicBlock* continueWith);

    /// Pops all the cleanups between the current scope and the target cursor.
    ///
    /// This does not insert any cleanup calls, use #runCleanups() beforehand.
    void popCleanups(CleanupCursor targetScope);

    /// Returns a cursor that identifies the current cleanup scope, to be later
    /// used with #runCleanups() et al.
    ///
    /// Note that this cursor is only valid as long as the current scope is not
    /// popped.
    CleanupCursor currentCleanupScope() { return cleanupScopes.size(); }

    /// Registers a catch block to be taken into consideration when an exception
    /// is thrown within the current scope.
    ///
    /// When a potentially throwing function call is emitted, a landing pad will
    /// be emitted to compare the dynamic type info of the exception against the
    /// given ClassInfo constant and to branch to the given body block if it
    /// matches. The registered catch blocks are maintained on a stack, with the
    /// top-most (i.e. last pushed, innermost) taking precedence.
    void pushCatch(llvm::Constant* classInfoPtr, llvm::BasicBlock* bodyBlock);

    /// Unregisters the last registered catch block.
    void popCatch();

    /// Registers a loop statement to be used as a target for break/continue
    /// statements in the current scope.
    void pushLoopTarget(Statement* loopStatement, llvm::BasicBlock* continueTarget,
        llvm::BasicBlock* breakTarget);

    /// Pops the last pushed loop target, so it is no longer taken into
    /// consideration for resolving breaks/continues.
    void popLoopTarget();

    /// Registers a statement to be used as a target for break statements in the
    /// current scope (currently applies only to switch statements).
    void pushBreakTarget(Statement* switchStatement, llvm::BasicBlock* targetBlock);

    /// Unregisters the last registered break target.
    void popBreakTarget();

    /// Adds a label to serve as a target for goto statements.
    ///
    /// Also causes in-flight forward references to this label to be resolved.
    void addLabelTarget(Identifier* labelName, llvm::BasicBlock* targetBlock);

    /// Emits a call or invoke to the given callee, depending on whether there
    /// are catches/cleanups active or not.
    template <typename T>
    llvm::CallSite callOrInvoke(llvm::Value* callee, const T &args,
        const char* name = "");

    /// Determines whether a given callee will be an invoke (same criteria as
    /// callOrInvoke)
    bool doesCalleeNeedInvoke(llvm::Function* calleeFn);

    /// Terminates the current basic block with an unconditional branch to the
    /// given label, along with the cleanups to execute on the way there.
    ///
    /// Legal forward references (i.e. within the same function, and not into
    /// a cleanup scope) will be resolved.
    void jumpToLabel(Loc loc, Identifier* labelName);

    /// Terminates the current basic block with an unconditional branch to the
    /// continue target generated by the given loop statement, along with
    /// the cleanups to execute on the way there.
    void continueWithLoop(Statement* loopStatement) {
        jumpToStatement(continueTargets, loopStatement);
    }

    /// Terminates the current basic block with an unconditional branch to the
    /// closest loop continue target, along with the cleanups to execute on
    /// the way there.
    void continueWithClosest() {
        jumpToClosest(continueTargets);
    }

    /// Terminates the current basic block with an unconditional branch to the
    /// break target generated by the given loop or switch statement, along with
    /// the cleanups to execute on the way there.
    void breakToStatement(Statement* loopOrSwitchStatement) {
        jumpToStatement(breakTargets, loopOrSwitchStatement);
    }

    /// Terminates the current basic block with an unconditional branch to the
    /// closest break statement target, along with the cleanups to execute on
    /// the way there.
    void breakToClosest() {
        jumpToClosest(breakTargets);
    }

    /// !HACK!: Used to force an invoke (so that calls in catch blocks get unwound)
    int catchBlockCount;

private:
    /// Internal version that allows specifying the scope at which to start
    /// emitting the cleanups.
    void runCleanups(CleanupCursor sourceScope, CleanupCursor targetScope,
        llvm::BasicBlock* continueWith);

    std::vector<GotoJump>& currentUnresolvedGotos();

    std::vector<llvm::BasicBlock*>& currentLandingPads();

    /// Emits a landing pad to honor all the active cleanups and catches.
    llvm::BasicBlock* emitLandingPad();

    /// Unified implementation for labeled break/continue.
    void jumpToStatement(std::vector<JumpTarget>& targets, Statement* loopOrSwitchStatement);

    /// Unified implementation for unlabeled break/continue.
    void jumpToClosest(std::vector<JumpTarget>& targets);

    /// The ambient IRState. For legacy reasons, there is currently a cyclic
    /// dependency between the two.
    IRState* irs;

    typedef llvm::DenseMap<Identifier*, JumpTarget> LabelTargetMap;
    /// The labels we have encountered in this function so far, accessed by
    /// their associated identifier (i.e. the name of the label).
    LabelTargetMap labelTargets;

    ///
    std::vector<JumpTarget> breakTargets;

    ///
    std::vector<JumpTarget> continueTargets;

    /// cleanupScopes[i] contains the information to go from
    /// currentCleanupScope() == i + 1 to currentCleanupScope() == i.
    std::vector<CleanupScope> cleanupScopes;

    ///
    std::vector<CatchScope> catchScopes;

    /// Gotos which we were not able to resolve to any cleanup scope, but which
    /// might still be defined later in the function at top level. If there are
    /// any left on function exit, it is an error (e.g. because the user tried
    /// to goto into a finally block, etc.).
    std::vector<GotoJump> topLevelUnresolvedGotos;

    /// Caches landing pads generated for catches without any cleanups to run
    /// (null if not yet emitted, one element is pushed to/popped from the back
    /// on entering/leaving a catch block).
    std::vector<llvm::BasicBlock*> topLevelLandingPads;
};

template <typename T>
llvm::CallSite ScopeStack::callOrInvoke(llvm::Value* callee, const T &args,
    const char* name
) {
    // If this is a direct call, we might be able to use the callee attributes
    // to our advantage.
    llvm::Function* calleeFn = llvm::dyn_cast<llvm::Function>(callee);

    if (!doesCalleeNeedInvoke(calleeFn)) {
        llvm::CallInst* call = irs->ir->CreateCall(callee, args, name);
        if (calleeFn) {
            call->setAttributes(calleeFn->getAttributes());
        }
        return call;
    }

    if (currentLandingPads().empty()) {
        // Have not encountered any catches (for which we would push a scope) or
        // calls to throwing functions (where we would have already executed
        // this if) in this cleanup scope yet.
        currentLandingPads().push_back(0);
    }

    llvm::BasicBlock*& landingPad = currentLandingPads().back();
    if (!landingPad) {
        landingPad = emitLandingPad();
    }

    llvm::BasicBlock* postinvoke = llvm::BasicBlock::Create(irs->context(),
        "postinvoke", irs->topfunc(), landingPad);
    llvm::InvokeInst* invoke = irs->ir->CreateInvoke(callee, postinvoke,
        landingPad, args, name);

    if (calleeFn) {
        invoke->setAttributes(calleeFn->getAttributes());
    }

    irs->scope() = IRScope(postinvoke);
    return invoke;
}

// represents a function
struct IrFunction {
    // constructor
    IrFunction(FuncDeclaration* fd);

    // annotations
    void setNeverInline();
    void setAlwaysInline();

    llvm::AllocaInst* getOrCreateEhPtrSlot();

    llvm::Function* func;
    llvm::Instruction* allocapoint;
    FuncDeclaration* decl;
    TypeFunction* type;

    /// Points to the associated scope stack while emitting code for the function.
    ScopeStack* scopes;

    llvm::Value* retArg; // return in ptr arg
    llvm::Value* thisArg; // class/struct 'this' arg
    llvm::Value* nestArg; // nested function 'this' arg

    llvm::Value* nestedVar; // alloca for the nested context of this function
    llvm::StructType* frameType; // type of nested context
    // number of enclosing functions with variables accessed by nested functions
    // (-1 if neither this function nor any enclosing ones access variables from enclosing functions)
    int depth;
    bool nestedContextCreated; // holds whether nested context is created

    llvm::Value* _arguments;
    llvm::Value* _argptr;

    /// A stack slot containing the return value, for functions that return by value.
    llvm::AllocaInst* retValSlot;
    /// The basic block with the return instruction.
    llvm::BasicBlock* retBlock;

    /// A stack slot containing the exception object pointer while a landing pad
    /// is active. Need this because the instruction must dominate all uses as a
    /// _d_eh_resume_unwind parameter, but if we take a select at the end on a
    /// cleanup on the way there, it also must dominate all other predecessors
    /// of the cleanup. Thus, we just create an alloca at the start of the
    /// function.
    llvm::AllocaInst* ehPtrSlot;
    /// The basic block with the return instruction. Because of
    /// ehPtrSlot, we do not need more than one, so might as well
    /// cache it.
    llvm::BasicBlock* resumeUnwindBlock;

    /// Similar story to ehPtrSlot, but for the selector value.
    llvm::AllocaInst* ehSelectorSlot;

#if LDC_LLVM_VER >= 307
    llvm::DISubprogram* diSubprogram = nullptr;
    std::stack<llvm::DILexicalBlock*> diLexicalBlocks;
    typedef llvm::DenseMap<VarDeclaration*, llvm::DILocalVariable*> VariableMap;
#else
    llvm::DISubprogram diSubprogram;
    std::stack<llvm::DILexicalBlock> diLexicalBlocks;
    typedef llvm::DenseMap<VarDeclaration*, llvm::DIVariable> VariableMap;
#endif
    // Debug info for all variables
    VariableMap variableMap;

    IrFuncTy irFty;
};

IrFunction* getIrFunc(FuncDeclaration* decl, bool create = false);
bool isIrFuncCreated(FuncDeclaration* decl);

#endif
