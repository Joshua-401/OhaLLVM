/*
 * Copyright (C) 2015 David Devecsery
 */

#include "include/lib/IndirFcnTarget.h"

#include <cstdio>

#include <fstream>
#include <map>
#include <string>
#include <sstream>
#include <vector>

#include "llvm/BasicBlock.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Instructions.h"
#include "llvm/InlineAsm.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/MathExtras.h"

#include "include/LLVMHelper.h"

static llvm::cl::opt<std::string>
  IndirFcnFilename("indir-info-file", llvm::cl::init("indir_fcns.log"),
      llvm::cl::value_desc("filename"),
      llvm::cl::desc("Id file loaded by indir-fcn-loader"));

static bool isIgnoredFcn(llvm::Function &fcn) {
  // Ignore intrinsic fcns
  if (fcn.isIntrinsic()) {
    return true;
  }

  // Ignore my own instrumentation functions
  if (fcn.getName().find("__InstrIndirCalls_fcn_call") == 0) {
    return true;
  }

  return false;
}

// Instrumentation pass {{{
namespace {


class InstrIndirCalls : public llvm::ModulePass {
 public:
    static char ID;

    InstrIndirCalls() : llvm::ModulePass(ID) { }

    bool runOnModule(llvm::Module &m) override;

    void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

