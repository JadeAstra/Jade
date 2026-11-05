// The code the VM runs (M6.7), made from a function's stack code once that is final.
//
// The compiler writes stack code, which the verifier follows knowing the stack's depth before
// each instruction (bytecode.cpp). From it, this makes the code the VM runs, with the source line
// of each of its bytes and the ranges of the function's rights in its offsets
// (docs/M6.7-plan.md, "The register machine").
//
// A local or a constant that the stack code pushes is not moved: the translator notes where the
// value is, and the instruction that takes it reads it from there, by an operand naming the slot
// or the constant. Such an instruction, an R instruction, carries the stack instruction it stands
// for after its own operands; where its quick way does not do (a value of another kind, a field
// not found where it was before), the VM puts its operands where the stack would have had them
// and runs that instruction as it would have run. So the VM's stack is as the stack code had it
// wherever anything but an R instruction runs: before any other instruction, and before a jump or
// a place jumped to, every value noted is put in its slot first.
//
// Values noted only ever stand above every value in a slot: before an R instruction writes its
// result, any noted below that result's slot is put in its slot. So every slot below the VM's top
// holds what was written there while it was below the top, and the collector, which marks what is
// below each stack's top, never meets a slot holding what it has collected.
#include <cstdlib>
#include <stdexcept>

#include "bytecode.h"

namespace astra {

namespace {

int readU16(const std::vector<uint8_t>& code, size_t at) { return code[at] | (code[at + 1] << 8); }
int32_t readI32(const std::vector<uint8_t>& code, size_t at) {
    return static_cast<int32_t>(static_cast<uint32_t>(code[at]) | (static_cast<uint32_t>(code[at + 1]) << 8) |
                                (static_cast<uint32_t>(code[at + 2]) << 16) | (static_cast<uint32_t>(code[at + 3]) << 24));
}

// What the translator knows of a value on the stack.
struct Entry {
    enum Kind : uint8_t {
        InSlot,    // in its own slot, where the stack has it
        Local,     // the value of slot `index`, not yet copied up
        Constant,  // constant `index`, not yet pushed
    };
    Kind kind = InSlot;
    uint16_t index = 0;
};

class Translator {
public:
    Translator(FunctionProto& f, const std::vector<int>& depths) : f_(f), code_(f.code), depths_(depths), n_(f.code.size()) {}

    void translate() {
        findLeaders();
        findCaptured();
        newAt_.assign(n_ + 1, 0);
        bool known = false;  // whether the state is what the code before leaves, falling through
        size_t at = 0;
        while (at < n_) {
            const size_t size = instructionSize(code_, at);
            const int depth = depths_[at];
            line_ = f_.lines[at];
            if (depth < 0) {
                // Never reached: copied as it is.
                if (known) canonical();
                known = false;
                newAt_[at] = static_cast<uint32_t>(out_.size());
                copy(at, size);
                at += size;
                continue;
            }
            if (leader_[at] || !known) {
                // Jumped to: the stack as the stack code has it, from wherever the code comes.
                if (known) canonical();
                reset(depth);
                known = true;
            }
            if (static_cast<int>(stack_.size()) != depth) fail("the depth does not match");
            newAt_[at] = static_cast<uint32_t>(out_.size());
            known = one(at, size, depth);
            at += size;
        }
        if (known) canonical();
        newAt_[n_] = static_cast<uint32_t>(out_.size());
        // An offset inside an instruction is where that instruction now starts.
        for (size_t i = 1; i < n_; i++)
            if (!isStart(i)) newAt_[i] = newAt_[i - 1];
        for (const auto& [place, target] : fixups_) {
            if (target > n_ || (target < n_ && !isStart(target))) fail("a jump into an instruction");
            putI32(place, static_cast<int32_t>(newAt_[target]));
        }
        f_.run = std::move(out_);
        f_.runLines = std::move(outLines_);
        f_.runRights.clear();
        for (const FunctionProto::Rights& r : f_.rights) f_.runRights.emplace_back(mapped(r.start), mapped(r.end));
    }

private:
    FunctionProto& f_;
    const std::vector<uint8_t>& code_;
    const std::vector<int>& depths_;
    const size_t n_;
    std::vector<bool> leader_, start_, captured_;
    std::vector<uint8_t> out_;
    std::vector<int> outLines_;
    std::vector<uint32_t> newAt_;
    std::vector<std::pair<size_t, size_t>> fixups_;  // where in the run code a target goes, and the old target
    std::vector<Entry> stack_;
    // Where the VM's stack top is: the stack's depth, below it where values noted stand at the
    // top, above it where a value was taken off by writing it to a local.
    int top_ = 0;
    int line_ = 0;

