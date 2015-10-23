/*
 * Copyright (C) 2015 David Devecsery
 */

#include "include/lib/DynPtsto.h"

#include <cstdio>

#include <fstream>
#include <list>
#include <map>
#include <string>
#include <sstream>
#include <vector>

#include "llvm/BasicBlock.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/GlobalVariable.h"
#include "llvm/InstrTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/MathExtras.h"

#include "include/SpecSFS.h"
#include "include/LLVMHelper.h"
#include "include/AllocInfo.h"
#include "include/lib/UnusedFunctions.h"

static llvm::cl::opt<std::string>
  DynPtstoFilename("dyn-ptsto-file", llvm::cl::init("dyn_ptsto.log"),
      llvm::cl::value_desc("filename"),
      llvm::cl::desc("Ptsto file saved/loaded by DynPtsto analysis"));

// First and last functions called
static const std::string InitInstName = "__DynPtsto_do_init";
static const std::string FinishInstName = "__DynPtsto_do_finish";

// Called to initialize the arguments to main
static const std::string MainInit2Name = "__DynPtsto_main_init2";
static const std::string MainInit3Name = "__DynPtsto_main_init3";

// Called on alloc/free
static const std::string AllocaInstName = "__DynPtsto_do_alloca";
static const std::string CallInstName = "__DynPtsto_do_call";
static const std::string RetInstName = "__DynPtsto_do_ret";
static const std::string MallocInstName = "__DynPtsto_do_malloc";
static const std::string FreeInstName = "__DynPtsto_do_free";
// Called on ptr returnin fcn
static const std::string VisitInstName = "__DynPtsto_do_visit";

// Instrument dyn ptsto info {{{
class InstrDynPtsto : public SpecSFS {
 public:
    static char ID;

    InstrDynPtsto() : SpecSFS(ID) { }

    bool runOnModule(llvm::Module &m) override;

    void getAnalysisUsage(llvm::AnalysisUsage &au) const override;

 private:
    void setupSpecSFSids(llvm::Module &);
    void addExternalFunctions(llvm::Module &);
    void addInitializationCalls(llvm::Module &);
    llvm::Instruction *addMallocCall(llvm::Module &m, ObjectMap::ObjID obj_id,
        llvm::Value *val, llvm::Value *size_val,
        llvm::Instruction *insert_before);
};

void InstrDynPtsto::getAnalysisUsage(llvm::AnalysisUsage &au) const {
  au.addRequired<UnusedFunctions>();
  au.setPreservesAll();
}

void InstrDynPtsto::setupSpecSFSids(llvm::Module &M) {
  // Set up our alias analysis
  // -- This is required for the llvm AliasAnalysis interface
  // InitializeAliasAnalysis(this);

  // Clear the def-use graph
  // It should already be cleared, but I'm paranoid
  ConstraintGraph cg;
  CFG cfg;
  ObjectMap &omap = omap_;

  if (identifyObjects(omap, M)) {
    abort();
  }

  const UnusedFunctions &unused_fcns =
      getAnalysis<UnusedFunctions>();

  if (createConstraints(cg, cfg, omap, M, unused_fcns)) {
    abort();
  }

  if (optimizeConstraints(cg, cfg, omap)) {
    abort();
  }

  // Also add indirect info... this means we have to wait for Andersen's
  Andersens aux;
  // Get AUX info, in this instance we choose Andersens
  if (aux.runOnModule(M)) {
    // Andersens had better not change M!
    abort();
  }

  // Now, fill in the indirect function calls
  if (addIndirectCalls(cg, cfg, aux, nullptr, omap)) {
    abort();
  }
}

