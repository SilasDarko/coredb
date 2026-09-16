#include "coredb/jit/query_jit.h"

#include <mutex>
#include <stdexcept>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>

namespace coredb::jit {

namespace {

void EnsureNativeTargetInitialized() {
  static std::once_flag once;
  std::call_once(once, [] {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
  });
}

constexpr char kFunctionName[] = "coredb_filter_gt_sum";

// Builds the IR for:
//   void coredb_filter_gt_sum(const int64_t* pred, const int64_t* out,
//                              uint64_t n, int64_t threshold,
//                              uint64_t* out_count, int64_t* out_sum) {
//     uint64_t i = 0, count = 0; int64_t sum = 0;
//     while (i < n) {
//       bool m = pred[i] > threshold;         // branchless predication —
//       sum += m ? out[i] : 0;                // same trick the SIMD
//       count += m ? 1 : 0;                   // kernels use, so the loop
//       i += 1;                               // body is a single block
//     }                                       // the vectorizer can work with
//     *out_count = count; *out_sum = sum;
//   }
std::unique_ptr<llvm::Module> BuildModule(llvm::LLVMContext& context) {
  auto module = std::make_unique<llvm::Module>("coredb_jit_module", context);
  llvm::IRBuilder<> builder(context);

  llvm::Type* i64 = builder.getInt64Ty();
  llvm::PointerType* ptr_ty = llvm::PointerType::get(context, 0);

  llvm::FunctionType* fn_type =
      llvm::FunctionType::get(builder.getVoidTy(), {ptr_ty, ptr_ty, i64, i64, ptr_ty, ptr_ty}, false);
  llvm::Function* fn =
      llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage, kFunctionName, module.get());

  auto arg_it = fn->arg_begin();
  llvm::Value* pred_ptr = &*arg_it++;
  llvm::Value* out_ptr = &*arg_it++;
  llvm::Value* n = &*arg_it++;
  llvm::Value* threshold = &*arg_it++;
  llvm::Value* out_count_ptr = &*arg_it++;
  llvm::Value* out_sum_ptr = &*arg_it++;
  pred_ptr->setName("pred");
  out_ptr->setName("out");
  n->setName("n");
  threshold->setName("threshold");
  out_count_ptr->setName("out_count");
  out_sum_ptr->setName("out_sum");

  llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", fn);
  llvm::BasicBlock* loop_header = llvm::BasicBlock::Create(context, "loop.header", fn);
  llvm::BasicBlock* loop_body = llvm::BasicBlock::Create(context, "loop.body", fn);
  llvm::BasicBlock* exit = llvm::BasicBlock::Create(context, "exit", fn);

  builder.SetInsertPoint(entry);
  llvm::Value* i_ptr = builder.CreateAlloca(i64, nullptr, "i.addr");
  llvm::Value* sum_ptr = builder.CreateAlloca(i64, nullptr, "sum.addr");
  llvm::Value* count_ptr = builder.CreateAlloca(i64, nullptr, "count.addr");
  builder.CreateStore(builder.getInt64(0), i_ptr);
  builder.CreateStore(builder.getInt64(0), sum_ptr);
  builder.CreateStore(builder.getInt64(0), count_ptr);
  builder.CreateBr(loop_header);

  builder.SetInsertPoint(loop_header);
  llvm::Value* i_val = builder.CreateLoad(i64, i_ptr, "i.val");
  llvm::Value* loop_cond = builder.CreateICmpULT(i_val, n, "loop.cond");
  builder.CreateCondBr(loop_cond, loop_body, exit);

  // Single-block body, no inner branch: `m` selects 0 instead of skipping
  // work, exactly like the NEON/AVX2/AVX-512 kernels' mask-and-add.
  builder.SetInsertPoint(loop_body);
  llvm::Value* pred_elem_ptr = builder.CreateGEP(i64, pred_ptr, i_val, "pred.elem.ptr");
  llvm::Value* pred_val = builder.CreateLoad(i64, pred_elem_ptr, "pred.val");
  llvm::Value* matched = builder.CreateICmpSGT(pred_val, threshold, "matched");

