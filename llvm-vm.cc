#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/DerivedTypes.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/JIT.h>
#include <llvm/LLVMContext.h>
#include <llvm/Module.h>
#include <llvm/PassManager.h>
#include <llvm/Transforms/Scalar.h>

#if HAVE_LLVM_SUPPORT_TARGETSELECT_H
#  include <llvm/Support/TargetSelect.h>
#elif HAVE_LLVM_TARGET_TARGETSELECT_H
#  include <llvm/Target/TargetSelect.h>
#endif

#if HAVE_LLVM_SUPPORT_IRBUILDER_H
#  include <llvm/Support/IRBuilder.h>
#elif HAVE_LLVM_IRBUILDER_H
#  include <llvm/IRBuilder.h>
#endif

#if (LLVM_VERSION_MAJOR > 3) || (LLVM_VERSION_MINOR >= 2)
#  define LLVM_TYPE llvm::Type
#else
#  define LLVM_TYPE const llvm::Type
#endif

#include "ca-table.h"
#include "ca-llvm.h"
#include "query.h"

namespace ca_llvm
{
  bool initialize_done;

  llvm::Module *module;
  llvm::ExecutionEngine *engine;

  llvm::Function *f_CA_output_char;
  llvm::Function *f_CA_output_string;
  llvm::Function *f_CA_output_json_string;
  llvm::Function *f_CA_output_uint64;
  llvm::Function *f_CA_output_time_float4;

  llvm::Function *f_ca_compare_like;
  llvm::Function *f_strcmp;

  LLVM_TYPE *t_void;

  LLVM_TYPE *t_int1;
  LLVM_TYPE *t_int8;
  LLVM_TYPE *t_int8_pointer;
  LLVM_TYPE *t_int16;
  LLVM_TYPE *t_int16_pointer;
  LLVM_TYPE *t_int32;
  LLVM_TYPE *t_int32_pointer;
  LLVM_TYPE *t_int64;
  LLVM_TYPE *t_int64_pointer;

  LLVM_TYPE *t_pointer;
  LLVM_TYPE *t_pointer_pointer;
  LLVM_TYPE *t_size;

  LLVM_TYPE *t_float;
  LLVM_TYPE *t_double;

  /* t_int32  header
   * t_int64  data0
   * t_int64  data1 */
  LLVM_TYPE *t_expression_value;
  LLVM_TYPE *t_expression_value_pointer;

  /* t_pointer
   * t_size */
  LLVM_TYPE *t_iovec;
  LLVM_TYPE *t_iovec_pointer;

  void
  initialize_types (const llvm::DataLayout *data_layout)
  {
    std::vector<LLVM_TYPE *> types;

    t_void =  llvm::Type::getVoidTy (llvm::getGlobalContext ());

    t_int1 =  llvm::Type::getInt1Ty (llvm::getGlobalContext ());
    t_int8 =  llvm::Type::getInt8Ty (llvm::getGlobalContext ());
    t_int16 = llvm::Type::getInt16Ty (llvm::getGlobalContext ());
    t_int32 = llvm::Type::getInt32Ty (llvm::getGlobalContext ());
    t_int64 = llvm::Type::getInt64Ty (llvm::getGlobalContext ());

    t_int8_pointer = llvm::PointerType::get (t_int8, 0);
    t_int16_pointer = llvm::PointerType::get (t_int16, 0);
    t_int32_pointer = llvm::PointerType::get (t_int32, 0);
    t_int64_pointer = llvm::PointerType::get (t_int64, 0);

    if (sizeof (void *) == sizeof (int64_t))
      t_pointer = t_int64;
    else if (sizeof (void *) == sizeof (int32_t))
      t_pointer = t_int32;
    else
      assert (!"unhandled void * size");

    t_pointer_pointer = llvm::PointerType::get (t_pointer, 0);

    if (sizeof (size_t) == sizeof (int64_t))
      t_size = t_int64;
    else if (sizeof (size_t) == sizeof (int32_t))
      t_size = t_int32;
    else
      assert (!"unhandled void * size");

    t_float = llvm::Type::getFloatTy (llvm::getGlobalContext ());
    t_double = llvm::Type::getDoubleTy (llvm::getGlobalContext ());

    types.clear ();
    types.push_back (t_int32);
    types.push_back (t_int64);
    types.push_back (t_int64);
    t_expression_value
      = llvm::StructType::get (llvm::getGlobalContext (), types);
    t_expression_value_pointer
      = llvm::PointerType::get (t_expression_value, 0);

    types.clear ();
    types.push_back (t_pointer);
    types.push_back (t_size);
    t_iovec = llvm::StructType::get (llvm::getGlobalContext (), types);
    t_iovec_pointer = llvm::PointerType::get (t_iovec, 0);

    assert (sizeof (struct expression_value) == data_layout->getTypeAllocSize (t_expression_value));
    assert (sizeof (struct iovec) == data_layout->getTypeAllocSize (t_iovec));
  }