 private:
};

void InstrIndirCalls::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool InstrIndirCalls::runOnModule(llvm::Module &m) {
  // Types:
  // Basic types
  auto i32_type = llvm::IntegerType::get(m.getContext(), 32);
  auto i64_type = llvm::IntegerType::get(m.getContext(), 64);
  auto void_type = llvm::Type::getVoidTy(m.getContext());
  auto void_ptr_type = llvm::PointerType::get(
      llvm::IntegerType::get(m.getContext(), 8), 0);
  std::vector<llvm::Type *> type_no_args;
  std::vector<llvm::Value *> no_args;

  // Function types
  auto void_fcn_type = llvm::FunctionType::get(
      void_type,
      type_no_args,
      false);
  auto ptr_void_fcn_type = void_fcn_type->getPointerTo();


  std::vector<llvm::Type *> call_type_args;
  call_type_args.push_back(i32_type);
  call_type_args.push_back(void_ptr_type);
  auto call_fcn_type = llvm::FunctionType::get(
      void_type,
      call_type_args,
      false);

  // The unique_id of the function call
  int32_t fcn_id = 0;

  auto call_fcn = llvm::Function::Create(call_fcn_type,
      llvm::GlobalValue::ExternalLinkage,
      "__InstrIndirCalls_fcn_call", &m);

  std::vector<llvm::Value *> fcn_lookup_initializer;

  for (auto &fcn : m) {
    // Add a mapping to this function type:
    // But only for real functions...
    if (!isIgnoredFcn(fcn)) {
      fcn_lookup_initializer.push_back(&fcn);
    }

    std::vector<llvm::Value *> insert_list;

    // Iterate each instruction in the function
    std::for_each(inst_begin(fcn), inst_end(fcn),
        [&m, &insert_list]
        (const llvm::Instruction &inst) {
      // Okay, lets get to work..
      if (auto cci = dyn_cast<llvm::CallInst>(&inst)) {
        auto ci = const_cast<llvm::CallInst *>(cci);
        auto fcn = LLVMHelper::getFcnFromCall(ci);

        if (fcn == nullptr) {
          insert_list.push_back(ci);
        }
      }
    });

    for (auto v : insert_list) {
      auto ci = cast<llvm::CallInst>(v);
      llvm::CallSite cs(ci);
      // TODO(ddevec): Also check if there is a unique target
      //   from andersens?
      // Add call to our instrumentation
      llvm::Value *callee = cs.getCalledValue();
      if (llvm::isa<llvm::InlineAsm>(callee)) {
        continue;
      }
      if (callee->getType() != void_ptr_type) {
        callee = new llvm::BitCastInst(callee, void_ptr_type,
          "", ci);
      }
      std::vector<llvm::Value *> args;
      args.push_back(llvm::ConstantInt::get(i32_type, fcn_id));
      args.push_back(callee);
      llvm::CallInst::Create(call_fcn,
        args,
        "",
        ci);
      // llvm::dbgs() << "id: " << fcn_id << " -> " << *ci << "\n";
      fcn_id++;
    }
  }

  auto array_type = llvm::ArrayType::get(void_ptr_type,
      fcn_lookup_initializer.size());

  // Create the array:
  auto fcn_lookup_array = new llvm::GlobalVariable(m,
      array_type,
      false,
      llvm::GlobalValue::ExternalLinkage,
      llvm::Constant::getNullValue(array_type),
      "__InstrIndirCalls_fcn_lookup_array");

  /*
  auto initializer_array = llvm::ConstantArray::get(array_type,
      fcn_lookup_initializer);

  fcn_lookup_array->setInitializer(initializer_array);
  */

  // And a lookup variable
  auto fcn_lookup_len = new llvm::GlobalVariable(m,
      i32_type,
      false,
      llvm::GlobalValue::ExternalLinkage,
      0,
      "__InstrIndirCalls_fcn_lookup_len");
  fcn_lookup_len->setInitializer(llvm::ConstantInt::get(i32_type,
        fcn_lookup_initializer.size()));

  auto num_callsites = new llvm::GlobalVariable(m,
      i32_type,
      false,
      llvm::GlobalValue::ExternalLinkage,
      0,
      "__InstrIndirCalls_num_callsites");
  num_callsites->setInitializer(llvm::ConstantInt::get(i32_type,
        fcn_id));

  // Okay, now we need to create a function which populate our array (because
  // addresses are not known until runtime, due to aslr
  auto array_init_fcn = llvm::Function::Create(void_fcn_type,
      llvm::GlobalValue::ExternalLinkage,
      "__InstrIndirCalls_array_init_fcn", &m);
  {
    auto init_entry = llvm::BasicBlock::Create(m.getContext(), "entry",
        array_init_fcn, 0);

    for (size_t i = 0; i < fcn_lookup_initializer.size(); i++) {
      auto fcn = fcn_lookup_initializer[i];
      auto fcn_void_ptr = new llvm::BitCastInst(fcn, void_ptr_type,
          "", init_entry);
      std::vector<llvm::Constant *> gep_indicies;
      gep_indicies.push_back(llvm::ConstantInt::get(i32_type, 0, false));
      gep_indicies.push_back(llvm::ConstantInt::get(i64_type, i, false));
      auto store_pos = llvm::ConstantExpr::getGetElementPtr(fcn_lookup_array,
          gep_indicies);
      new llvm::StoreInst(fcn_void_ptr, store_pos, false, init_entry);
    }

    llvm::ReturnInst::Create(m.getContext(), init_entry);
  }

  // Add inst stubs
  auto init_fcn = llvm::Function::Create(void_fcn_type,
      llvm::GlobalValue::ExternalLinkage,
      "__InstrIndirCalls_init_inst", &m);
  auto finish_fcn = llvm::Function::Create(void_fcn_type,
      llvm::GlobalValue::ExternalLinkage,
      "__InstrIndirCalls_finish_inst", &m);

  // Now, add a call to this function to the beginning of main...
  {
    auto main_fcn = m.getFunction("main");
    // get the first instruction
    auto &first_inst = *inst_begin(main_fcn);

    // Insert a function call before first
    llvm::CallInst::Create(array_init_fcn, no_args, "", &first_inst);

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
    llvm::CallInst::Create(init_fcn, no_args, "", &first_inst);
    // Setup our function to call atexit
    std::vector<llvm::Value *> atexit_call_args;
    atexit_call_args.push_back(finish_fcn);
    llvm::CallInst::Create(at_exit, atexit_call_args, "", &first_inst);
  }


  return true;
}

char InstrIndirCalls::ID = 0;
static llvm::RegisterPass<InstrIndirCalls> X("insert-indir-profiling",
    "Instruments indirect calls, for use with SpecSFS",
    false, false);
}  // namespace
//}}}

