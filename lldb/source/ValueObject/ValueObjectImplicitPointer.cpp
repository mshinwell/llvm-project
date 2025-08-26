//===-- ValueObjectImplicitPointer.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/ValueObject/ValueObjectImplicitPointer.h"
#include "lldb/Core/Value.h"
#include "lldb/Core/dwarf.h"
#include "lldb/Expression/DWARFExpression.h"
#include "lldb/Expression/DWARFExpressionList.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/Status.h"
#include "lldb/ValueObject/ValueObjectConstResult.h"
#include "lldb/ValueObject/ValueObjectMemory.h"
#include "Plugins/SymbolFile/DWARF/SymbolFileDWARF.h"
#include "Plugins/SymbolFile/DWARF/DWARFAttribute.h"
#include "Plugins/SymbolFile/DWARF/DWARFDataExtractor.h"
#include "Plugins/SymbolFile/DWARF/DWARFDIE.h"
#include "Plugins/SymbolFile/DWARF/DWARFDebugInfoEntry.h"
#include "Plugins/SymbolFile/DWARF/DWARFFormValue.h"
#include "Plugins/SymbolFile/DWARF/DWARFUnit.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include <cstring>

using namespace lldb;
using namespace lldb_private;

ValueObjectImplicitPointer::ValueObjectImplicitPointer(
    ExecutionContext &exe_ctx, ValueObjectManager &manager,
    CompilerType pointer_type, ConstString name, 
    uint64_t die_offset, int64_t byte_offset)
    : ValueObject(exe_ctx.GetTargetPtr(), manager),
      m_pointer_type(pointer_type), m_die_offset(die_offset),
      m_byte_offset(byte_offset) {
  SetName(name);
  // Set the value type to indicate this is an implicit pointer
  m_value.SetValueType(Value::ValueType::HostAddress);
}

lldb::ValueObjectSP ValueObjectImplicitPointer::Create(
    ExecutionContext &exe_ctx, CompilerType pointer_type, ConstString name,
    uint64_t die_offset, int64_t byte_offset) {
  
  auto manager_sp = ValueObjectManager::Create();
  return (new ValueObjectImplicitPointer(
      exe_ctx, *manager_sp, pointer_type, name, die_offset, byte_offset))->GetSP();
}

llvm::Expected<uint64_t> ValueObjectImplicitPointer::GetByteSize() {
  if (m_pointer_type.IsValid()) {
    ExecutionContext exe_ctx(GetExecutionContextRef());
    if (auto size = m_pointer_type.GetByteSize(
            exe_ctx.GetBestExecutionContextScope()))
      return *size;
  }
  return llvm::createStringError("Invalid pointer type");
}

bool ValueObjectImplicitPointer::IsInScope() {
  // Implicit pointers are valid as long as we have debug info
  return true;
}

bool ValueObjectImplicitPointer::UpdateValue() {
  SetValueIsValid(false);
  m_error.Clear();

  ExecutionContext exe_ctx(GetExecutionContextRef());
  Target *target = exe_ctx.GetTargetPtr();
  
  if (!target) {
    m_error = Status::FromErrorString("No target available");
    return false;
  }

  // For an implicit pointer, we don't have an actual address
  // We store the DIE offset as a marker
  m_value.GetScalar() = m_die_offset;
  m_value.SetValueType(Value::ValueType::HostAddress);
  m_value.SetCompilerType(m_pointer_type);
  
  // Store the byte offset in the data buffer for later use
  DataBufferHeap &buffer = m_value.GetBuffer();
  buffer.CopyData(&m_byte_offset, sizeof(m_byte_offset));
  
  SetValueIsValid(true);
  return true;
}

