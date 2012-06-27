//===--- IRGenModule.cpp - Swift Global LLVM IR Generation ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for global declarations in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Diagnostics.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Intrinsics.h"
#include "llvm/Module.h"
#include "llvm/Type.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Target/TargetData.h"

#include "GenType.h"
#include "IRGenModule.h"
#include "Linking.h"

using namespace swift;
using namespace irgen;

IRGenModule::IRGenModule(ASTContext &Context,
			 Options &Opts, llvm::Module &Module,
                         const llvm::TargetData &TargetData)
  : Context(Context), Opts(Opts),
    Module(Module), LLVMContext(Module.getContext()),
    TargetData(TargetData), Types(*new TypeConverter(*this)) {
  VoidTy = llvm::Type::getVoidTy(getLLVMContext());
  Int1Ty = llvm::Type::getInt1Ty(getLLVMContext());
  Int8Ty = llvm::Type::getInt8Ty(getLLVMContext());
  Int16Ty = llvm::Type::getInt16Ty(getLLVMContext());
  Int32Ty = llvm::Type::getInt32Ty(getLLVMContext());
  Int64Ty = llvm::Type::getInt64Ty(getLLVMContext());
  Int8PtrTy = llvm::Type::getInt8PtrTy(getLLVMContext());
  Int8PtrPtrTy = Int8PtrTy->getPointerTo(0);
  SizeTy = TargetData.getIntPtrType(getLLVMContext());
  MemCpyFn = nullptr;
  AllocObjectFn = nullptr;
  RetainNoResultFn = nullptr;
  ReleaseFn = nullptr;
  DeallocObjectFn = nullptr;
  ObjCRetainFn = nullptr;
  ObjCReleaseFn = nullptr;
  RawAllocFn = nullptr;
  RawDeallocFn = nullptr;
  SlowAllocFn = nullptr;
  SlowRawDeallocFn = nullptr;

  RefCountedStructTy =
    llvm::StructType::create(getLLVMContext(), "swift.refcounted");
  RefCountedPtrTy = RefCountedStructTy->getPointerTo(/*addrspace*/ 0);
  RefCountedNull = llvm::ConstantPointerNull::get(RefCountedPtrTy);

  DtorTy = llvm::FunctionType::get(SizeTy, RefCountedPtrTy, false);
  llvm::Type *dtorPtrTy = DtorTy->getPointerTo();
  llvm::Type *heapMetadataElts[] = { dtorPtrTy, dtorPtrTy };
  HeapMetadataStructTy =
    llvm::StructType::create(getLLVMContext(), heapMetadataElts,
                             "swift.heapmetadata");
  HeapMetadataPtrTy = HeapMetadataStructTy->getPointerTo(/*addrspace*/ 0);

  llvm::Type *refCountedElts[] = { HeapMetadataPtrTy, SizeTy };
  RefCountedStructTy->setBody(refCountedElts);

  PtrSize = Size(TargetData.getPointerSize());

  llvm::Type *funcElts[] = { Int8PtrTy, RefCountedPtrTy };
  FunctionPairTy = llvm::StructType::get(LLVMContext, funcElts,
                                         /*packed*/ false);

  OpaqueStructTy = nullptr;
  FixedBufferTy = nullptr;
  for (unsigned i = 0; i != NumValueWitnesses; ++i)
    ValueWitnessTys[i] = nullptr;

  ObjCPtrTy = llvm::StructType::create(getLLVMContext(), "objc_object")
                ->getPointerTo(0);

  // TODO: use "tinycc" on platforms that support it
  RuntimeCC = llvm::CallingConv::C;
}

IRGenModule::~IRGenModule() {
  delete &Types;
}

llvm::StructType *IRGenModule::getOpaqueStructTy() {
  if (OpaqueStructTy) return OpaqueStructTy;
  OpaqueStructTy = llvm::StructType::create(LLVMContext, "swift.opaque");
  return OpaqueStructTy;
}

static llvm::Constant *createRuntimeFunction(IRGenModule &IGM, StringRef name,
                                             llvm::FunctionType *fnType) {
  llvm::Constant *addr = IGM.Module.getOrInsertFunction(name, fnType);
  if (auto fn = dyn_cast<llvm::Function>(addr))
    fn->setCallingConv(IGM.RuntimeCC);
  return addr;
}