bool InstrDynPtsto::runOnModule(llvm::Module &m) {
  auto i32_type = llvm::IntegerType::get(m.getContext(), 32);
  auto i64_type = llvm::IntegerType::get(m.getContext(), 64);
  auto i8_ptr_type = llvm::PointerType::get(
      llvm::IntegerType::get(m.getContext(), 8), 0);

  // Okay, we identify all allocations:
  //   Static allocations (alloca instrs)
  //   Dynamic allcoations (use Andersens.h for this?)
  // We also need to identify frees:
  //   Return calls
  //   free calls (add to Andersens.h?)

  // Setup ObjectMap ids using the SpecSFS identifiers...
  setupSpecSFSids(m);
  ObjectMap &omap = omap_;

  // Notify module of external functions
  addExternalFunctions(m);

  // Iterate each instruction, keeping lists
  for (auto &fcn : m) {
    // Ignore functions without bodies
    if (fcn.isDeclaration() || fcn.isIntrinsic()) {
      continue;
    }


    int32_t fcn_num_allocas = 0;
    std::vector<llvm::Instruction *> ret_list;

    for (auto &bb : fcn) {
      // Create list to hold all malloc/free Value*s that needs instrumenting
      // within this function
      std::vector<llvm::Instruction *> alloca_list;
      std::vector<llvm::Instruction *> malloc_list;
      std::vector<llvm::Instruction *> free_list;

      // Also create list for all pointer values
      std::vector<llvm::Instruction *> pointer_list;
      std::list<llvm::Instruction *> phi_list;

      for (auto &inst : bb) {
        // Add stack allocation
        if (llvm::isa<llvm::AllocaInst>(&inst)) {
          if (llvm::isa<llvm::PointerType>(inst.getType())) {
            fcn_num_allocas++;
            alloca_list.push_back(&inst);
          }
        // Possible alloc/dealloc
        } else if (auto ci = dyn_cast<llvm::CallInst>(&inst)) {
          auto fcn = LLVMHelper::getFcnFromCall(ci);

          // FIXME: Use andersens results on nullptr???
          if (fcn != nullptr) {
            if (AllocInfo::fcnIsMalloc(fcn)) {
              malloc_list.push_back(&inst);
            }
            if (AllocInfo::fcnIsFree(fcn)) {
              free_list.push_back(&inst);
            }
          }
        // Add stack deallocation
        } else if (llvm::isa<llvm::ReturnInst>(&inst)) {
          ret_list.push_back(&inst);
        }

        // Grab ptsto from return
        if (llvm::isa<llvm::PointerType>(inst.getType())) {
          // Deal w/ phi nodes... meh
          if (llvm::isa<llvm::PHINode>(inst)) {
            phi_list.push_front(&inst);
          } else {
            pointer_list.push_back(&inst);
          }
        }
      }

      // Determine the first non-alloc of the function
      /*
      llvm::Instruction *alloca_insert_start = nullptr;
      for (auto &insn : fcn.getEntryBlock()) {
        if (!llvm::isa<llvm::AllocaInst>(insn)) {
          alloca_insert_start = &insn;
          break;
        }
      }
      */

      // Add instrumentation calls:
      // First, deal w/ the phi nodes (meh)
      // Find the first non-phi of the bb:
      // Get the external function
      auto visit_fcn = m.getFunction(VisitInstName);

      auto inst_it = std::begin(bb);
      llvm::Instruction *inst = &(*inst_it);

      while (llvm::isa<llvm::PHINode>(inst_it) && inst_it != std::end(bb)) {
        ++inst_it;
        inst = &(*inst_it);
      }

      {
        // we now have first inst which isn't a phi
        // We add all of our phi inst calls here:
        for (auto &phi_inst : phi_list) {
          auto val_id = omap.getValue(phi_inst);
          auto i8_ptr_val = new llvm::BitCastInst(phi_inst, i8_ptr_type);
          i8_ptr_val->insertBefore(inst);

          std::vector<llvm::Value *> args;
          args.push_back(llvm::ConstantInt::get(i32_type, val_id.val()));
          args.push_back(i8_ptr_val);
          auto visit_insn = llvm::CallInst::Create(visit_fcn, args);

          visit_insn->insertAfter(i8_ptr_val);
        }
      }

      // for Pointer returning instructions
      for (auto val : pointer_list) {
        // The return value is the val
        auto val_id = omap.getValue(val);

        // Make the call
        auto i8_ptr_val = new llvm::BitCastInst(val, i8_ptr_type);
        i8_ptr_val->insertAfter(val);

        std::vector<llvm::Value *> args;
        args.push_back(llvm::ConstantInt::get(i32_type, val_id.val()));
        args.push_back(i8_ptr_val);
        auto visit_insn = llvm::CallInst::Create(visit_fcn, args);

        visit_insn->insertAfter(i8_ptr_val);
      }

      // for allocas
      // First get the external function
      auto alloc_fcn = m.getFunction(AllocaInstName);
      // Then, call for each alloc
      for (auto val : alloca_list) {
        auto ai = cast<llvm::AllocaInst>(val);
        // The id is the objid from the omap
        auto obj_id = omap.getObject(val);

        // Insert for static alloca's in the functions entry BB before the first
        //    non-alloca inst
        // llvm::Instruction *insert_pos = alloca_insert_start;
        // If this isn't a static alloca, do instrumentation before it
        // if (!ai->isStaticAlloca()) {
        llvm::Instruction *insert_pos = ai;
        // }
        // The address is returned from the alloc
        // The size is gotten from the alloc...
        //   sizeof(type) * arraysize
        // First, type, then type size
        auto type_size_ce = LLVMHelper::calcTypeOffset(m,
            ai->getAllocatedType(), insert_pos);

        // Then, get arraysize from the instruction
        auto array_size_val = ai->getArraySize();

        // Convert to int64_t...
        array_size_val = new llvm::SExtInst(array_size_val, i64_type, "",
            insert_pos);

        // Add the mult inst?...
        auto total_size_val =
          llvm::BinaryOperator::Create(llvm::Instruction::Mul, type_size_ce,
            array_size_val, "", insert_pos);

        auto i8_ptr_val = new llvm::BitCastInst(val, i8_ptr_type);
        i8_ptr_val->insertAfter(ai);
        // pass the final result to the function
        // Make the call
        std::vector<llvm::Value *> args;
        args.push_back(llvm::ConstantInt::get(i32_type, obj_id.val()));
        args.push_back(total_size_val);
        args.push_back(i8_ptr_val);
        auto call_insn = llvm::CallInst::Create(alloc_fcn, args);
        call_insn->insertAfter(i8_ptr_val);
      }

      // NOTE: Must do frees before mallocs for realloc functions
      // (which are both frees and allocs), we need to do free
      // then alloc for frees
      auto free_fcn = m.getFunction(FreeInstName);
      // Get the external funciton
      // For each free:
      for (auto val : free_list) {
        auto ci = cast<llvm::CallInst>(val);

        // Get the function
        auto fcn = LLVMHelper::getFcnFromCall(ci);
        // Get the arg_pos for the object being freed
        auto free_arg = AllocInfo::getFreeArg(m, ci, fcn);
        // Call the instrumentation, before calling free
        std::vector<llvm::Value *> args;
        args.push_back(free_arg);
        llvm::CallInst::Create(free_fcn,
            args, "", val);
      }

      // for mallocs
      // Get the external function
      auto malloc_fcn = m.getFunction(MallocInstName);
      // For each malloc:
      for (auto val : malloc_list) {
        auto ci = cast<llvm::CallInst>(val);
        // Get the obj from the return value
        auto obj_id = omap.getObject(val);

        // Get the function
        auto fcn = LLVMHelper::getFcnFromCall(ci);
        // Get the arg_pos for the size from the function
        auto size_val = AllocInfo::getMallocSizeArg(m, ci, fcn);

        llvm::Instruction *i8_ptr_val = ci;
        if (ci->getType() != i8_ptr_type) {
          i8_ptr_val = new llvm::BitCastInst(ci, i8_ptr_type);
          i8_ptr_val->insertAfter(ci);
        }

        // Make the call
        std::vector<llvm::Value *> args;
        args.push_back(llvm::ConstantInt::get(i32_type, obj_id.val()));
        args.push_back(size_val);
        args.push_back(i8_ptr_val);
        auto malloc_inst_call = llvm::CallInst::Create(
            malloc_fcn, args);

        malloc_inst_call->insertAfter(i8_ptr_val);
      }
    }

    // If we have one or more allocs, we need a call and ret pair
    if (fcn_num_allocas > 0) {
      // First, for calls
      // Get external function
      auto call_fcn = m.getFunction(CallInstName);
      std::vector<llvm::Value *> args;
      // NOTE: This should be the first function
      //    (before the alloc instr calls)
      auto &first_insn = *std::begin(fcn.getEntryBlock());
      llvm::CallInst::Create(call_fcn,
          args, "", &first_insn);
      // for rets
      // First, get the external function
      auto ret_fcn = m.getFunction(RetInstName);
      // Then, call for each ret
      for (auto val : ret_list) {
        // No args, just pops the last alloc frame...
        llvm::CallInst::Create(ret_fcn,
            args, "", val);
      }
    }

    // Also, add visits for the args
    // NOTE: Main is handled specially
    if (fcn.getName() != "main") {
      auto visit_fcn = m.getFunction(VisitInstName);
      std::for_each(fcn.arg_begin(), fcn.arg_end(),
          [&i8_ptr_type, &i32_type, &omap, &fcn, &visit_fcn]
          (llvm::Argument &arg) {
        if (llvm::isa<llvm::PointerType>(arg.getType())) {
          // The return value is the val
          auto val_id = omap.getValue(&arg);
          auto &ins_pos = *llvm::inst_begin(fcn);

          // Make the call
          auto i8_ptr_val = new llvm::BitCastInst(&arg, i8_ptr_type);
          i8_ptr_val->insertBefore(&ins_pos);

          std::vector<llvm::Value *> args;
          args.push_back(llvm::ConstantInt::get(i32_type, val_id.val()));
          args.push_back(i8_ptr_val);
          auto visit_insn = llvm::CallInst::Create(visit_fcn, args);

          visit_insn->insertAfter(i8_ptr_val);
        }
      });
    }
  }

  // Add global initializers for function pointers
  // AND deal with argc & argv
  {
    auto malloc_fcn = m.getFunction(MallocInstName);

    auto main_fcn = m.getFunction("main");
    auto &first_inst = *std::begin(main_fcn->getEntryBlock());
    for (auto &fcn : m) {
      // Ignore intrinsics
      if (fcn.isIntrinsic()) {
        continue;
      }
      // Get the obj from the function value
      auto obj_id = omap.getObject(&fcn);

      // Get an i8_ptr from the function
      auto i8_ptr_val = new llvm::BitCastInst(&fcn, i8_ptr_type, "",
        &first_inst);

      // calc the size of a pointer
      auto size_val = LLVMHelper::calcTypeOffset(m,
          i8_ptr_type, &first_inst);

      // Make the call
      std::vector<llvm::Value *> args;
      args.push_back(llvm::ConstantInt::get(i32_type, obj_id.val()));
      args.push_back(size_val);
      args.push_back(i8_ptr_val);
      llvm::CallInst::Create(malloc_fcn, args, "", &first_inst);
    }
  }

  // Now, add global initializers to the beignning of main
  {
    // Get the external function
    auto malloc_fcn = m.getFunction(MallocInstName);

    auto main_fcn = m.getFunction("main");
    auto &first_inst = *std::begin(main_fcn->getEntryBlock());

    // For each global init:
    std::for_each(m.global_begin(), m.global_end(),
        [&omap, &malloc_fcn, &m, &first_inst,
          &i32_type, &i8_ptr_type]
        (llvm::Value &glbl) {
      // Get the obj from the return value
      auto obj_id = omap.getObject(&glbl);

      // Get the arg_pos for the size from the function
      // llvm::dbgs() << "glbl type is: " << *glbl.getType() << "\n";
      auto type = glbl.getType();
      assert(llvm::isa<llvm::PointerType>(type));
      // Strip pointer type:
      type = type->getContainedType(0);
      // llvm::dbgs() << "passed type is: " << *type << "\n";
      auto size_val = LLVMHelper::calcTypeOffset(m,
          type, &first_inst);

      auto i8_ptr_val = new llvm::BitCastInst(&glbl, i8_ptr_type, "",
        &first_inst);

      // Make the call
      std::vector<llvm::Value *> args;
      args.push_back(llvm::ConstantInt::get(i32_type, obj_id.val()));
      args.push_back(size_val);
      args.push_back(i8_ptr_val);
      llvm::CallInst::Create(malloc_fcn, args, "", &first_inst);
    });
  }

  // Add initialization calls:
  addInitializationCalls(m);

  // We modify all the stuff
  return true;
}

