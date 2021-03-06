#include "VM.h"
#include "State.h"
#include "Table.h"
#include "Function.h"
#include "Exception.h"
#include <assert.h>
#include <math.h>

namespace
{
    std::string NumberToStr(luna::Value *num)
    {
        assert(num->type_ == luna::ValueT_Number);
        char temp[64];
        if (floor(num->num_) == num->num_)
            snprintf(temp, sizeof(temp), "%lld", static_cast<long long>(num->num_));
        else
            snprintf(temp, sizeof(temp), "%g", num->num_);
        return temp;
    }
} // namespace

namespace luna
{
#define GET_CONST_VALUE(i)      (proto->GetConstValue(Instruction::GetParamBx(i)))
#define GET_REGISTER_A(i)       (call->register_ + Instruction::GetParamA(i))
#define GET_REGISTER_B(i)       (call->register_ + Instruction::GetParamB(i))
#define GET_REGISTER_C(i)       (call->register_ + Instruction::GetParamC(i))
#define GET_UPVALUE_B(i)        (cl->GetUpvalue(Instruction::GetParamB(i)))
#define GET_REAL_VALUE(a)       (a->type_ == ValueT_Upvalue ? a->upvalue_->GetValue() : a)

#define GET_REGISTER_ABC(i)                                 \
    a = GET_REGISTER_A(i);                                  \
    b = GET_REGISTER_B(i);                                  \
    c = GET_REGISTER_C(i);

#define GET_CALLINFO_AND_PROTO()                            \
    assert(!state_->calls_.empty());                        \
    auto call = &state_->calls_.back();                     \
    assert(call->func_ && call->func_->closure_);           \
    auto proto = call->func_->closure_->GetPrototype()

    VM::VM(State *state) : state_(state)
    {
    }

    void VM::Execute()
    {
        assert(!state_->calls_.empty());

        while (!state_->calls_.empty())
            ExecuteFrame();
    }