    [[noreturn]] static void fail(const std::string& why) { throw std::runtime_error(why); }

    bool isStart(size_t at) const { return at < n_ && start_[at]; }
    uint32_t mapped(size_t at) const { return at >= n_ ? static_cast<uint32_t>(newAt_[n_]) : newAt_[at]; }

    // ---- What is known before translating ----

    void findLeaders() {
        leader_.assign(n_ + 1, false);
        start_.assign(n_ + 1, false);
        for (size_t at = 0; at < n_;) {
            start_[at] = true;
            const Op op = static_cast<Op>(code_[at]);
            const int t = targetOperand(op);
            if (t != 0) {
                const size_t target = static_cast<size_t>(readI32(code_, at + static_cast<size_t>(t)));
                if (target <= n_) leader_[target] = true;
            }
            at += instructionSize(code_, at);
        }
    }

    // The slots a function made here keeps by reference: never read through a note, as the
    // function may change them while the note stands.
    void findCaptured() {
        captured_.assign(static_cast<size_t>(std::max(f_.maxStack, 1)) + 1, false);
        for (size_t at = 0; at < n_; at += instructionSize(code_, at)) {
            const Op op = static_cast<Op>(code_[at]);
            if (op == Op::Closure || op == Op::MakeForms || op == Op::MethodForms || op == Op::StaticForms) {
                const size_t index = static_cast<size_t>(readU16(code_, at + 1));
                if (index < f_.protos.size()) markCaptured(*f_.protos[index]);
            } else if (op == Op::Capture) {
                if (code_[at + 3] == 0) capture(readU16(code_, at + 1));
            }
        }
    }
    void markCaptured(const FunctionProto& nested) {
        for (const UpvalueDesc& u : nested.upvalues)
            if (u.isLocal) capture(u.index);
        for (const FunctionProto* inner : nested.protos)
            if (inner != nullptr) markCaptured(*inner);  // a set of forms made of nested functions
    }
    void capture(size_t slot) {
        if (slot >= captured_.size()) captured_.resize(slot + 1, false);
        captured_[slot] = true;
    }
    bool isCaptured(size_t slot) const { return slot < captured_.size() && captured_[slot]; }

    // ---- Writing ----

    void byte(int b) {
        out_.push_back(static_cast<uint8_t>(b));
        outLines_.push_back(line_);
    }
    void u16(int v) {
        if (v < 0 || v > 0xFFFF) fail("an operand out of range");
        byte(v & 0xFF);
        byte((v >> 8) & 0xFF);
    }
    void op(Op o) { byte(static_cast<int>(o)); }
    void putI32(size_t place, int32_t v) {
        const uint32_t u = static_cast<uint32_t>(v);
        for (int i = 0; i < 4; i++) out_[place + static_cast<size_t>(i)] = static_cast<uint8_t>(u >> (8 * i));
    }
    // An instruction of the stack code, as it is; a place it jumps to is moved where it now is.
    void copy(size_t at, size_t size) {
        const size_t start = out_.size();
        for (size_t i = 0; i < size; i++) byte(code_[at + i]);
        const int t = targetOperand(static_cast<Op>(code_[at]));
        if (t != 0) fixups_.emplace_back(start + static_cast<size_t>(t), static_cast<size_t>(readI32(code_, at + static_cast<size_t>(t))));
    }

    // An operand naming where a noted value is: its slot, or the constant.
    int rk(const Entry& e, int position) const {
        switch (e.kind) {
            case Entry::InSlot: return position;
            case Entry::Local: return e.index;
            case Entry::Constant: return kRkConstant | e.index;
        }
        return position;
    }

    // ---- The stack as the translator sees it ----

    void reset(int depth) {
        stack_.assign(static_cast<size_t>(depth), Entry{});
        top_ = depth;
    }
    bool noted(const Entry& e) const { return e.kind != Entry::InSlot; }