// Adding calls {{{
void InstrDynPtsto::addExternalFunctions(llvm::Module &m) {
  auto void_type = llvm::Type::getVoidTy(m.getContext());
  auto i8_ptr_type = llvm::PointerType::get(
      llvm::IntegerType::get(m.getContext(), 8), 0);
  auto i32_type = llvm::IntegerType::get(m.getContext(), 32);
  auto i64_type = llvm::IntegerType::get(m.getContext(), 64);
  // Create external linkages for:
  // AllocaInst(i32 obj_id, i64 size, i8 *ret)
  {
    // Create the args
    std::vector<llvm::Type *> alloc_fcn_type_args;
    alloc_fcn_type_args.push_back(i32_type);
    alloc_fcn_type_args.push_back(i64_type);
    alloc_fcn_type_args.push_back(i8_ptr_type);
    // Create the function type
    auto alloc_fcn_type = llvm::FunctionType::get(
        void_type,
        alloc_fcn_type_args,
        false);
    // Create the function
    llvm::Function::Create(alloc_fcn_type,
        llvm::GlobalValue::ExternalLinkage,
        AllocaInstName, &m);
  }
  // CallInst(void)
  {
    // Create the args
    std::vector<llvm::Type *> call_fcn_type_args;
    // Create the function type
    auto call_fcn_type = llvm::FunctionType::get(
        void_type,
        call_fcn_type_args,
        false);
    // Create the function
    llvm::Function::Create(call_fcn_type,
        llvm::GlobalValue::ExternalLinkage,
        CallInstName, &m);
  }
  // RetInst(void)
  {
    // Create the args
    std::vector<llvm::Type *> ret_fcn_type_args;
    // Create the function type
    auto ret_fcn_type = llvm::FunctionType::get(
        void_type,
        ret_fcn_type_args,
        false);
    // Create the function
    llvm::Function::Create(ret_fcn_type,
        llvm::GlobalValue::ExternalLinkage,
        RetInstName, &m);
  }
  // MallocInst(i32 obj_id, i64 size, i8 *ret)
  {
    // Create the args
    std::vector<llvm::Type *> malloc_fcn_type_args;
    malloc_fcn_type_args.push_back(i32_type);
    malloc_fcn_type_args.push_back(i64_type);
    malloc_fcn_type_args.push_back(i8_ptr_type);
    // Create the function type
    auto malloc_fcn_type = llvm::FunctionType::get(
        void_type,
        malloc_fcn_type_args,
        false);
    // Create the function
    llvm::Function::Create(malloc_fcn_type,
        llvm::GlobalValue::ExternalLinkage,
        MallocInstName, &m);
  }
  // FreeInst(i8 *ptr)
  {
    // Create the args
    std::vector<llvm::Type *> free_fcn_type_args;
    free_fcn_type_args.push_back(i8_ptr_type);
    // Create the function type
    auto free_fcn_type = llvm::FunctionType::get(
        void_type,
        free_fcn_type_args,
        false);
    // Create the function
    llvm::Function::Create(free_fcn_type,
        llvm::GlobalValue::ExternalLinkage,
        FreeInstName, &m);
  }
  // VisitInst(i32 val_id, i8 *ptr)
  {
    // Create the args
    std::vector<llvm::Type *> visit_fcn_type_args;
    visit_fcn_type_args.push_back(i32_type);
    visit_fcn_type_args.push_back(i8_ptr_type);
    // Create the function type
    auto visit_fcn_type = llvm::FunctionType::get(
        void_type,
        visit_fcn_type_args,
        false);
    // Create the function
    llvm::Function::Create(visit_fcn_type,
        llvm::GlobalValue::ExternalLinkage,
        VisitInstName, &m);
  }
  // InitMainArgs2(i32 argc, char **argv)
  {
    // Create the args
    std::vector<llvm::Type *> main2_type_args;
    main2_type_args.push_back(i32_type);
    main2_type_args.push_back(i32_type);
    main2_type_args.push_back(i8_ptr_type->getPointerTo());
    // Create the function type
    auto main2_fcn_type = llvm::FunctionType::get(
        void_type,
        main2_type_args,
        false);
    // Create the function
    llvm::Function::Create(main2_fcn_type,
        llvm::GlobalValue::ExternalLinkage,
        MainInit2Name, &m);
  }
  // InitMainArgs3(i32 argc, char **argv, char **envp)
  {
    // Create the args
    std::vector<llvm::Type *> main3_type_args;
    main3_type_args.push_back(i32_type);
    main3_type_args.push_back(i32_type);
    main3_type_args.push_back(i8_ptr_type->getPointerTo());
    main3_type_args.push_back(i8_ptr_type->getPointerTo());
    // Create the function type
    auto main3_fcn_type = llvm::FunctionType::get(
        void_type,
        main3_type_args,
        false);
    // Create the function
    llvm::Function::Create(main3_fcn_type,
        llvm::GlobalValue::ExternalLinkage,
        MainInit3Name, &m);
  }
}