    void VM::ExecuteFrame()
    {
        CallInfo *call = &state_->calls_.back();
        Closure *cl = call->func_ ? call->func_->closure_ : nullptr;
        Function *proto = cl ? cl->GetPrototype() : nullptr;
        Value *a = nullptr;
        Value *b = nullptr;
        Value *c = nullptr;

        while (call->instruction_ < call->end_)
        {
            state_->CheckRunGC();
            Instruction i = *call->instruction_++;

            switch (Instruction::GetOpCode(i)) {
                case OpType_LoadNil:
                    a = GET_REGISTER_A(i);
                    GET_REAL_VALUE(a)->SetNil();
                    break;
                case OpType_LoadBool:
                    a = GET_REGISTER_A(i);
                    GET_REAL_VALUE(a)->SetBool(Instruction::GetParamB(i) ? true : false);
                    break;
                case OpType_LoadInt:
                    a = GET_REGISTER_A(i);
                    assert(call->instruction_ < call->end_);
                    a->num_ = (*call->instruction_++).opcode_;
                    a->type_ = ValueT_Number;
                    break;
                case OpType_LoadConst:
                    a = GET_REGISTER_A(i);
                    b = GET_CONST_VALUE(i);
                    *GET_REAL_VALUE(a) = *b;
                    break;
                case OpType_Move:
                    a = GET_REGISTER_A(i);
                    b = GET_REGISTER_B(i);
                    *GET_REAL_VALUE(a) = *GET_REAL_VALUE(b);
                    break;
                case OpType_Call:
                    a = GET_REGISTER_A(i);
                    if (Call(a, i)) return ;
                    break;
                case OpType_GetUpvalue:
                    a = GET_REGISTER_A(i);
                    b = GET_UPVALUE_B(i)->GetValue();
                    *GET_REAL_VALUE(a) = *b;
                    break;
                case OpType_SetUpvalue:
                    a = GET_REGISTER_A(i);
                    b = GET_UPVALUE_B(i)->GetValue();
                    *b = *a;
                    break;
                case OpType_GetGlobal:
                    a = GET_REGISTER_A(i);
                    b = GET_CONST_VALUE(i);
                    *GET_REAL_VALUE(a) = state_->global_.table_->GetValue(*b);
                    break;
                case OpType_SetGlobal:
                    a = GET_REGISTER_A(i);
                    b = GET_CONST_VALUE(i);
                    state_->global_.table_->SetValue(*b, *a);
                    break;
                case OpType_Closure:
                    a = GET_REGISTER_A(i);
                    GenerateClosure(a, i);
                    break;
                case OpType_VarArg:
                    a = GET_REGISTER_A(i);
                    CopyVarArg(a, i);
                    break;
                case OpType_Ret:
                    a = GET_REGISTER_A(i);
                    return Return(a, i);
                case OpType_JmpFalse:
                    a = GET_REGISTER_A(i);
                    if (GET_REAL_VALUE(a)->IsFalse())
                        call->instruction_ += -1 + Instruction::GetParamsBx(i);
                    break;
                case OpType_JmpTrue:
                    a = GET_REGISTER_A(i);
                    if (!GET_REAL_VALUE(a)->IsFalse())
                        call->instruction_ += -1 + Instruction::GetParamsBx(i);
                    break;
                case OpType_JmpNil:
                    a = GET_REGISTER_A(i);
                    if (a->type_ == ValueT_Nil)
                        call->instruction_ += -1 + Instruction::GetParamsBx(i);
                    break;
                case OpType_Jmp:
                    call->instruction_ += -1 + Instruction::GetParamsBx(i);
                    break;
                case OpType_Neg:
                    a = GET_REGISTER_A(i);
                    CheckType(a, ValueT_Number, "neg");
                    a->num_ = -a->num_;
                    break;
                case OpType_Not:
                    a = GET_REGISTER_A(i);
                    a->SetBool(a->IsFalse() ? true : false);
                    break;
                case OpType_Len:
                    a = GET_REGISTER_A(i);
                    if (a->type_ == ValueT_Table)
                        a->num_ = a->table_->ArraySize();
                    else if (a->type_ == ValueT_String)
                        a->num_ = a->str_->GetLength();
                    else
                        ReportTypeError(a, "length of");
                    a->type_ = ValueT_Number;
                    break;
                case OpType_Add:
                    GET_REGISTER_ABC(i);
                    CheckArithType(b, c, "add");
                    a->num_ = b->num_ + c->num_;
                    a->type_ = ValueT_Number;
                    break;
                case OpType_Sub:
                    GET_REGISTER_ABC(i);
                    CheckArithType(b, c, "sub");
                    a->num_ = b->num_ - c->num_;
                    a->type_ = ValueT_Number;
                    break;
                case OpType_Mul:
                    GET_REGISTER_ABC(i);
                    CheckArithType(b, c, "multiply");
                    a->num_ = b->num_ * c->num_;
                    a->type_ = ValueT_Number;
                    break;
                case OpType_Div:
                    GET_REGISTER_ABC(i);
                    CheckArithType(b, c, "div");
                    a->num_ = b->num_ / c->num_;
                    a->type_ = ValueT_Number;
                    break;
                case OpType_Pow:
                    GET_REGISTER_ABC(i);
                    CheckArithType(b, c, "power");
                    a->num_ = pow(b->num_, c->num_);
                    a->type_ = ValueT_Number;
                    break;
                case OpType_Mod:
                    GET_REGISTER_ABC(i);
                    CheckArithType(b, c, "mod");
                    a->num_ = fmod(b->num_, c->num_);
                    a->type_ = ValueT_Number;
                    break;
                case OpType_Concat:
                    GET_REGISTER_ABC(i);
                    Concat(a, b, c);
                    break;
                case OpType_Less:
                    GET_REGISTER_ABC(i);
                    CheckInequalityType(b, c, "compare(<)");
                    if (b->type_ == ValueT_Number)
                        a->SetBool(b->num_ < c->num_);
                    else
                        a->SetBool(*b->str_ < *c->str_);
                    break;
                case OpType_Greater:
                    GET_REGISTER_ABC(i);
                    CheckInequalityType(b, c, "compare(>)");
                    if (b->type_ == ValueT_Number)
                        a->SetBool(b->num_ > c->num_);
                    else
                        a->SetBool(*b->str_ > *c->str_);
                    break;
                case OpType_Equal:
                    GET_REGISTER_ABC(i);
                    a->SetBool(*b == *c);
                    break;
                case OpType_UnEqual:
                    GET_REGISTER_ABC(i);
                    a->SetBool(*b != *c);
                    break;
                case OpType_LessEqual:
                    GET_REGISTER_ABC(i);
                    CheckInequalityType(b, c, "compare(<=)");
                    if (b->type_ == ValueT_Number)
                        a->SetBool(b->num_ <= c->num_);
                    else
                        a->SetBool(*b->str_ <= *c->str_);
                    break;
                case OpType_GreaterEqual:
                    GET_REGISTER_ABC(i);
                    CheckInequalityType(b, c, "compare(>=)");
                    if (b->type_ == ValueT_Number)
                        a->SetBool(b->num_ >= c->num_);
                    else
                        a->SetBool(*b->str_ >= *c->str_);
                    break;
                case OpType_NewTable:
                    a = GET_REGISTER_A(i);
                    a->table_ = state_->NewTable();
                    a->type_ = ValueT_Table;
                    break;
                case OpType_SetTable:
                    GET_REGISTER_ABC(i);
                    CheckTableType(a, b, "set", "to");
                    a->table_->SetValue(*b, *c);
                    break;
                case OpType_GetTable:
                    GET_REGISTER_ABC(i);
                    CheckTableType(a, b, "get", "from");
                    *c = a->table_->GetValue(*b);
                    break;
                case OpType_ForInit:
                    GET_REGISTER_ABC(i);
                    ForInit(a, b, c);
                    break;
                case OpType_ForStep:
                    GET_REGISTER_ABC(i);
                    i = *call->instruction_++;
                    if ((c->num_ > 0.0 && a->num_ > b->num_) ||
                        (c->num_ <= 0.0 && a->num_ < b->num_))
                        call->instruction_ += -1 + Instruction::GetParamsBx(i);
                    break;
                default:
                    break;
            }
        }

        // For bootstrap CallInfo, we use call->register_ as new top
        Value *new_top = call->func_ ? call->func_ : call->register_;
        // Reset top value
        state_->stack_.SetNewTop(new_top);
        // Set expect results
        if (call->expect_result != EXP_VALUE_COUNT_ANY)
            state_->stack_.SetNewTop(new_top + call->expect_result);

        // Pop current CallInfo, and return to last CallInfo
        state_->calls_.pop_back();
    }