    // Puts the value noted at `position` in its slot.
    void place(int position) {
        Entry& e = stack_[static_cast<size_t>(position)];
        if (!noted(e)) return;
        if (position == top_) {
            // The VM's top is there: pushed as the stack code would have.
            op(e.kind == Entry::Local ? Op::GetLocal : Op::Const);
            u16(e.index);
            top_++;
        } else if (position < top_) {
            op(Op::Move);
            u16(position);
            u16(rk(e, position));
        } else {
            fail("a value noted above a slot not written");
        }
        e = Entry{};
    }
    // The stack as the stack code has it: every value in its slot, the VM's top where its is.
    void canonical() {
        const int depth = static_cast<int>(stack_.size());
        for (int p = 0; p < depth; p++) place(p);
        settle(depth);
    }
    void settle(int depth) {
        if (top_ != depth) {
            op(Op::SetTop);
            u16(depth);
            top_ = depth;
        }
    }
    // Every value noted below `depth` in its slot, the VM's top left as it is: before an R
    // instruction that jumps, whose quick way sets the top itself.
    void placeBelow(int depth) {
        for (int p = 0; p < depth; p++) place(p);
    }
    // Before slot `slot` changes: what is noted as its value is copied up first.
    void beforeWrite(int slot) {
        for (size_t p = 0; p < stack_.size(); p++)
            if (stack_[p].kind == Entry::Local && stack_[p].index == slot) place(static_cast<int>(p));
    }
    // What GetLocal pushes, noted: the slot's value, or what is noted for the slot itself.
    Entry localEntry(int slot) const {
        const Entry& there = stack_[static_cast<size_t>(slot)];
        if (noted(there)) return there;
        return Entry{Entry::Local, static_cast<uint16_t>(slot)};
    }
    static Entry constantEntry(int index) { return Entry{Entry::Constant, static_cast<uint16_t>(index)}; }

    // An R instruction: its operands, the values at the stack's top `count` deep (with `extra`,
    // values noted that the stack code had not pushed, after them), and the stack instruction it
    // stands for, `plain` with the original's operands from `at`.
    void rInstruction(Op r, const std::vector<Entry>& sources, int s, Op plain, size_t at, size_t size) {
        op(r);
        u16(s);
        for (size_t i = 0; i < sources.size(); i++) u16(rk(sources[i], s + static_cast<int>(i)));
        const size_t start = out_.size();
        op(plain);
        for (size_t i = 1; i < size; i++) byte(code_[at + i]);
        const int t = targetOperand(plain);
        if (t != 0) fixups_.emplace_back(start + static_cast<size_t>(t), static_cast<size_t>(readI32(code_, at + static_cast<size_t>(t))));
    }
    // The `count` values at the stack's top, taken off.
    std::vector<Entry> take(int count) {
        std::vector<Entry> taken(stack_.end() - count, stack_.end());
        stack_.resize(stack_.size() - static_cast<size_t>(count));
        return taken;
    }
    void pushResult(int s) {
        stack_.push_back(Entry{});
        top_ = s + 1;
    }

    static bool fitsRk(const Entry& e) { return e.kind != Entry::Constant || e.index < kRkConstant; }

    // ---- One instruction ----