void InstrDynPtsto::addInitializationCalls(llvm::Module &m) {
  // Var types
  auto void_type = llvm::Type::getVoidTy(m.getContext());
  auto i64_type = llvm::IntegerType::get(m.getContext(), 64);

  // Function types
  std::vector<llvm::Type *> type_no_args;
  auto void_fcn_type = llvm::FunctionType::get(
      void_type,
      type_no_args,
      false);

  auto ptr_void_fcn_type = void_fcn_type->getPointerTo();

  // Create the init/finish functions
  auto init_fcn = llvm::Function::Create(void_fcn_type,
      llvm::GlobalValue::ExternalLinkage,
      InitInstName, &m);
  auto finish_fcn = llvm::Function::Create(void_fcn_type,
      llvm::GlobalValue::ExternalLinkage,
      FinishInstName, &m);

  auto main_fcn = m.getFunction("main");

  // get the first instruction
  auto first_inst = &(*inst_begin(main_fcn));

  // While we're at it, we're going to add the args to main to our set of
  //   objs...
  {
    auto i8_ptr_type = llvm::PointerType::get(
        llvm::IntegerType::get(m.getContext(), 8), 0);
    auto ce_null = llvm::ConstantPointerNull::get(i8_ptr_type);

    // Do one for IntValue
    first_inst = addMallocCall(m, ObjectMap::NullValue, ce_null,
        llvm::ConstantInt::get(i64_type, 4096*8), first_inst);

    // Deal w/ argc, argv, and envp here
    // Detect number of main args
    std::vector<llvm::Value *> main_args;
    // Set first arg to call to be objid for argvval
    auto i32_type = llvm::IntegerType::get(m.getContext(), 32);
    main_args.push_back(llvm::ConstantInt::get(i32_type,
          ObjectMap::ArgvValue.val()));

    std::for_each(main_fcn->arg_begin(), main_fcn->arg_end(),
        [&main_args] (llvm::Argument &arg) {
      main_args.push_back(&arg);
    });

    // Note all main_args size comps are +1 due to the obj_id arg
    if (main_args.size() != 1) {
      llvm::Function *main_init_fcn;

      if (main_args.size() == 2) {
        llvm_unreachable("Main has 1 arg?");
      } else if (main_args.size() == 3) {
        main_init_fcn = m.getFunction(MainInit2Name);
      } else if (main_args.size() == 4) {
        main_init_fcn = m.getFunction(MainInit3Name);
      } else {
        llvm_unreachable("Main has more than 3 args?");
      }

      llvm::CallInst::Create(main_init_fcn, main_args, "", first_inst);
    }
  }

  // get "atexit" function
  // Create the "atexit" type
  std::vector<llvm::Type *> atexit_args;
  atexit_args.push_back(ptr_void_fcn_type);
  auto atexit_type = llvm::FunctionType::get(
      void_type,
      atexit_args,
      false);

  auto at_exit = m.getFunction("atexit");
  if (at_exit == nullptr) {
    at_exit = llvm::Function::Create(
        atexit_type,
        llvm::GlobalValue::ExternalLinkage,
        "atexit", &m);
  }

  // Call our function before the first inst:
  std::vector<llvm::Value *> no_args;
  llvm::CallInst::Create(init_fcn, no_args, "", first_inst);
  // Setup our function to call atexit
  std::vector<llvm::Value *> atexit_call_args;
  atexit_call_args.push_back(finish_fcn);
  llvm::CallInst::Create(at_exit, atexit_call_args, "", first_inst);
}