    bool VM::Call(Value *a, Instruction i)
    {
        // Set stack top when arg_count is fixed
        int arg_count = Instruction::GetParamB(i) - 1;
        if (arg_count != EXP_VALUE_COUNT_ANY)
            state_->stack_.top_ = a + 1 + arg_count;

        int expect_result = Instruction::GetParamC(i) - 1;
        if (a->type_ == ValueT_Closure)
        {
            // We need enter next ExecuteFrame
            CallClosure(a, expect_result);
            return true;
        }
        else if (a->type_ == ValueT_CFunction)
        {
            CallCFunction(a, expect_result);
            return false;
        }
        else
        {
            ReportTypeError(a, "call");
            return true;
        }
    }

    void VM::CallClosure(Value *a, int expect_result)
    {
        CallInfo callee;
        Function *callee_proto = a->closure_->GetPrototype();

        callee.func_ = a;
        callee.instruction_ = callee_proto->GetOpCodes();
        callee.end_ = callee.instruction_ + callee_proto->OpCodeSize();
        callee.expect_result = expect_result;

        Value *arg = a + 1;
        int fixed_args = callee_proto->FixedArgCount();

        // Fixed arg start from base register
        if (callee_proto->HasVararg())
        {
            Value *top = state_->stack_.top_;
            callee.register_ = top;
            int count = top - arg;
            for (int i = 0; i < count && i < fixed_args; ++i)
                *top++ = *arg++;
        }
        else
        {
            callee.register_ = arg;
        }

        state_->stack_.SetNewTop(callee.register_ + fixed_args);
        state_->calls_.push_back(callee);
    }