// Loader pass {{{

void IndirFunctionInfo::getAnalysisUsage(llvm::AnalysisUsage &au)
    const {
  au.setPreservesAll();
}

bool IndirFunctionInfo::runOnModule(llvm::Module &m) {
  std::string logfilename(IndirFcnFilename);

  std::map<int32_t, const llvm::Value *> id_to_fcn;
  std::map<int32_t, const llvm::Value *> id_to_call;

  int32_t fcn_count = 0;
  int32_t call_count = 0;
  // First, create a mapping of all callsites to ids
  // And all functions to ids
  for (auto &fcn : m) {
    // Add a mapping to this function type:
    // But only for real functions...
    if (!isIgnoredFcn(fcn)) {
      id_to_fcn[fcn_count] = &fcn;
      fcn_count++;
    }

    std::vector<llvm::Value *> insert_list;

    // Iterate each instruction in the function
    for (auto it = inst_begin(fcn), en = inst_end(fcn); it != en;
        ++it) {
      auto &inst = *it;
      // Okay, lets get to work..
      if (auto cci = dyn_cast<llvm::CallInst>(&inst)) {
        auto ci = const_cast<llvm::CallInst *>(cci);

        llvm::ImmutableCallSite cs(ci);
        auto cv = cs.getCalledValue();
        if (dyn_cast_or_null<llvm::InlineAsm>(cv)) {
          continue;
        }

        auto fcn = LLVMHelper::getFcnFromCall(ci);

        if (fcn == nullptr) {
          insert_list.push_back(ci);
        }
      }
    }

    for (auto v : insert_list) {
      id_to_call[call_count] = v;
      call_count++;
    }
  }

  // Now that we know the id mappings, lets parse our input file
  std::ifstream logfile(logfilename);
  if (!logfile.is_open()) {
    llvm::dbgs() << "IndirFcnInfo: no logfile found!\n";
    hasInfo_ = false;
  } else {
    llvm::dbgs() << "IndirFcnInfo: Successfully Loaded\n";
    hasInfo_ = true;
    for (std::string line; std::getline(logfile, line, ':'); ) {
      // First parse the first int till the :
      int32_t call_id = stoi(line);
      auto call = id_to_call[call_id];
      /*
      llvm::dbgs() << "Parsing indir id: " << call_id << ": " <<
        ValPrinter(call) << "\n";
      */

      /*
      auto it = callToTarget_.find(call);
      if (it == std::end(callToTarget_)) {
        // llvm::dbgs() << "Adding call to targets: " << *call << "\n";
        auto ret =
          callToTarget_.emplace(std::piecewise_construct, std::make_tuple(call),
              std::make_tuple());
        assert(ret.second);
        it = ret.first;
      }
      */
      auto rc = callToTarget_.emplace(call,
          std::vector<const llvm::Value *>());
      auto it = rc.first;
      auto &fcn_vec = it->second;

      std::getline(logfile, line);
      // Now split the line...
      std::stringstream converter(line);
      for (auto it = std::istream_iterator<uint32_t>(converter),
                en = std::istream_iterator<uint32_t>();
                it != en;
                ++it) {
        auto fcn_id = *it;
        auto fcn = cast<llvm::Function>(id_to_fcn[fcn_id]);

        fcn_vec.push_back(fcn);
      }
    }
  }

  // We dont' modify instructions
  return false;
}

char IndirFunctionInfo::ID = 0;
static llvm::RegisterPass<IndirFunctionInfo> F("load-indir",
    "Loads dynamic information about indirect callsites",
    false, false);
//}}}

