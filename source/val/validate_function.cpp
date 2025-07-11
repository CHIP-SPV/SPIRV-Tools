// Copyright (c) 2018 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>

#include "source/opcode.h"
#include "source/table2.h"
#include "source/val/instruction.h"
#include "source/val/validate.h"
#include "source/val/validation_state.h"

namespace spvtools {
namespace val {
namespace {

// Returns true if |a| and |b| are instructions defining pointers that point to
// types logically match and the decorations that apply to |b| are a subset
// of the decorations that apply to |a|.
bool DoPointeesLogicallyMatch(val::Instruction* a, val::Instruction* b,
                              ValidationState_t& _) {
  if (a->opcode() != spv::Op::OpTypePointer ||
      b->opcode() != spv::Op::OpTypePointer) {
    return false;
  }

  const auto& dec_a = _.id_decorations(a->id());
  const auto& dec_b = _.id_decorations(b->id());
  for (const auto& dec : dec_b) {
    if (std::find(dec_a.begin(), dec_a.end(), dec) == dec_a.end()) {
      return false;
    }
  }

  uint32_t a_type = a->GetOperandAs<uint32_t>(2);
  uint32_t b_type = b->GetOperandAs<uint32_t>(2);

  if (a_type == b_type) {
    return true;
  }

  Instruction* a_type_inst = _.FindDef(a_type);
  Instruction* b_type_inst = _.FindDef(b_type);

  return _.LogicallyMatch(a_type_inst, b_type_inst, true);
}

spv_result_t ValidateFunction(ValidationState_t& _, const Instruction* inst) {
  const auto function_type_id = inst->GetOperandAs<uint32_t>(3);
  const auto function_type = _.FindDef(function_type_id);
  if (!function_type || spv::Op::OpTypeFunction != function_type->opcode()) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpFunction Function Type <id> " << _.getIdName(function_type_id)
           << " is not a function type.";
  }

  const auto return_id = function_type->GetOperandAs<uint32_t>(1);
  if (return_id != inst->type_id()) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpFunction Result Type <id> " << _.getIdName(inst->type_id())
           << " does not match the Function Type's return type <id> "
           << _.getIdName(return_id) << ".";
  }

  std::vector<spv::Op> acceptable = {
      spv::Op::OpGroupDecorate,
      spv::Op::OpDecorate,
      spv::Op::OpEnqueueKernel,
      spv::Op::OpEntryPoint,
      spv::Op::OpExecutionMode,
      spv::Op::OpExecutionModeId,
      spv::Op::OpFunctionCall,
      spv::Op::OpGetKernelNDrangeSubGroupCount,
      spv::Op::OpGetKernelNDrangeMaxSubGroupSize,
      spv::Op::OpGetKernelWorkGroupSize,
      spv::Op::OpGetKernelPreferredWorkGroupSizeMultiple,
      spv::Op::OpGetKernelLocalSizeForSubgroupCount,
      spv::Op::OpGetKernelMaxNumSubgroups,
      spv::Op::OpName,
      spv::Op::OpCooperativeMatrixPerElementOpNV,
      spv::Op::OpCooperativeMatrixReduceNV,
      spv::Op::OpCooperativeMatrixLoadTensorNV};
  if (_.HasCapability(spv::Capability::FunctionPointersINTEL))
    acceptable.push_back(spv::Op::OpConstantFunctionPointerINTEL);
  for (auto& pair : inst->uses()) {
    const auto* use = pair.first;
    if (std::find(acceptable.begin(), acceptable.end(), use->opcode()) ==
            acceptable.end() &&
        !use->IsNonSemantic() && !use->IsDebugInfo()) {
      return _.diag(SPV_ERROR_INVALID_ID, use)
             << "Invalid use of function result id " << _.getIdName(inst->id())
             << ".";
    }
  }

  return SPV_SUCCESS;
}

spv_result_t ValidateFunctionParameter(ValidationState_t& _,
                                       const Instruction* inst) {
  // NOTE: Find OpFunction & ensure OpFunctionParameter is not out of place.
  size_t param_index = 0;
  size_t inst_num = inst->LineNum() - 1;
  if (inst_num == 0) {
    return _.diag(SPV_ERROR_INVALID_LAYOUT, inst)
           << "Function parameter cannot be the first instruction.";
  }

  auto func_inst = &_.ordered_instructions()[inst_num];
  while (--inst_num) {
    func_inst = &_.ordered_instructions()[inst_num];
    if (func_inst->opcode() == spv::Op::OpFunction) {
      break;
    } else if (func_inst->opcode() == spv::Op::OpFunctionParameter) {
      ++param_index;
    }
  }

  if (func_inst->opcode() != spv::Op::OpFunction) {
    return _.diag(SPV_ERROR_INVALID_LAYOUT, inst)
           << "Function parameter must be preceded by a function.";
  }

  const auto function_type_id = func_inst->GetOperandAs<uint32_t>(3);
  const auto function_type = _.FindDef(function_type_id);
  if (!function_type) {
    return _.diag(SPV_ERROR_INVALID_ID, func_inst)
           << "Missing function type definition.";
  }
  if (param_index >= function_type->words().size() - 3) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "Too many OpFunctionParameters for " << func_inst->id()
           << ": expected " << function_type->words().size() - 3
           << " based on the function's type";
  }