    void VM::CallCFunction(Value *a, int expect_result)
    {
        // Push the c function CallInfo
        CallInfo callee;
        callee.register_ = a + 1;
        callee.func_ = a;
        callee.expect_result = expect_result;
        state_->calls_.push_back(callee);

        // Call c function
        CFunctionType cfunc = a->cfunc_;
        state_->ClearCFunctionError();
        int res_count = cfunc(state_);
        CheckCFuntionError();

        Value *src = nullptr;
        if (res_count > 0)
            src = state_->stack_.top_ - res_count;

        // Copy c function result to caller stack
        Value *dst = a;
        if (expect_result == EXP_VALUE_COUNT_ANY)
        {
            for (int i = 0; i < res_count; ++i)
                *dst++ = *src++;
        }
        else
        {
            int count = std::min(expect_result, res_count);
            for (int i = 0; i < count; ++i)
                *dst++ = *src++;
            // Set all remain expect results to nil
            count = expect_result - res_count;
            for (int i = 0; i < count; ++i, ++dst)
                dst->SetNil();
        }

        // Set registers which after dst to nil
        // and set new stack top pointer
        state_->stack_.SetNewTop(dst);

        // Pop the c function CallInfo
        state_->calls_.pop_back();
    }

    void VM::GenerateClosure(Value *a, Instruction i)
    {
        GET_CALLINFO_AND_PROTO();
        auto a_proto = proto->GetChildFunction(Instruction::GetParamBx(i));
        a->type_ = ValueT_Closure;
        a->closure_ = state_->NewClosure();
        a->closure_->SetPrototype(a_proto);

        // Prepare all upvalues
        auto new_closure = a->closure_;
        auto closure = call->func_->closure_;
        auto count = a_proto->GetUpvalueCount();
        for (std::size_t i = 0; i < count; ++i)
        {
            auto upvalue_info = a_proto->GetUpvalue(i);
            if (upvalue_info->parent_local_)
            {
                // Transform local variable to upvalue
                auto reg = call->register_ + upvalue_info->register_index_;
                if (reg->type_ != ValueT_Upvalue)
                {
                    auto upvalue = state_->NewUpvalue();
                    upvalue->SetValue(*reg);
                    reg->type_ = ValueT_Upvalue;
                    reg->upvalue_ = upvalue;
                    new_closure->AddUpvalue(upvalue);
                }
                else
                {
                    new_closure->AddUpvalue(reg->upvalue_);
                }
            }
            else
            {
                // Get upvalue from parent upvalue list
                auto upvalue = closure->GetUpvalue(upvalue_info->register_index_);
                new_closure->AddUpvalue(upvalue);
            }
        }
    }

    void VM::CopyVarArg(Value *a, Instruction i)
    {
        GET_CALLINFO_AND_PROTO();
        auto arg = call->func_ + 1;
        int total_args = call->register_ - arg;
        int vararg_count = total_args - proto->FixedArgCount();

        arg += proto->FixedArgCount();
        int expect_count = Instruction::GetParamsBx(i);
        if (expect_count == EXP_VALUE_COUNT_ANY)
        {
            for (int i = 0; i < vararg_count; ++i)
                *a++ = *arg++;
            state_->stack_.SetNewTop(a);
        }
        else
        {
            int i = 0;
            for (; i < vararg_count && i < expect_count; ++i)
                *a++ = *arg++;
            for (; i < expect_count; ++i, ++a)
                a->SetNil();
        }
    }