    // Translates the instruction at `at`; false where the code after it is not reached from it.
    bool one(size_t at, size_t size, int depth) {
        const Op o = static_cast<Op>(code_[at]);
        const int a = size >= 3 ? readU16(code_, at + 1) : 0;
        const int b = size >= 5 ? readU16(code_, at + 3) : 0;
        switch (o) {
            case Op::GetLocal:
                if (isCaptured(static_cast<size_t>(a))) break;
                stack_.push_back(localEntry(a));
                return true;
            case Op::Const:
                if (a >= kRkConstant) break;
                stack_.push_back(constantEntry(a));
                return true;
            case Op::Pop:
                if (!noted(stack_.back())) break;
                stack_.pop_back();
                return true;
            case Op::Dup:
                if (!noted(stack_.back())) break;
                stack_.push_back(stack_.back());
                return true;
            case Op::SetLocal:
            case Op::StoreLocal: {
                beforeWrite(a);
                const int position = depth - 1;
                op(Op::Move);
                u16(a);
                u16(rk(stack_.back(), position));
                stack_[static_cast<size_t>(a)] = Entry{};
                if (o == Op::StoreLocal) {
                    stack_.pop_back();
                } else if (!noted(stack_.back()) && position != a) {
                    // The value stays where it is; the local holds it too.
                }
                return true;
            }
            case Op::CopyLocal: {
                beforeWrite(b);
                op(Op::Move);
                u16(b);
                u16(rk(localEntry(a), a));
                stack_[static_cast<size_t>(b)] = Entry{};
                return true;
            }
            case Op::Add: return binary(Op::RAdd, o, at, size, depth);
            case Op::Sub: return binary(Op::RSub, o, at, size, depth);
            case Op::Mul: return binary(Op::RMul, o, at, size, depth);
            case Op::Div: return binary(Op::RDiv, o, at, size, depth);
            case Op::Mod: return binary(Op::RMod, o, at, size, depth);
            case Op::Lt: return binary(Op::RLt, o, at, size, depth);
            case Op::Le: return binary(Op::RLe, o, at, size, depth);
            case Op::Gt: return binary(Op::RGt, o, at, size, depth);
            case Op::Ge: return binary(Op::RGe, o, at, size, depth);
            case Op::Eq: return binary(Op::REq, o, at, size, depth);
            case Op::AddLocal: return joined(Op::RAdd, Op::Add, localEntry(a));
            case Op::SubLocal: return joined(Op::RSub, Op::Sub, localEntry(a));
            case Op::MulLocal: return joined(Op::RMul, Op::Mul, localEntry(a));
            case Op::DivLocal: return joined(Op::RDiv, Op::Div, localEntry(a));
            case Op::ModLocal: return joined(Op::RMod, Op::Mod, localEntry(a));
            case Op::LtLocal: return joined(Op::RLt, Op::Lt, localEntry(a));
            case Op::LeLocal: return joined(Op::RLe, Op::Le, localEntry(a));
            case Op::GtLocal: return joined(Op::RGt, Op::Gt, localEntry(a));
            case Op::GeLocal: return joined(Op::RGe, Op::Ge, localEntry(a));
            case Op::EqLocal: return joined(Op::REq, Op::Eq, localEntry(a));
            case Op::AddConst:
            case Op::SubConst:
            case Op::MulConst:
            case Op::DivConst:
            case Op::ModConst:
            case Op::LtConst:
            case Op::LeConst:
            case Op::GtConst:
            case Op::GeConst:
            case Op::EqConst:
                if (a >= kRkConstant) break;
                return joined(rFor(o), plainFor(o), constantEntry(a));
            case Op::AddLocals: return locals(Op::RAdd, Op::Add, a, b);
            case Op::SubLocals: return locals(Op::RSub, Op::Sub, a, b);
            case Op::MulLocals: return locals(Op::RMul, Op::Mul, a, b);
            case Op::DivLocals: return locals(Op::RDiv, Op::Div, a, b);
            case Op::LtLocals: return locals(Op::RLt, Op::Lt, a, b);
            case Op::LeLocals: return locals(Op::RLe, Op::Le, a, b);
            case Op::GtLocals: return locals(Op::RGt, Op::Gt, a, b);
            case Op::GeLocals: return locals(Op::RGe, Op::Ge, a, b);
            case Op::EqLocals: return locals(Op::REq, Op::Eq, a, b);
            case Op::Neg: return unary(Op::RNeg, o, at, size, depth);
            case Op::Not: return unary(Op::RNot, o, at, size, depth);
            case Op::GetProp:
            case Op::GetOwn:
            case Op::GetPropRef:
            case Op::GetOwnRef:
                if (!noted(stack_.back())) break;  // already in its slot: the stack instruction as it is
                return unary(Op::RGetProp, o, at, size, depth);
            case Op::SetProp:
            case Op::SetOwn:
                if (!noted(stack_[stack_.size() - 1]) && !noted(stack_[stack_.size() - 2])) break;
                return binary(Op::RSetProp, o, at, size, depth);
            case Op::GetIndex:
                if (!noted(stack_[stack_.size() - 1]) && !noted(stack_[stack_.size() - 2])) break;
                return binary(Op::RGetIndex, o, at, size, depth);
            case Op::JumpIfFalse: {
                const int s = depth - 1;
                if (!fitsRk(stack_.back())) break;
                placeBelow(s);
                const std::vector<Entry> sources = take(1);
                rInstruction(Op::RJumpIfFalse, sources, s, o, at, size);
                top_ = s;
                return true;
            }
            case Op::Return: {
                const int s = depth - 1;
                if (!fitsRk(stack_.back())) break;
                const std::vector<Entry> sources = take(1);
                rInstruction(Op::RReturn, sources, s, o, at, size);
                return false;
            }
            default: break;
        }
        // Any other: the stack as the stack code has it, and the instruction as it is.
        canonical();
        copy(at, size);
        const size_t next = at + size;
        if (terminal(o) || next >= n_ || depths_[next] < 0) return false;
        reset(depths_[next]);
        return true;
    }