  llvm::Value* out_elem_ptr = builder.CreateGEP(i64, out_ptr, i_val, "out.elem.ptr");
  llvm::Value* out_val = builder.CreateLoad(i64, out_elem_ptr, "out.val");
  llvm::Value* masked_val = builder.CreateSelect(matched, out_val, builder.getInt64(0), "masked.val");
  llvm::Value* new_sum = builder.CreateAdd(builder.CreateLoad(i64, sum_ptr, "sum.cur"), masked_val, "sum.new");
  builder.CreateStore(new_sum, sum_ptr);

  llvm::Value* masked_one = builder.CreateSelect(matched, builder.getInt64(1), builder.getInt64(0), "masked.one");
  llvm::Value* new_count = builder.CreateAdd(builder.CreateLoad(i64, count_ptr, "count.cur"), masked_one, "count.new");
  builder.CreateStore(new_count, count_ptr);

  llvm::Value* i_next = builder.CreateAdd(i_val, builder.getInt64(1), "i.next");
  builder.CreateStore(i_next, i_ptr);
  builder.CreateBr(loop_header);

  builder.SetInsertPoint(exit);
  builder.CreateStore(builder.CreateLoad(i64, count_ptr, "count.final"), out_count_ptr);
  builder.CreateStore(builder.CreateLoad(i64, sum_ptr, "sum.final"), out_sum_ptr);
  builder.CreateRetVoid();

  std::string verify_error;
  llvm::raw_string_ostream verify_stream(verify_error);
  if (llvm::verifyFunction(*fn, &verify_stream)) {
    throw std::runtime_error("QueryJit: generated IR failed verification: " + verify_stream.str());
  }
  return module;
}

// Runs the same -O3 pipeline `clang -O3` would (mem2reg to get the loop
// counter/accumulators into registers, instcombine, loop rotation, the
// SLP/loop vectorizer, ...) on the hand-built IR above before codegen. A
// real query compiler doesn't hand the raw, naively-lowered IR straight to
// the backend — skipping this step is why an earlier version of this file
// JIT-compiled a loop slower than the hand-written SIMD kernels despite
// having no per-row interpreter overhead at all (see BENCHMARKS.md).
void OptimizeModule(llvm::Module& module) {
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;

  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  mpm.run(module, mam);
}

}  // namespace

QueryJit::QueryJit() { Compile(); }
QueryJit::~QueryJit() = default;

void QueryJit::Compile() {
  EnsureNativeTargetInitialized();

  auto context = std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> module = BuildModule(*context);
  OptimizeModule(*module);

  auto jit_or_err =
      llvm::orc::LLJITBuilder()
          .setJITTargetMachineBuilder(llvm::orc::JITTargetMachineBuilder::detectHost()->setCodeGenOptLevel(
              llvm::CodeGenOpt::Aggressive))
          .create();
  if (!jit_or_err) {
    throw std::runtime_error("QueryJit: failed to create LLJIT: " +
                              llvm::toString(jit_or_err.takeError()));
  }
  jit_ = std::move(*jit_or_err);

  llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(context));
  if (auto err = jit_->addIRModule(std::move(tsm))) {
    throw std::runtime_error("QueryJit: addIRModule failed: " + llvm::toString(std::move(err)));
  }

  auto sym = jit_->lookup(kFunctionName);
  if (!sym) {
    throw std::runtime_error("QueryJit: symbol lookup failed: " + llvm::toString(sym.takeError()));
  }
  compiled_fn_ = sym->toPtr<FusedFn>();
}

simd::FilterSumResult QueryJit::Run(const int64_t* pred, const int64_t* out, size_t n, int64_t threshold) const {
  simd::FilterSumResult result;
  compiled_fn_(pred, out, static_cast<uint64_t>(n), threshold, &result.matched_count, &result.sum);
  return result;
}

}  // namespace coredb::jit