  bool
  initialize ()
  {
    std::vector<LLVM_TYPE *> argument_types;

    llvm::InitializeNativeTarget();

    module = new llvm::Module ("ca-table JIT module", llvm::getGlobalContext ());

    llvm::EngineBuilder engine_builder(module);
    std::string error_string;

    engine_builder.setErrorStr (&error_string);

    if (!(engine = engine_builder.create ()))
      {
        ca_set_error ("Failed to create execution engine: %s", error_string.c_str ());

        return false;
      }

    initialize_types (engine->getDataLayout ());

    argument_types.clear ();
    argument_types.push_back (t_int32);

    f_CA_output_char
      = llvm::Function::Create (llvm::FunctionType::get (t_void, argument_types, false),
                                llvm::Function::ExternalLinkage,
                                "CA_output_char", module);

    argument_types.clear ();
    argument_types.push_back (t_pointer);

    f_CA_output_string
      = llvm::Function::Create (llvm::FunctionType::get (t_void, argument_types, false),
                                llvm::Function::ExternalLinkage,
                                "CA_output_string", module);

    f_CA_output_json_string
      = llvm::Function::Create (llvm::FunctionType::get (t_void, argument_types, false),
                                llvm::Function::ExternalLinkage,
                                "CA_output_json_string", module);

    argument_types.clear ();
    argument_types.push_back (t_int64);

    f_CA_output_uint64
      = llvm::Function::Create (llvm::FunctionType::get (t_void, argument_types, false),
                                llvm::Function::ExternalLinkage,
                                "CA_output_uint64", module);

    argument_types.clear ();
    argument_types.push_back (t_iovec_pointer);

    f_CA_output_time_float4
      = llvm::Function::Create (llvm::FunctionType::get (t_void, argument_types, false),
                                llvm::Function::ExternalLinkage,
                                "CA_output_time_float4", module);

    argument_types.clear ();
    argument_types.push_back (t_pointer);
    argument_types.push_back (t_pointer);

    f_ca_compare_like
      = llvm::Function::Create (llvm::FunctionType::get (t_int1, argument_types, false),
                                llvm::Function::ExternalLinkage,
                                "CA_compare_like", module);

    f_strcmp
      = llvm::Function::Create (llvm::FunctionType::get (t_int32, argument_types, false),
                                llvm::Function::ExternalLinkage,
                                "strcmp", module);

    initialize_done = true;

    return true;
  }
} /* namespace ca_llvm */

static int
CA_generate_output (llvm::IRBuilder<> *builder,
                    llvm::Function *function,
                    llvm::Value *value, enum ca_type type,
                    enum ca_param_value output_format)
{
  using namespace ca_llvm;

  switch (type)
    {
    case CA_BOOLEAN:

      builder->CreateCall (f_CA_output_string,
                           builder->CreateSelect (value,
                                                  llvm::ConstantInt::get (t_pointer, (ptrdiff_t) "true"),
                                                  llvm::ConstantInt::get (t_pointer, (ptrdiff_t) "false")));

      break;

    case CA_TEXT:

      if (output_format == CA_PARAM_VALUE_JSON)
        builder->CreateCall (f_CA_output_json_string, value);
      else
        builder->CreateCall (f_CA_output_string, value);

      break;

    case CA_INT64:

      builder->CreateCall (f_CA_output_uint64, value);

      break;

    case CA_TIME_FLOAT4:

      builder->CreateCall (f_CA_output_time_float4, value);

      break;

    default:

      ca_set_error ("Don't know how to print data of type %s", ca_type_to_string (type));

      return -1;
    }

  return 0;
}