    static bool terminal(Op o) {
        switch (o) {
            case Op::Jump:
            case Op::Loop:
            case Op::Return:
            case Op::ReturnInit:
            case Op::Throw:
            case Op::TailLoop:
            case Op::MissingArg: return true;
            default: return false;
        }
    }

    static Op rFor(Op joinedConst) {
        switch (joinedConst) {
            case Op::AddConst: return Op::RAdd;
            case Op::SubConst: return Op::RSub;
            case Op::MulConst: return Op::RMul;
            case Op::DivConst: return Op::RDiv;
            case Op::ModConst: return Op::RMod;
            case Op::LtConst: return Op::RLt;
            case Op::LeConst: return Op::RLe;
            case Op::GtConst: return Op::RGt;
            case Op::GeConst: return Op::RGe;
            default: return Op::REq;
        }
    }
    static Op plainFor(Op joinedConst) {
        switch (joinedConst) {
            case Op::AddConst: return Op::Add;
            case Op::SubConst: return Op::Sub;
            case Op::MulConst: return Op::Mul;
            case Op::DivConst: return Op::Div;
            case Op::ModConst: return Op::Mod;
            case Op::LtConst: return Op::Lt;
            case Op::LeConst: return Op::Le;
            case Op::GtConst: return Op::Gt;
            case Op::GeConst: return Op::Ge;
            default: return Op::Eq;
        }
    }

    // `a op b` of the two values at the stack's top, its result in the slot of the first.
    bool binary(Op r, Op plain, size_t at, size_t size, int depth) {
        const int s = depth - 2;
        if (!fitsRk(stack_[stack_.size() - 1]) || !fitsRk(stack_[stack_.size() - 2])) return fallBack(plain, at, size);
        placeBelow(s);
        rInstruction(r, take(2), s, plain, at, size);
        pushResult(s);
        return true;
    }
    bool unary(Op r, Op plain, size_t at, size_t size, int depth) {
        const int s = depth - 1;
        if (!fitsRk(stack_.back())) return fallBack(plain, at, size);
        placeBelow(s);
        rInstruction(r, take(1), s, plain, at, size);
        pushResult(s);
        return true;
    }
    // `top op x` where the stack code joined x, a local or a constant, to the instruction.
    bool joined(Op r, Op plain, Entry second) {
        const int s = static_cast<int>(stack_.size()) - 1;
        placeBelow(s);
        std::vector<Entry> sources = take(1);
        sources.push_back(second);
        rInstruction(r, sources, s, plain, 0, 1);
        pushResult(s);
        return true;
    }
    // `x op y` of two locals, pushed: its result where the stack code would have pushed it.
    bool locals(Op r, Op plain, int x, int y) {
        const int s = static_cast<int>(stack_.size());
        const Entry first = localEntry(x), second = localEntry(y);
        placeBelow(s);
        rInstruction(r, {first, second}, s, plain, 0, 1);
        pushResult(s);
        return true;
    }
    bool fallBack(Op plain, size_t at, size_t size) {
        (void)plain;
        canonical();
        copy(at, size);
        const size_t next = at + size;
        if (next >= n_ || depths_[next] < 0) return false;
        reset(depths_[next]);
        return true;
    }
};

// The stack code as it is, which the VM runs as well.
void asItIs(FunctionProto& f) {
    f.run = f.code;
    f.runLines = f.lines;
    f.runRights.clear();
    for (const FunctionProto::Rights& r : f.rights) f.runRights.emplace_back(r.start, r.end);
}

}  // namespace

void translate(FunctionProto& f, const std::vector<int>& depths) {
    // The register code is made only with ASTRA_REGISTER_CODE set, until it runs faster than the
    // stack code it is made from (docs/M6.7-plan.md); without it, the VM runs the stack code.
    static const bool registers = std::getenv("ASTRA_REGISTER_CODE") != nullptr;
    if (!registers || f.chooser || f.code.empty() || depths.size() != f.code.size()) return asItIs(f);
    try {
        Translator(f, depths).translate();
    } catch (const std::exception& e) {
        // Code the translator does not expect keeps its stack code, which runs as before; with
        // ASTRA_TRANSLATE_STRICT set, a test of the translator, it is an error.
        if (std::getenv("ASTRA_TRANSLATE_STRICT") != nullptr) throw std::runtime_error(std::string("cannot translate ") + f.name + ": " + e.what());
        asItIs(f);
    }
}

}  // namespace astra