    void VM::Return(Value *a, Instruction i)
    {
        // Set stack top when return value count is fixed
        int ret_value_count = Instruction::GetParamsBx(i);
        if (ret_value_count != EXP_VALUE_COUNT_ANY)
            state_->stack_.top_ = a + ret_value_count;

        assert(!state_->calls_.empty());
        auto call = &state_->calls_.back();

        auto src = a;
        auto dst = call->func_;

        int expect_result = call->expect_result;
        int result_count = state_->stack_.top_ - src;
        if (expect_result == EXP_VALUE_COUNT_ANY)
        {
            for (int i = 0; i < result_count; ++i)
                *dst++ = *src++;
        }
        else
        {
            int i = 0;
            int count = std::min(expect_result, result_count);
            for (; i < count; ++i)
                *dst++ = *src++;
            // No enough results for expect results, set remain as nil
            for (; i < expect_result; ++i, ++dst)
                dst->SetNil();
        }

        // Set new top and pop current CallInfo
        state_->stack_.SetNewTop(dst);
        state_->calls_.pop_back();
    }

    void VM::Concat(Value *dst, Value *op1, Value *op2)
    {
        if (op1->type_ == ValueT_String && op2->type_ == ValueT_String)
        {
            dst->str_ = state_->GetString(op1->str_->GetStdString() +
                                          op2->str_->GetCStr());
        }
        else if (op1->type_ == ValueT_String && op2->type_ == ValueT_Number)
        {
            dst->str_ = state_->GetString(op1->str_->GetCStr() +
                                          NumberToStr(op2));
        }
        else if (op1->type_ == ValueT_Number && op2->type_ == ValueT_String)
        {
            dst->str_ = state_->GetString(NumberToStr(op1) +
                                          op2->str_->GetCStr());
        }
        else
        {
            auto line = GetCurrentInstructionLine();
            throw RuntimeException(op1, op2, "concat", line);
        }

        dst->type_ = ValueT_String;
    }

    void VM::ForInit(Value *var, Value *limit, Value *step)
    {
        if (var->type_ != ValueT_Number)
        {
            throw RuntimeException(var, "'for' init", "number",
                                   GetCurrentInstructionLine());
        }

        if (limit->type_ != ValueT_Number)
        {
            throw RuntimeException(limit, "'for' limit", "number",
                                   GetCurrentInstructionLine());
        }

        if (step->type_ != ValueT_Number)
        {
            throw RuntimeException(step, "'for' step", "number",
                                   GetCurrentInstructionLine());
        }
    }

    std::pair<const char *, const char *> VM::GetOperandNameAndScope(const Value *a) const
    {
        GET_CALLINFO_AND_PROTO();

        auto reg = a - call->register_;
        auto instruction = call->instruction_ - 1;
        auto base = proto->GetOpCodes();
        auto pc = instruction - base;
        const char *unknown_name = "?";
        const char *scope_global = "global";
        const char *scope_local = "local";
        const char *scope_upvalue = "upvalue";
        const char *scope_table = "table member";
        const char *scope_null = "";

        // Search last instruction which dst register is reg,
        // and get the name base on the instruction
        while (instruction > base)
        {
            --instruction;
            switch (Instruction::GetOpCode(*instruction)) {
                case OpType_GetGlobal:
                    if (reg == Instruction::GetParamA(*instruction))
                    {
                        auto index = Instruction::GetParamBx(*instruction);
                        auto key = proto->GetConstValue(index);
                        if (key->type_ == ValueT_String)
                            return { key->str_->GetCStr(), scope_global };
                        else
                            return { unknown_name, scope_null };
                    }
                    break;
                case OpType_Move:
                    if (reg == Instruction::GetParamA(*instruction))
                    {
                        auto src = Instruction::GetParamB(*instruction);
                        auto name = proto->SearchLocalVar(src, pc);
                        if (name)
                            return { name->GetCStr(), scope_local };
                        else
                            return { unknown_name, scope_null };
                    }
                    break;
                case OpType_GetUpvalue:
                    if (reg == Instruction::GetParamA(*instruction))
                    {
                        auto index = Instruction::GetParamB(*instruction);
                        auto upvalue_info = proto->GetUpvalue(index);
                        return { upvalue_info->name_->GetCStr(), scope_upvalue };
                    }
                    break;
                case OpType_GetTable:
                    if (reg == Instruction::GetParamC(*instruction))
                    {
                        auto key = Instruction::GetParamB(*instruction);
                        auto key_reg = call->register_ + key;
                        if (key_reg->type_ == ValueT_String)
                            return { key_reg->str_->GetCStr(), scope_table };
                        else
                            return { unknown_name, scope_table };
                    }
                    break;
            }
        }

        return { unknown_name, scope_null };
    }