llvm::Instruction *InstrDynPtsto::addMallocCall(llvm::Module &m,
    ObjectMap::ObjID obj_id, llvm::Value *val, llvm::Value *size_val,
    llvm::Instruction *insert_before) {
  // Make the call
  auto i32_type = llvm::IntegerType::get(m.getContext(), 32);
  auto malloc_fcn = m.getFunction(MallocInstName);
  auto i8_ptr_type = llvm::PointerType::get(
      llvm::IntegerType::get(m.getContext(), 8), 0);

  auto i8_ptr_val = new llvm::BitCastInst(val, i8_ptr_type, "", insert_before);

  std::vector<llvm::Value *> args;
  args.push_back(llvm::ConstantInt::get(i32_type, obj_id.val()));
  args.push_back(size_val);
  args.push_back(i8_ptr_val);
  auto ret = llvm::CallInst::Create(
      malloc_fcn, args, "", insert_before);

  assert(insert_before != nullptr);

  return ret;
}
//}}}

char InstrDynPtsto::ID = 0;

static llvm::RegisterPass<InstrDynPtsto> X("insert-ptsto-profiling",
    "Instruments pointsto sets, for use with SpecSFS",
    false, false);
//}}}

// The dynamic ptsto pass loader {{{
void DynPtstoLoader::getAnalysisUsage(llvm::AnalysisUsage &au) const {
  au.addRequired<UnusedFunctions>();
  au.setPreservesAll();
}

