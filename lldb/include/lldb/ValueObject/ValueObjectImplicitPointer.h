//===-- ValueObjectImplicitPointer.h ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_VALUEOBJECT_VALUEOBJECTIMPLICITPOINTER_H
#define LLDB_VALUEOBJECT_VALUEOBJECTIMPLICITPOINTER_H

#include "lldb/ValueObject/ValueObject.h"
#include "lldb/lldb-forward.h"

namespace lldb_private {

/// Represents an implicit pointer as described by DWARF DW_OP_implicit_pointer.
/// 
/// An implicit pointer is a pointer that cannot be represented as a real pointer
/// in memory, even though the value it would point to can be described. This 
/// happens when optimizing compilers eliminate a pointer while still retaining 
/// the value that the pointer addressed.
class ValueObjectImplicitPointer : public ValueObject {
public:
  ~ValueObjectImplicitPointer() override = default;

  static lldb::ValueObjectSP Create(ExecutionContext &exe_ctx,
                                    CompilerType pointer_type,
                                    ConstString name, 
                                    uint64_t die_offset,
                                    int64_t byte_offset);

  // ValueObject overrides
  llvm::Expected<uint64_t> GetByteSize() override;
  
  lldb::ValueType GetValueType() const override {
    return lldb::eValueTypeImplicitPointer;
  }
  
  bool IsInScope() override;
  
  bool IsDereferenceOfParent() override { return false; }
  
  bool UpdateValue() override;
  
  // Dereferencing an implicit pointer evaluates the referenced DIE's location
  lldb::ValueObjectSP Dereference(Status &error) override;
  
  // Can't take the address of an implicit pointer
  lldb::ValueObjectSP AddressOf(Status &error) override {
    error = Status::FromErrorString(
        "Cannot take the address of an implicit pointer");
    return lldb::ValueObjectSP();
  }
  
  CompilerType GetCompilerTypeImpl() override {
    return m_pointer_type;
  }
  
protected:
  llvm::Expected<uint32_t> 
  CalculateNumChildren(uint32_t max = UINT32_MAX) override {
    // Implicit pointers themselves don't have children, but when pointing 
    // to structures, the dereferenced value may have children. Those are
    // accessed through Dereference(), not as direct children of the pointer.
    // 
    // For example, if we have an implicit pointer to a struct:
    //   struct S { int x, y; } *p;  
    // Then p has no children, but *p has children x and y.
    //
    // This is consistent with how regular pointers work - the pointer
    // itself has no children, but the dereferenced value might.
    return 0;
  }
  
  ValueObjectImplicitPointer(ExecutionContext &exe_ctx,
                             ValueObjectManager &manager,
                             CompilerType pointer_type,
                             ConstString name, 
                             uint64_t die_offset,
                             int64_t byte_offset);
  
private:
  CompilerType m_pointer_type;  // The pointer type
  uint64_t m_die_offset;         // Offset to the DIE describing the dereferenced value
  int64_t m_byte_offset;         // Byte offset from the start of the dereferenced value
  
  ValueObjectImplicitPointer(const ValueObjectImplicitPointer &) = delete;
  const ValueObjectImplicitPointer &
  operator=(const ValueObjectImplicitPointer &) = delete;
};

} // namespace lldb_private

#endif // LLDB_VALUEOBJECT_VALUEOBJECTIMPLICITPOINTER_H