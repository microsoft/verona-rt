// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "compiler/codegen/builtins.h"

#include "compiler/codegen/generator.h"

namespace verona::compiler
{
  using bytecode::Opcode;

  /* static */
  void BuiltinGenerator::generate(
    Context& context, Generator& gen, const CodegenItem<Method>& method)
  {
    FunctionABI abi(*method.definition->signature);
    BuiltinGenerator v(context, gen, abi);
    v.generate_header(method.instantiated_path());
    v.generate_builtin(
      method.definition->parent->name, method.definition->name);
    v.finish();
  }

  void BuiltinGenerator::generate_builtin(
    std::string_view entity, std::string_view method)
  {
    if (entity == "Builtin")
    {
      if (method.rfind("print", 0) == 0)
        return builtin_print();
      else if (method == "create_sleeping_cown")
        return builtin_create_sleeping_cown();
      else if (method == "fulfill_sleeping_cown")
        return builtin_fulfill_sleeping_cown();
      else if (method == "trace")
        return builtin_trace_region();
    }
    else if (entity == "U64")
    {
      if (method == "add")
        return builtin_binop(bytecode::BinaryOperator::Add);
      else if (method == "sub")
        return builtin_binop(bytecode::BinaryOperator::Sub);
      else if (method == "lt")
        return builtin_binop(bytecode::BinaryOperator::Lt);
      else if (method == "gt")
        return builtin_binop(bytecode::BinaryOperator::Gt);
      else if (method == "le")
        return builtin_binop(bytecode::BinaryOperator::Le);
      else if (method == "ge")
        return builtin_binop(bytecode::BinaryOperator::Ge);
      else if (method == "eq")
        return builtin_binop(bytecode::BinaryOperator::Eq);
      else if (method == "ne")
        return builtin_binop(bytecode::BinaryOperator::Ne);
      else if (method == "and")
        return builtin_binop(bytecode::BinaryOperator::And);
      else if (method == "or")
        return builtin_binop(bytecode::BinaryOperator::Or);
    }
    throw std::logic_error("Invalid builtin");
  }

  void BuiltinGenerator::builtin_print()
  {
    // The method can generate a print method with any arity
    // It needs at least 2 arguments, for the receiver and the format string.
    assert(abi_.arguments >= 2);
    assert(abi_.returns == 1);

    size_t value_count = abi_.arguments - 2;
    uint8_t value_count_trunc = truncate<uint8_t>(value_count);
    gen_.opcode(Opcode::Print);
    gen_.reg(Register(1));
    gen_.u8(value_count_trunc);
    for (uint8_t i = 0; i < value_count_trunc; i++)
    {
      gen_.reg(Register(2 + i));
    }

    gen_.opcode(Opcode::Clear);
    gen_.reg(Register(1));
    for (uint8_t i = 0; i < value_count_trunc; i++)
    {
      gen_.opcode(Opcode::Clear);
      gen_.reg(Register(2 + i));
    }

    gen_.opcode(Opcode::Clear);
    gen_.reg(Register(0));
    gen_.opcode(Opcode::Return);
  }

  void BuiltinGenerator::builtin_create_sleeping_cown()
  {
    assert(abi_.arguments == 1);
    assert(abi_.returns == 1);

    gen_.opcode(Opcode::NewSleepingCown);
    gen_.reg(Register(0));
    gen_.opcode(Opcode::Return);
  }

  void BuiltinGenerator::builtin_trace_region()
  {
    assert(abi_.arguments == 2);
    assert(abi_.returns == 1);

    gen_.opcode(Opcode::TraceRegion);
    gen_.reg(Register(1));
    gen_.opcode(Opcode::Return);
  }

  void BuiltinGenerator::builtin_fulfill_sleeping_cown()
  {
    assert(abi_.arguments == 3);
    assert(abi_.returns == 1);

    gen_.opcode(Opcode::FulfillSleepingCown);
    gen_.reg(Register(1));
    gen_.reg(Register(2));
    gen_.opcode(Opcode::Clear);
    gen_.reg(Register(0));
    gen_.opcode(Opcode::Clear);
    gen_.reg(Register(1));
    gen_.opcode(Opcode::Clear);
    gen_.reg(Register(2));
    gen_.opcode(Opcode::Return);
  }

  void BuiltinGenerator::builtin_binop(bytecode::BinaryOperator op)
  {
    assert(abi_.arguments == 2);
    assert(abi_.returns == 1);

    gen_.opcode(Opcode::BinOp);
    gen_.reg(Register(0));
    gen_.u8(static_cast<uint8_t>(op));
    gen_.reg(Register(0));
    gen_.reg(Register(1));
    gen_.opcode(Opcode::Clear);
    gen_.reg(Register(1));
    gen_.opcode(Opcode::Return);
  }
}