  const auto param_type =
      _.FindDef(function_type->GetOperandAs<uint32_t>(param_index + 2));
  if (!param_type || inst->type_id() != param_type->id()) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpFunctionParameter Result Type <id> "
           << _.getIdName(inst->type_id())
           << " does not match the OpTypeFunction parameter "
              "type of the same index.";
  }

  return SPV_SUCCESS;
}

spv_result_t ValidateFunctionCall(ValidationState_t& _,
                                  const Instruction* inst) {
  const auto function_id = inst->GetOperandAs<uint32_t>(2);
  const auto function = _.FindDef(function_id);
  if (!function || spv::Op::OpFunction != function->opcode()) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpFunctionCall Function <id> " << _.getIdName(function_id)
           << " is not a function.";
  }

  auto return_type = _.FindDef(function->type_id());
  if (!return_type || return_type->id() != inst->type_id()) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpFunctionCall Result Type <id> " << _.getIdName(inst->type_id())
           << "s type does not match Function <id> "
           << _.getIdName(return_type->id()) << "s return type.";
  }

  const auto function_type_id = function->GetOperandAs<uint32_t>(3);
  const auto function_type = _.FindDef(function_type_id);
  if (!function_type || function_type->opcode() != spv::Op::OpTypeFunction) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "Missing function type definition.";
  }

  const auto function_call_arg_count = inst->words().size() - 4;
  const auto function_param_count = function_type->words().size() - 3;
  if (function_param_count != function_call_arg_count) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpFunctionCall Function <id>'s parameter count does not match "
              "the argument count.";
  }

  for (size_t argument_index = 3, param_index = 2;
       argument_index < inst->operands().size();
       argument_index++, param_index++) {
    const auto argument_id = inst->GetOperandAs<uint32_t>(argument_index);
    const auto argument = _.FindDef(argument_id);
    if (!argument) {
      return _.diag(SPV_ERROR_INVALID_ID, inst)
             << "Missing argument " << argument_index - 3 << " definition.";
    }

    const auto argument_type = _.FindDef(argument->type_id());
    if (!argument_type) {
      return _.diag(SPV_ERROR_INVALID_ID, inst)
             << "Missing argument " << argument_index - 3
             << " type definition.";
    }

    const auto parameter_type_id =
        function_type->GetOperandAs<uint32_t>(param_index);
    const auto parameter_type = _.FindDef(parameter_type_id);
    if (!parameter_type || argument_type->id() != parameter_type->id()) {
      if (!parameter_type || !_.options()->before_hlsl_legalization ||
          !DoPointeesLogicallyMatch(argument_type, parameter_type, _)) {
        return _.diag(SPV_ERROR_INVALID_ID, inst)
               << "OpFunctionCall Argument <id> " << _.getIdName(argument_id)
               << "s type does not match Function <id> "
               << _.getIdName(parameter_type_id) << "s parameter type.";
      }
    }

    if (_.addressing_model() == spv::AddressingModel::Logical) {
      if ((parameter_type->opcode() == spv::Op::OpTypePointer ||
           parameter_type->opcode() == spv::Op::OpTypeUntypedPointerKHR) &&
          !_.options()->relax_logical_pointer) {
        spv::StorageClass sc =
            parameter_type->GetOperandAs<spv::StorageClass>(1u);
        // Validate which storage classes can be pointer operands.
        switch (sc) {
          case spv::StorageClass::UniformConstant:
          case spv::StorageClass::Function:
          case spv::StorageClass::Private:
          case spv::StorageClass::Workgroup:
          case spv::StorageClass::AtomicCounter:
            // These are always allowed.
            break;
          case spv::StorageClass::StorageBuffer:
            if (!_.features().variable_pointers) {
              return _.diag(SPV_ERROR_INVALID_ID, inst)
                     << "StorageBuffer pointer operand "
                     << _.getIdName(argument_id)
                     << " requires a variable pointers capability";
            }
            break;
          default:
            return _.diag(SPV_ERROR_INVALID_ID, inst)
                   << "Invalid storage class for pointer operand "
                   << _.getIdName(argument_id);
        }

        // Validate memory object declaration requirements.
        if (argument->opcode() != spv::Op::OpVariable &&
            argument->opcode() != spv::Op::OpUntypedVariableKHR &&
            argument->opcode() != spv::Op::OpFunctionParameter) {
          const bool ssbo_vptr =
              _.HasCapability(spv::Capability::VariablePointersStorageBuffer) &&
              sc == spv::StorageClass::StorageBuffer;
          const bool wg_vptr =
              _.HasCapability(spv::Capability::VariablePointers) &&
              sc == spv::StorageClass::Workgroup;
          const bool uc_ptr = sc == spv::StorageClass::UniformConstant;
          if (!_.options()->before_hlsl_legalization && !ssbo_vptr &&
              !wg_vptr && !uc_ptr) {
            return _.diag(SPV_ERROR_INVALID_ID, inst)
                   << "Pointer operand " << _.getIdName(argument_id)
                   << " must be a memory object declaration";
          }
        }
      }
    }
  }
  return SPV_SUCCESS;
}