ca_expression_function
CA_expression_compile (struct ca_query_parse_context *context,
                       const char *name,
                       struct expression *expr,
                       const struct ca_field *fields,
                       size_t field_count,
                       int flags)
{
  using namespace ca_llvm;

  enum ca_type return_type;
  llvm::Value *return_value = NULL;

  unsigned int item_index = 0, item_count = 0;
  struct expression *expr_i;
  llvm::Value *delimiter = NULL, *key_separator = NULL;

  std::vector<LLVM_TYPE *> argument_types;

  if (!initialize_done && !initialize ())
    return NULL;

  auto builder = new llvm::IRBuilder<> (llvm::getGlobalContext ());

  argument_types.push_back (t_pointer);
  argument_types.push_back (t_iovec_pointer);
  auto function_type = llvm::FunctionType::get (t_int32, argument_types, false);

  auto function = llvm::Function::Create (function_type, llvm::Function::InternalLinkage, name, module);

  auto argument = function->arg_begin ();
  llvm::Value *arena = argument++;
  llvm::Value *field_values = argument++;
  assert (argument == function->arg_end ());

  auto basic_block = llvm::BasicBlock::Create (llvm::getGlobalContext (), "entry", function);

  builder->SetInsertPoint (basic_block);

  if (0 != (flags & CA_EXPRESSION_PRINT))
    {
      if (CA_output_format == CA_PARAM_VALUE_JSON)
        {
          builder->CreateCall (f_CA_output_string,
                               llvm::ConstantInt::get (t_pointer, (uintptr_t) "{\""));

          delimiter = llvm::ConstantInt::get (t_pointer, (ptrdiff_t) ",\"");
          key_separator = llvm::ConstantInt::get (t_pointer, (ptrdiff_t) "\":");
        }
      else
        delimiter = llvm::ConstantInt::get (t_int32, '\t');
    }

  for (expr_i = expr; expr_i; expr_i = expr_i->next)
    {
      if (expr_i->type == EXPR_ASTERISK)
        item_count += field_count;
      else
        ++item_count;
    }

  for (; expr; expr = expr->next)
    {
      struct select_item *si;

      si = (struct select_item *) expr;

      if (expr->type == EXPR_ASTERISK)
        {
          size_t i = 0;

          for (i = 0; i < field_count; ++i)
            {
              struct expression tmp_expr;

              tmp_expr.type = EXPR_FIELD;
              tmp_expr.value.type = (enum ca_type) fields[i].type;
              tmp_expr.value.d.field_index = i;

              if (!(return_value = subexpression_compile (builder, module,
                                                          &tmp_expr, fields,
                                                          arena,
                                                          field_values,
                                                          &return_type)))
                {
                  return NULL;
                }

              if (0 != (flags & CA_EXPRESSION_PRINT))
                {
                  if (item_index)
                    {
                      if (CA_output_format == CA_PARAM_VALUE_JSON)
                        builder->CreateCall (f_CA_output_string, delimiter);
                      else
                        builder->CreateCall (f_CA_output_char, delimiter);
                    }

                  if (CA_output_format == CA_PARAM_VALUE_JSON)
                    {
                      builder->CreateCall (f_CA_output_string,
                                           llvm::ConstantInt::get (t_pointer, (uintptr_t) fields[i].name));
                      builder->CreateCall (f_CA_output_string, key_separator);
                    }

                  if (-1 == CA_generate_output (builder, function,
                                                return_value, return_type,
                                                CA_output_format))
                    {
                      return NULL;
                    }
                }

              ++item_index;
            }
        }
      else
        {
          if (!(return_value = subexpression_compile (builder, module,
                                                      expr, fields,
                                                      arena,
                                                      field_values,
                                                      &return_type)))
            {
              return NULL;
            }

          if (0 != (flags & CA_EXPRESSION_PRINT))
            {
              if (item_index)
                {
                  if (CA_output_format == CA_PARAM_VALUE_JSON)
                    builder->CreateCall (f_CA_output_string, delimiter);
                  else
                    builder->CreateCall (f_CA_output_char, delimiter);
                }

              if (CA_output_format == CA_PARAM_VALUE_JSON)
                {
                  builder->CreateCall (f_CA_output_string,
                                       llvm::ConstantInt::get (t_pointer, (uintptr_t) si->alias));
                  builder->CreateCall (f_CA_output_string, key_separator);
                }

              if (-1 == CA_generate_output (builder, function,
                                            return_value, return_type,
                                            CA_output_format))
                {
                  return NULL;
                }
            }

          ++item_index;
        }
    }

  if (0 != (flags & CA_EXPRESSION_PRINT))
    {
      if (CA_output_format == CA_PARAM_VALUE_JSON)
        builder->CreateCall (f_CA_output_char, llvm::ConstantInt::get (t_int32, '}'));
      else
        builder->CreateCall (f_CA_output_char, llvm::ConstantInt::get (t_int32, '\n'));
    }

  if (0 != (flags & CA_EXPRESSION_RETURN_BOOL))
    {
      if (return_type != CA_BOOLEAN)
        {
          ca_set_error ("Expression is not of type BOOLEAN");

          return NULL;
        }

      builder->CreateRet (builder->CreateIntCast (return_value, t_int32, false));
    }
  else
    builder->CreateRet (llvm::ConstantInt::get (t_int32, 0));

  llvm::verifyFunction (*function);

  auto fpm = new llvm::FunctionPassManager (module);

  fpm->add (new llvm::DataLayout (*engine->getDataLayout ())); /* Freed by fpm */
  fpm->add (llvm::createBasicAliasAnalysisPass ());
  fpm->add (llvm::createInstructionCombiningPass ());
  fpm->add (llvm::createReassociatePass ());
  fpm->add (llvm::createGVNPass ());
  fpm->add (llvm::createCFGSimplificationPass ());
  fpm->doInitialization ();
  fpm->run (*function);

  delete fpm;

  return (ca_expression_function) engine->getPointerToFunction (function);
}