    int VM::GetCurrentInstructionLine() const
    {
        GET_CALLINFO_AND_PROTO();
        auto index = call->instruction_ - 1 - proto->GetOpCodes();
        return proto->GetInstructionLine(index);
    }

    void VM::CheckCFuntionError() const
    {
        auto error = state_->GetCFunctionErrorData();
        if (error->type_ == CFuntionErrorType_NoError)
            return ;

        char buffer[128] = { 0 };
        if (error->type_ == CFuntionErrorType_ArgCount)
        {
            snprintf(buffer, sizeof(buffer), "expect %d arguments",
                     error->expect_arg_count_);
        }
        else if (error->type_ == CFuntionErrorType_ArgType)
        {
            auto &call = state_->calls_.back();
            auto arg = call.register_ + error->arg_index_;
            snprintf(buffer, sizeof(buffer),
                     "argument #%d is a %s value, expect a %s value",
                     error->arg_index_ + 1, arg->TypeName(),
                     Value::TypeName(error->expect_type_));
        }

        // Pop the c function CallInfo, then GetCurrentInstructionLine
        // can calculate line number of the call
        state_->calls_.pop_back();
        int line = GetCurrentInstructionLine();
        throw RuntimeException(buffer, line);
    }

    void VM::CheckType(const Value *v, ValueT type, const char *op) const
    {
        if (v->type_ != type)
            ReportTypeError(v, op);
    }

    void VM::CheckArithType(const Value *v1, const Value *v2, const char *op) const
    {
        if (v1->type_ != ValueT_Number || v2->type_ != ValueT_Number)
        {
            auto line = GetCurrentInstructionLine();
            throw RuntimeException(v1, v2, op, line);
        }
    }

    void VM::CheckInequalityType(const Value *v1, const Value *v2,
                                 const char *op) const
    {
        if (v1->type_ != v2->type_ ||
            (v1->type_ != ValueT_Number && v1->type_ != ValueT_String))
        {
            auto line = GetCurrentInstructionLine();
            throw RuntimeException(v1, v2, op, line);
        }
    }

    void VM::CheckTableType(const Value *t, const Value *k,
                            const char *op, const char *desc) const
    {
        if (t->type_ != ValueT_Table)
        {
            auto ns = GetOperandNameAndScope(t);
            auto line = GetCurrentInstructionLine();
            auto key_name = k->type_ == ValueT_String ? k->str_->GetCStr() : "?";
            std::string op_desc = std::string(op) + " table key '" + key_name + "' " + desc;
            throw RuntimeException(t, ns.first, ns.second, op_desc.c_str(), line);
        }
    }

    void VM::ReportTypeError(const Value *v, const char *op) const
    {
        auto ns = GetOperandNameAndScope(v);
        auto line = GetCurrentInstructionLine();
        throw RuntimeException(v, ns.first, ns.second, op, line);
    }
} // namespace luna