spv_result_t ValidateCooperativeMatrixPerElementOp(ValidationState_t& _,
                                                   const Instruction* inst) {
  const auto function_id = inst->GetOperandAs<uint32_t>(3);
  const auto function = _.FindDef(function_id);
  if (!function || spv::Op::OpFunction != function->opcode()) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpCooperativeMatrixPerElementOpNV Function <id> "
           << _.getIdName(function_id) << " is not a function.";
  }

  const auto matrix_id = inst->GetOperandAs<uint32_t>(2);
  const auto matrix = _.FindDef(matrix_id);
  const auto matrix_type_id = matrix->type_id();
  if (!_.IsCooperativeMatrixKHRType(matrix_type_id)) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpCooperativeMatrixPerElementOpNV Matrix <id> "
           << _.getIdName(matrix_id) << " is not a cooperative matrix.";
  }

  const auto result_type_id = inst->GetOperandAs<uint32_t>(0);
  if (matrix_type_id != result_type_id) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpCooperativeMatrixPerElementOpNV Result Type <id> "
           << _.getIdName(result_type_id) << " must match matrix type <id> "
           << _.getIdName(matrix_type_id) << ".";
  }

  const auto matrix_comp_type_id =
      _.FindDef(matrix_type_id)->GetOperandAs<uint32_t>(1);
  const auto function_type_id = function->GetOperandAs<uint32_t>(3);
  const auto function_type = _.FindDef(function_type_id);
  auto return_type_id = function_type->GetOperandAs<uint32_t>(1);
  if (return_type_id != matrix_comp_type_id) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpCooperativeMatrixPerElementOpNV function return type <id> "
           << _.getIdName(return_type_id)
           << " must match matrix component type <id> "
           << _.getIdName(matrix_comp_type_id) << ".";
  }

  if (function_type->operands().size() < 5) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpCooperativeMatrixPerElementOpNV function type <id> "
           << _.getIdName(function_type_id)
           << " must have a least three parameters.";
  }

  const auto param0_id = function_type->GetOperandAs<uint32_t>(2);
  const auto param1_id = function_type->GetOperandAs<uint32_t>(3);
  const auto param2_id = function_type->GetOperandAs<uint32_t>(4);
  if (!_.IsIntScalarType(param0_id) || _.GetBitWidth(param0_id) != 32) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpCooperativeMatrixPerElementOpNV function type first parameter "
              "type <id> "
           << _.getIdName(param0_id) << " must be a 32-bit integer.";
  }

  if (!_.IsIntScalarType(param1_id) || _.GetBitWidth(param1_id) != 32) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpCooperativeMatrixPerElementOpNV function type second "
              "parameter type <id> "
           << _.getIdName(param1_id) << " must be a 32-bit integer.";
  }

  if (param2_id != matrix_comp_type_id) {
    return _.diag(SPV_ERROR_INVALID_ID, inst)
           << "OpCooperativeMatrixPerElementOpNV function type third parameter "
              "type <id> "
           << _.getIdName(param2_id) << " must match matrix component type.";
  }

  return SPV_SUCCESS;
}

}  // namespace

spv_result_t FunctionPass(ValidationState_t& _, const Instruction* inst) {
  switch (inst->opcode()) {
    case spv::Op::OpFunction:
      if (auto error = ValidateFunction(_, inst)) return error;
      break;
    case spv::Op::OpFunctionParameter:
      if (auto error = ValidateFunctionParameter(_, inst)) return error;
      break;
    case spv::Op::OpFunctionCall:
      if (auto error = ValidateFunctionCall(_, inst)) return error;
      break;
    case spv::Op::OpCooperativeMatrixPerElementOpNV:
      if (auto error = ValidateCooperativeMatrixPerElementOp(_, inst))
        return error;
      break;
    default:
      break;
  }

  return SPV_SUCCESS;
}

}  // namespace val
}  // namespace spvtools