ValueObjectSP ValueObjectImplicitPointer::Dereference(Status &error) {
  if (m_deref_valobj)
    return m_deref_valobj->GetSP();

  ExecutionContext exe_ctx(GetExecutionContextRef());
  Target *target = exe_ctx.GetTargetPtr();
  
  if (!target) {
    error = Status::FromErrorString("No target available");
    return ValueObjectSP();
  }

  // Get the module to access DWARF info
  ModuleSP module_sp = GetModule();
  if (!module_sp) {
    error = Status::FromErrorString("No module available for implicit pointer");
    return ValueObjectSP();
  }

  // Get the DWARF symbol file
  SymbolFile *symbol_file = module_sp->GetSymbolFile();
  if (!symbol_file) {
    error = Status::FromErrorString("No symbol file available");
    return ValueObjectSP();
  }

  // Cast to SymbolFileDWARF to access DWARF-specific functionality
  auto *dwarf_symbol_file = llvm::dyn_cast<lldb_private::plugin::dwarf::SymbolFileDWARF>(symbol_file);
  if (!dwarf_symbol_file) {
    error = Status::FromErrorString("Symbol file is not DWARF");
    return ValueObjectSP();
  }

  // Look up the DIE at the given offset
  lldb_private::plugin::dwarf::DWARFDIE die = dwarf_symbol_file->GetDIE(m_die_offset);
  if (!die.IsValid()) {
    error = Status::FromErrorString("Invalid DIE offset for implicit pointer");
    return ValueObjectSP();
  }

  // Check what kind of DIE this is
  dw_tag_t tag = die.Tag();
  
  // The DIE should be a variable, parameter, or similar
  if (tag != llvm::dwarf::DW_TAG_variable && 
      tag != llvm::dwarf::DW_TAG_formal_parameter &&
      tag != llvm::dwarf::DW_TAG_dwarf_procedure) {
    error = Status::FromErrorString("Referenced DIE is not a variable or parameter");
    return ValueObjectSP();
  }

  // Get the type of the referenced DIE
  // For now, use the pointer's pointee type as a placeholder
  CompilerType type = m_pointer_type.GetPointeeType();
  if (!type.IsValid()) {
    error = Status::FromErrorString("Could not determine type from pointer");
    return ValueObjectSP();
  }
  
  // Get variable name from DIE
  const char* name = die.GetName();
  if (!name)
    name = "<anonymous>";
  
  // Extract attributes from the DIE
  lldb_private::plugin::dwarf::DWARFAttributes attributes = die.GetAttributes();
  lldb_private::plugin::dwarf::DWARFFormValue location_form;
  lldb_private::plugin::dwarf::DWARFFormValue const_value_form;
  
  for (size_t i = 0; i < attributes.Size(); ++i) {
    dw_attr_t attr = attributes.AttributeAtIndex(i);
    lldb_private::plugin::dwarf::DWARFFormValue form_value;
    
    if (!attributes.ExtractFormValueAtIndex(i, form_value))
      continue;
      
    switch (attr) {
    case llvm::dwarf::DW_AT_location:
      location_form = form_value;
      break;
    case llvm::dwarf::DW_AT_const_value:
      const_value_form = form_value;
      break;
    default:
      break;
    }
  }
  
  // Prefer DW_AT_location over DW_AT_const_value (following LLDB convention)
  bool has_location = location_form.IsValid();
  bool has_const_value = const_value_form.IsValid();
  
  DWARFExpressionList location_list;
  
  if (has_location) {
    // Extract location expression
    if (lldb_private::plugin::dwarf::DWARFFormValue::IsBlockForm(location_form.Form())) {
      const DWARFDataExtractor &data = die.GetData();
      uint64_t block_offset = location_form.BlockData() - data.GetDataStart();
      uint64_t block_length = location_form.Unsigned();
      
      location_list = DWARFExpressionList(
          module_sp, DataExtractor(data, block_offset, block_length), die.GetCU());
    } else {
      // Handle location lists
      DataExtractor data = die.GetCU()->GetLocationData();
      dw_offset_t offset = location_form.Unsigned();
      if (location_form.Form() == llvm::dwarf::DW_FORM_loclistx)
        offset = die.GetCU()->GetLoclistOffset(offset).value_or(-1);
        
      if (data.ValidOffset(offset)) {
        // Parse the location list properly
        data = DataExtractor(data, offset, data.GetByteSize() - offset);
        location_list = DWARFExpressionList(module_sp, data, die.GetCU());
      }
    }
  } else if (has_const_value) {
    // Extract constant value
    const DWARFDataExtractor &debug_info_data = die.GetData();
    if (lldb_private::plugin::dwarf::DWARFFormValue::IsBlockForm(const_value_form.Form())) {
      uint64_t block_offset = const_value_form.BlockData() - debug_info_data.GetDataStart();
      uint64_t block_length = const_value_form.Unsigned();
      
      location_list = DWARFExpressionList(
          module_sp, DataExtractor(debug_info_data, block_offset, block_length), die.GetCU());
    } else if (const char *str = const_value_form.AsCString()) {
      // String constant
      location_list = DWARFExpressionList(
          module_sp, DataExtractor(str, strlen(str) + 1,
                                  target->GetArchitecture().GetByteOrder(),
                                  target->GetArchitecture().GetAddressByteSize()),
          die.GetCU());
    } else {
      // Numeric constant - create a synthetic value
      uint64_t const_value = const_value_form.Unsigned();
      DataBufferSP buffer_sp = std::make_shared<DataBufferHeap>(sizeof(const_value), 0);
      memcpy(const_cast<uint8_t*>(buffer_sp->GetBytes()), &const_value, sizeof(const_value));
      
      DataExtractor const_data(buffer_sp, target->GetArchitecture().GetByteOrder(),
                               target->GetArchitecture().GetAddressByteSize());
      location_list = DWARFExpressionList(module_sp, const_data, die.GetCU());
    }
  }
  
  // Evaluate the location expression to get the value
  lldb::ValueObjectSP result_sp;
  
  if (location_list.IsValid()) {
    // Evaluate the location expression
    // This should produce a value in debugger memory for implicit pointers
    llvm::Expected<Value> maybe_value = location_list.Evaluate(
        &exe_ctx, nullptr, LLDB_INVALID_ADDRESS, nullptr, nullptr);
    
    if (maybe_value) {
      Value &value = *maybe_value;
      
      // Apply byte offset if needed
      if (m_byte_offset != 0) {
        // Adjust the address or data offset
        if (value.GetValueType() == Value::ValueType::HostAddress) {
          // For host address values, adjust the pointer
          value.GetScalar() += m_byte_offset;
        }
      }
      
      // Create a ValueObject from the evaluated value
      // If the DIE represents a struct/class, the resulting ValueObject
      // will have children for the struct's fields. This allows navigation
      // through implicit pointers to complex types just like regular pointers.
      result_sp = ValueObjectConstResult::Create(
          exe_ctx.GetBestExecutionContextScope(), 
          value, ConstString(name), module_sp.get());
          
      if (result_sp) {
        // Mark this as synthetic/implicit data
        result_sp->SetSyntheticChildrenGenerated(true);
        m_deref_valobj = result_sp.get();
        error.Clear();
        return result_sp;
      }
    } else {
      error = Status::FromError(maybe_value.takeError());
      return ValueObjectSP();
    }
  }
  
  // Fallback: create a synthetic value if we couldn't extract location
  auto type_size_or_err = type.GetByteSize(nullptr);
  size_t type_size = type_size_or_err ? *type_size_or_err : 8;
  DataBufferSP buffer_sp = std::make_shared<DataBufferHeap>(type_size, 0);
  
  result_sp = ValueObjectConstResult::Create(
      exe_ctx.GetBestExecutionContextScope(), type, ConstString(name), 
      buffer_sp, target->GetArchitecture().GetByteOrder(),
      target->GetArchitecture().GetAddressByteSize(), LLDB_INVALID_ADDRESS);
  
  if (result_sp) {
    result_sp->SetSyntheticChildrenGenerated(true);
    m_deref_valobj = result_sp.get();
    error.Clear();
    return result_sp;
  }
  
  error = Status::FromErrorString("Failed to create dereferenced value object");
  return ValueObjectSP();
}