void DynPtstoLoader::setupSpecSFSids(llvm::Module &M) {
  // Set up our alias analysis
  // -- This is required for the llvm AliasAnalysis interface
  // InitializeAliasAnalysis(this);

  // Clear the def-use graph
  // It should already be cleared, but I'm paranoid
  ConstraintGraph cg;
  CFG cfg;
  ObjectMap &omap = omap_;

  if (identifyObjects(omap, M)) {
    abort();
  }

  const UnusedFunctions &unused_fcns =
      getAnalysis<UnusedFunctions>();

  ObjectMap::replaceDbgOmap(omap);
  if (createConstraints(cg, cfg, omap, M, unused_fcns)) {
    abort();
  }

  // Also add indirect info... this means we have to wait for Andersen's
  Andersens aux;
  // Get AUX info, in this instance we choose Andersens
  if (aux.runOnModule(M)) {
    // Andersens had better not change M!
    abort();
  }

  // Now, fill in the indirect function calls
  if (addIndirectCalls(cg, cfg, aux, nullptr, omap)) {
    abort();
  }
}

bool DynPtstoLoader::runOnModule(llvm::Module &m) {
  // Setup ObjectMap ids using the SpecSFS identifiers...
  setupSpecSFSids(m);

  std::ifstream logfile(DynPtstoFilename);
  if (!logfile.is_open()) {
    llvm::dbgs() << "DynPtstoLoader: no logfile found!\n";
    hasInfo_ = false;
  } else {
    llvm::dbgs() << "DynPtstoLoader: Successfully Loaded\n";
    hasInfo_ = true;

    for (std::string line; std::getline(logfile, line, ':'); ) {
      ObjectMap::ObjID call_id = ObjectMap::ObjID(stoi(line));

      auto &obj_set = valToObjs_[call_id];

      std::getline(logfile, line);

      std::stringstream converter(line);

      int32_t obj_int_val;
      converter >> obj_int_val;
      while (!converter.fail()) {
        auto obj_id = ObjectMap::ObjID(obj_int_val);

        // Don't add a ptsto for null value
        if (obj_id != ObjectMap::NullValue) {
          if_debug_enabled(auto ret =)
            obj_set.insert(obj_id);
          assert(ret.second);
        }

        converter >> obj_int_val;
      }
    }
  }

  /*
  llvm::dbgs() << "Printing loaded ptsto for top-level variables:\n";
  for (auto &pr : valToObjs_) {
    llvm::dbgs() << "Value (" << pr.first << ") is: " <<
        ValPrint(pr.first, omap_) << ":\n";

    auto &pts_set = pr.second;
    for (auto obj_id : pts_set) {
      llvm::dbgs() << "  " << ValPrint(obj_id, omap_) << "\n";
    }
  }
  */

  return false;
}

char DynPtstoLoader::ID = 0;

static llvm::RegisterPass<DynPtstoLoader> D("load-ptsto",
    "loads dynamic ptsto set info, for use with SpecSFS",
    false, false);
// }}}