llvm::Constant *IRGenModule::getAllocObjectFn() {
  if (AllocObjectFn) return AllocObjectFn;

  llvm::Type *types[] = { HeapMetadataPtrTy, SizeTy, SizeTy };
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(RefCountedPtrTy, types, false);
  AllocObjectFn = createRuntimeFunction(*this, "swift_allocObject", fnType);
  return AllocObjectFn;
}

llvm::Constant *IRGenModule::getRawAllocFn() {
  if (RawAllocFn) return RawAllocFn;

  /// void *swift_rawAlloc(SwiftAllocIndex index);
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(Int8PtrTy, SizeTy, false);
  RawAllocFn = createRuntimeFunction(*this, "swift_rawAlloc", fnType);
  return RawAllocFn;
}

llvm::Constant *IRGenModule::getRawDeallocFn() {
  if (RawDeallocFn) return RawDeallocFn;

  /// void swift_rawDealloc(void *ptr, SwiftAllocIndex index);
  llvm::Type *types[] = { Int8PtrTy, SizeTy };
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(VoidTy, types, false);
  RawDeallocFn = createRuntimeFunction(*this, "swift_rawDealloc", fnType);
  return RawDeallocFn;
}

llvm::Constant *IRGenModule::getSlowAllocFn() {
  if (SlowAllocFn) return SlowAllocFn;

  /// void *swift_slowAlloc(size_t size, size_t flags);
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(Int8PtrTy, SizeTy, false);
  SlowAllocFn = createRuntimeFunction(*this, "swift_slowAlloc", fnType);
  return SlowAllocFn;
}

llvm::Constant *IRGenModule::getSlowRawDeallocFn() {
  if (SlowRawDeallocFn) return SlowRawDeallocFn;

  /// void swift_slowRawDealloc(void *ptr, size_t size);
  llvm::Type *types[] = { Int8PtrTy, SizeTy };
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(VoidTy, types, false);
  SlowRawDeallocFn = createRuntimeFunction(*this, "swift_slowRawDealloc", fnType);
  return SlowRawDeallocFn;
}

llvm::Constant *IRGenModule::getRetainNoResultFn() {
  if (RetainNoResultFn) return RetainNoResultFn;

  // void swift_retainNoResult(void *ptr);
  auto fnType = llvm::FunctionType::get(VoidTy, RefCountedPtrTy, false);
  RetainNoResultFn = createRuntimeFunction(*this, "swift_retain_noresult", fnType);
  if (auto fn = dyn_cast<llvm::Function>(RetainNoResultFn))
    fn->setDoesNotCapture(1);
  return RetainNoResultFn;
}

llvm::Constant *IRGenModule::getReleaseFn() {
  if (ReleaseFn) return ReleaseFn;

  // void swift_release(void *ptr);
  auto fnType = llvm::FunctionType::get(VoidTy, RefCountedPtrTy, false);
  ReleaseFn = createRuntimeFunction(*this, "swift_release", fnType);
  if (auto fn = dyn_cast<llvm::Function>(ReleaseFn))
    fn->setDoesNotCapture(1);
  return ReleaseFn;
}

llvm::Constant *IRGenModule::getDeallocObjectFn() {
  if (DeallocObjectFn) return DeallocObjectFn;

  // void swift_deallocObject(void *ptr, size_t size);
  llvm::Type *argTypes[] = { RefCountedPtrTy, SizeTy };
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(VoidTy, argTypes, false);
  DeallocObjectFn = createRuntimeFunction(*this, "swift_deallocObject", fnType);
  return DeallocObjectFn;
}

void IRGenModule::unimplemented(SourceLoc loc, StringRef message) {
  Context.Diags.diagnose(loc, diag::irgen_unimplemented, message);
}

void IRGenModule::error(SourceLoc loc, const Twine &message) {
  SmallVector<char, 128> buffer;
  Context.Diags.diagnose(loc, diag::irgen_failure,
                         message.toStringRef(buffer));
}
