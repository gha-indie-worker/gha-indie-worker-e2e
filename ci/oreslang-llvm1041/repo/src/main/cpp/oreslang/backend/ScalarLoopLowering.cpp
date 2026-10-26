#include "oreslang/backend/LlvmCodegen.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "oreslang/backend/LlvmCompat.hpp"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace oreslang {

std::string emit_scalar_while_frame_llvm_ir(const ScalarWhileLoop& loop) {
    if (loop.initial < 0 || loop.upper_bound < loop.initial ||
            loop.upper_bound > 1000000000LL)
        throw std::runtime_error("invalid experimental scalar loop bounds");

    // ABI v0: caller allocates an exclusively owned, pinned, zero-initialized
    // 24-byte frame. [state, counter, magic] are never stack slots across a
    // return-to-scheduler boundary. A real actor carrier must serially own
    // all calls; this compiler fragment does NOT implement actor dispatch.
    llvm::LLVMContext context;
    llvm::Module module("oreslang-native-scalar-loop", context);
    auto* i64 = llvm::Type::getInt64Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* voidTy = llvm::Type::getVoidTy(context);
    auto* ptr = native_pointer_type(context, i64);
    auto* frameTy = llvm::StructType::create(context, {i64, i64, i64},
                                             "oreslang.rt.scalar.frame");
    constexpr std::uint64_t kMagic = 0x4f5245534c4f4f50ULL;
    const auto constant = [&](std::uint64_t n) { return llvm::ConstantInt::get(i64, n); };
    const auto field = [&](llvm::IRBuilder<>& b, llvm::Value* frame, unsigned index) {
        return b.CreateStructGEP(frameTy, frame, index, "frame.field");
    };
    const auto fail = [&](llvm::IRBuilder<>& b) {
        b.CreateCall(native_trap(module));
        b.CreateUnreachable();
    };
    const auto entryGuard = [&](llvm::Function* fn, llvm::Value* frame, llvm::IRBuilder<>& b) {
        auto* ok = llvm::BasicBlock::Create(context, "frame.nonnull", fn);
        auto* bad = llvm::BasicBlock::Create(context, "frame.null", fn);
        b.CreateCondBr(b.CreateICmpNE(frame, llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptr))), ok, bad);
        b.SetInsertPoint(bad);
        fail(b);
        b.SetInsertPoint(ok);
    };

    auto* init = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptr}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_init", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", init));
        auto* frame = &*init->arg_begin();
        entryGuard(init, frame, b);
        auto* stateAddress = field(b, frame, 0);
        auto* fresh = llvm::BasicBlock::Create(context, "fresh", init);
        auto* invalid = llvm::BasicBlock::Create(context, "already.initialized", init);
        b.CreateCondBr(b.CreateICmpEQ(b.CreateLoad(i64, stateAddress), constant(0)),
                       fresh, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(fresh);
        b.CreateStore(constant(1), stateAddress);
        b.CreateStore(constant(static_cast<std::uint64_t>(loop.initial)), field(b, frame, 1));
        b.CreateStore(constant(kMagic), field(b, frame, 2));
        b.CreateRetVoid();
    }

    // Status: 0 DONE, 1 COOPERATE, 2 CANCELLED. Zero fuel is a harmless
    // handoff without guest side effects. Fuel >512 traps, not an exemption
    // from the carrier reduction cap. No native stack survives a return.
    auto* step = llvm::Function::Create(
        llvm::FunctionType::get(i32, {ptr, i64}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_step", module);
    {
        auto arg = step->arg_begin();
        llvm::Value* frame = &*arg++;
        llvm::Value* fuel = &*arg;
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", step));
        entryGuard(step, frame, b);
        auto* stateAddress = field(b, frame, 0);
        auto* counterAddress = field(b, frame, 1);
        auto* valid = llvm::BasicBlock::Create(context, "frame.valid", step);
        auto* invalid = llvm::BasicBlock::Create(context, "frame.invalid", step);
        llvm::Value* magic = b.CreateLoad(i64, field(b, frame, 2));
        llvm::Value* cap = b.CreateICmpULE(fuel, constant(512));
        b.CreateCondBr(b.CreateAnd(b.CreateICmpEQ(magic, constant(kMagic)), cap),
                       valid, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(valid);

        auto* active = llvm::BasicBlock::Create(context, "active", step);
        auto* done = llvm::BasicBlock::Create(context, "done", step);
        auto* cancelled = llvm::BasicBlock::Create(context, "cancelled", step);
        auto* loopHead = llvm::BasicBlock::Create(context, "loop.check", step);
        auto* budget = llvm::BasicBlock::Create(context, "loop.fuel", step);
        auto* advance = llvm::BasicBlock::Create(context, "loop.commit", step);
        auto* exhausted = llvm::BasicBlock::Create(context, "cooperate", step);
        auto* complete = llvm::BasicBlock::Create(context, "loop.complete", step);
        auto* state = b.CreateLoad(i64, stateAddress, "state");
        auto* sw = b.CreateSwitch(state, invalid, 3);
        sw->addCase(constant(1), active);
        sw->addCase(constant(2), done);
        sw->addCase(constant(3), cancelled);

        b.SetInsertPoint(done);
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        b.SetInsertPoint(cancelled);
        b.CreateRet(llvm::ConstantInt::get(i32, 2));
        b.SetInsertPoint(active);
        b.CreateBr(loopHead);
        b.SetInsertPoint(loopHead);
        auto* remaining = b.CreatePHI(i64, 2, "dispatch.fuel");
        remaining->addIncoming(fuel, active);
        auto* current = b.CreateLoad(i64, counterAddress, "counter.before");
        b.CreateCondBr(b.CreateICmpSLT(current,
            llvm::ConstantInt::getSigned(i64, loop.upper_bound)), budget, complete);

        b.SetInsertPoint(complete);
        b.CreateStore(constant(2), stateAddress);
        b.CreateRet(llvm::ConstantInt::get(i32, 0));
        b.SetInsertPoint(budget);
        b.CreateCondBr(b.CreateICmpEQ(remaining, constant(0)), exhausted, advance);
        b.SetInsertPoint(exhausted);
        b.CreateRet(llvm::ConstantInt::get(i32, 1));
        b.SetInsertPoint(advance);
        // Source body has exactly one nonthrowing, bounded +1 effect.
        // Commit the i64 counter BEFORE any subsequent cooperation request.
        auto* next = b.CreateNSWAdd(current, constant(1), "counter.committed");
        b.CreateStore(next, counterAddress);
        auto* nextFuel = b.CreateSub(remaining, constant(1), "fuel.remaining");
        b.CreateBr(loopHead);
        remaining->addIncoming(nextFuel, advance);
    }

    auto* cancel = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptr}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_cancel", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", cancel));
        auto* frame = &*cancel->arg_begin();
        entryGuard(cancel, frame, b);
        auto* stateAddress = field(b, frame, 0);
        auto* s = b.CreateLoad(i64, stateAddress);
        auto* valid = llvm::BasicBlock::Create(context, "valid", cancel);
        auto* invalid = llvm::BasicBlock::Create(context, "invalid", cancel);
        auto* known = b.CreateAnd(b.CreateICmpUGE(s, constant(1)),
                                   b.CreateICmpULE(s, constant(3)));
        b.CreateCondBr(b.CreateAnd(known,
            b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 2)), constant(kMagic))),
            valid, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(valid);
        auto* active = llvm::BasicBlock::Create(context, "cancel.active", cancel);
        auto* final = llvm::BasicBlock::Create(context, "cancel.final", cancel);
        b.CreateCondBr(b.CreateICmpEQ(s, constant(1)), active, final);
        b.SetInsertPoint(active);
        b.CreateStore(constant(3), stateAddress);
        b.CreateBr(final);
        b.SetInsertPoint(final);
        b.CreateRetVoid();
    }

    auto* result = llvm::Function::Create(
        llvm::FunctionType::get(i64, {ptr}, false),
        llvm::Function::ExternalLinkage, "oreslang_rt_scalar_frame_result", module);
    {
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", result));
        auto* frame = &*result->arg_begin();
        entryGuard(result, frame, b);
        auto* valid = llvm::BasicBlock::Create(context, "result.ready", result);
        auto* invalid = llvm::BasicBlock::Create(context, "result.invalid", result);
        b.CreateCondBr(b.CreateAnd(
            b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 0)), constant(2)),
            b.CreateICmpEQ(b.CreateLoad(i64, field(b, frame, 2)), constant(kMagic))),
            valid, invalid);
        b.SetInsertPoint(invalid);
        fail(b);
        b.SetInsertPoint(valid);
        b.CreateRet(b.CreateLoad(i64, field(b, frame, 1)));
    }

    std::string errors;
    llvm::raw_string_ostream diagnostic(errors);
    if (llvm::verifyModule(module, &diagnostic))
        throw std::runtime_error("invalid scalar-loop LLVM IR: " + diagnostic.str());
    std::string output;
    llvm::raw_string_ostream o(output);
    module.print(o, nullptr);
    o.flush();
    return output;
}

} // namespace oreslang
