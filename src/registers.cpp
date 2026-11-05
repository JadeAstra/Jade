// The code the VM runs (M6.7), made from a function's stack code once that is final.
//
// The compiler writes stack code, which the verifier follows knowing the stack's depth before
// each instruction (bytecode.cpp). From it, this makes the code the VM runs, with the source line
// of each of its bytes and the ranges of the function's rights in its offsets
// (docs/M6.7-plan.md, "The register machine").
//
// A local or a constant that the stack code pushes is not moved: the translator notes where the
// value is, and the instruction that takes it reads it from there, by an operand naming the slot
// or the constant. Such an instruction, an R instruction, does what the stack code would have done
// where its operands are numbers, or a field is where it was found before; anywhere else it goes
// to its slow way: it puts its operands where the stack would have had them and runs the stack code
// as it would have run, put after the function's own code, which then comes back. An R instruction
// may write its result straight into the local the stack code stores it in, and a comparison may
// jump where the JumpIfFalse after it would, the store or the jump then being part of the stack
// code its slow way runs. So the VM's stack is as the stack code had it wherever anything but an
// R instruction runs: before any other instruction, and before a jump or a place jumped to, every
// value noted is put in its slot first.
//
// Values noted only ever stand above every value in a slot: before an R instruction writes its
// result, any noted below that result's slot is put in its slot. So every slot below the VM's top
// holds what was written there while it was below the top, and the collector, which marks what is
// below each stack's top, never meets a slot holding what it has collected.
//
// Slots and constants are named by a byte: a function needing more than 255 slots keeps its stack
// code, and a constant past the 255th is pushed as the stack code had it.
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

// An operation of R instructions, and its instructions for each kind of operand: slot and slot,
// slot and constant, constant and slot (Op::Count_ where there is none).
struct Family {
    Op ss, sk, ks;
};

const Family* familyOf(Op plain) {
    static const Family add{Op::RAddSS, Op::RAddSK, Op::RAddKS};
    static const Family sub{Op::RSubSS, Op::RSubSK, Op::Count_};
    static const Family mul{Op::RMulSS, Op::RMulSK, Op::RMulKS};
    static const Family div{Op::RDivSS, Op::RDivSK, Op::Count_};
    static const Family lt{Op::RLtSS, Op::RLtSK, Op::Count_};
    static const Family le{Op::RLeSS, Op::RLeSK, Op::Count_};
    static const Family gt{Op::RGtSS, Op::RGtSK, Op::Count_};
    static const Family ge{Op::RGeSS, Op::RGeSK, Op::Count_};
    static const Family eq{Op::REqSS, Op::REqSK, Op::Count_};
    switch (plain) {
        case Op::Add: return &add;
        case Op::Sub: return &sub;
        case Op::Mul: return &mul;
        case Op::Div: return &div;
        case Op::Lt: return &lt;
        case Op::Le: return &le;
        case Op::Gt: return &gt;
        case Op::Ge: return &ge;
        case Op::Eq: return &eq;
        default: return nullptr;
    }
}

// A comparison's form that jumps where the JumpIfFalse after it would; Count_ for any other.
Op jumpFormOf(Op r) {
    switch (r) {
        case Op::RLtSS: return Op::RJumpLtSS;
        case Op::RLtSK: return Op::RJumpLtSK;
        case Op::RLeSS: return Op::RJumpLeSS;
        case Op::RLeSK: return Op::RJumpLeSK;
        case Op::RGtSS: return Op::RJumpGtSS;
        case Op::RGtSK: return Op::RJumpGtSK;
        case Op::RGeSS: return Op::RJumpGeSS;
        case Op::RGeSK: return Op::RJumpGeSK;
        case Op::REqSS: return Op::RJumpEqSS;
        case Op::REqSK: return Op::RJumpEqSK;
        default: return Op::Count_;
    }
}

// An R instruction whose result goes to slot A, which a StoreLocal after it may send elsewhere.
bool storesResult(Op r) {
    return (r >= Op::RAddSS && r <= Op::REqSK) || r == Op::RGetProp;
}

// An R instruction's slow way, written after the function's own code: the stack code it stands
// for, which runs with its operands where the stack would have had them, from slot `s` (the R
// instruction puts them there), and a jump back to the instruction after it.
struct Slow {
    size_t instruction = 0;  // where the R instruction starts
    size_t size = 0;         // and its length
    size_t place = 0;        // where in it the slow way's start goes
    int s = 0;
    std::vector<uint8_t> code;                       // the stack code
    std::vector<std::pair<size_t, size_t>> targets;  // where in it a place to jump to goes, and the old target
    int line = 0;
    size_t old = 0;  // the stack instruction's offset in the stack code, for the rights there
};

class Translator {
public:
    Translator(FunctionProto& f, const std::vector<int>& depths) : f_(f), code_(f.code), depths_(depths), n_(f.code.size()) {}

    void translate() {
        if (f_.maxStack > 255) fail("more than 255 slots");
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
                // Jumped to: the stack as the stack code has it, from wherever the code comes, and
                // nothing joined to what comes before, which the jumps here do not run.
                if (known) canonical();
                reset(depth);
                known = true;
                lastR_ = false;
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
        // The rights of the code each range of the stack code covers, and of the slow ways of
        // the R instructions made from it.
        f_.runRights.clear();
        for (size_t i = 0; i < f_.rights.size(); i++)
            f_.runRights.push_back({mapped(f_.rights[i].start), mapped(f_.rights[i].end), static_cast<uint32_t>(i)});
        for (const Slow& slow : slows_) writeSlow(slow);
        for (const auto& [place, target] : fixups_) {
            if (target > n_ || (target < n_ && !isStart(target))) fail("a jump into an instruction");
            putI32(place, static_cast<int32_t>(newAt_[target]));
        }
        f_.run = std::move(out_);
        f_.runLines = std::move(outLines_);
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
    std::vector<Slow> slows_;
    std::vector<Entry> stack_;
    // Where the VM's stack top is: the stack's depth, below it where values noted stand at the
    // top, above it where a value was taken off by writing it to a local.
    int top_ = 0;
    int line_ = 0;
    // The R instruction just written with nothing after it yet, its slow way the last of slows_:
    // its result may still be sent straight to a local, or its comparison joined to the jump
    // after it.
    bool lastR_ = false;

    [[noreturn]] static void fail(const std::string& why) { throw std::runtime_error(why); }

    bool isStart(size_t at) const { return at < n_ && start_[at]; }
    uint32_t mapped(size_t at) const { return at >= n_ ? static_cast<uint32_t>(newAt_[n_]) : newAt_[at]; }

    // ---- What is known before translating ----

    void findLeaders() {
        leader_.assign(n_ + 1, false);
        start_.assign(n_ + 1, false);
        for (size_t at = 0; at < n_;) {
            start_[at] = true;
            const int t = targetOperand(static_cast<Op>(code_[at]));
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
                if (code_[at + 3] == 0) capture(static_cast<size_t>(readU16(code_, at + 1)));
            }
        }
    }
    void markCaptured(const FunctionProto& nested) {
        for (const UpvalueDesc& u : nested.upvalues)
            if (u.isLocal) capture(u.index);
        for (const FunctionProto* inner : nested.protos)
            if (inner != nullptr) markCaptured(*inner);
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
    void u8(int v) {
        if (v < 0 || v > 255) fail("an operand out of range");
        byte(v);
    }
    void u16(int v) {
        if (v < 0 || v > 0xFFFF) fail("an operand out of range");
        byte(v & 0xFF);
        byte((v >> 8) & 0xFF);
    }
    void i32(int32_t v) {
        const uint32_t u = static_cast<uint32_t>(v);
        for (int i = 0; i < 4; i++) byte(static_cast<int>((u >> (8 * i)) & 0xFF));
    }
    void op(Op o) {
        lastR_ = false;
        byte(static_cast<int>(o));
    }
    void putI32(size_t place, int32_t v) {
        const uint32_t u = static_cast<uint32_t>(v);
        for (int i = 0; i < 4; i++) out_[place + static_cast<size_t>(i)] = static_cast<uint8_t>(u >> (8 * i));
    }
    // An instruction of the stack code, as it is; a place it jumps to is moved where it now is.
    void copy(size_t at, size_t size) {
        lastR_ = false;
        const size_t start = out_.size();
        for (size_t i = 0; i < size; i++) byte(code_[at + i]);
        const int t = targetOperand(static_cast<Op>(code_[at]));
        if (t != 0) fixups_.emplace_back(start + static_cast<size_t>(t), static_cast<size_t>(readI32(code_, at + static_cast<size_t>(t))));
    }
    // The same into a slow way's stack code, as instruction `plain` with the operands of the one at `at`.
    void copyInto(Slow& slow, Op plain, size_t at, size_t size) {
        const size_t start = slow.code.size();
        slow.code.push_back(static_cast<uint8_t>(plain));
        for (size_t i = 1; i < size; i++) slow.code.push_back(code_[at + i]);
        const int t = targetOperand(plain);
        if (t != 0) slow.targets.emplace_back(start + static_cast<size_t>(t), static_cast<size_t>(readI32(code_, at + static_cast<size_t>(t))));
    }

    // ---- The stack as the translator sees it ----

    void reset(int depth) {
        stack_.assign(static_cast<size_t>(depth), Entry{});
        top_ = depth;
    }
    static bool noted(const Entry& e) { return e.kind != Entry::InSlot; }

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
            move(position, e);
        } else {
            fail("a value noted above a slot not written");
        }
        e = Entry{};
    }
    void move(int to, const Entry& from) {
        if (from.kind == Entry::InSlot) fail("a move from no slot named");
        if (from.kind == Entry::Constant && from.index > 255) fail("a constant out of range");
        op(from.kind == Entry::Constant ? Op::MoveK : Op::MoveS);
        u8(to);
        u8(from.index);
    }
    // The stack as the stack code has it: every value in its slot, the VM's top where its is.
    void canonical() {
        const int depth = static_cast<int>(stack_.size());
        for (int p = 0; p < depth; p++) place(p);
        if (top_ != depth) {
            op(Op::SetTop);
            u8(depth);
            top_ = depth;
        }
    }
    // Every value noted below `depth` in its slot, the VM's top left as it is.
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
    static bool isConstant(const Entry& e) { return e.kind == Entry::Constant; }
    // An operand byte: the slot holding the value, or the constant.
    static int operand(const Entry& e, int position) { return e.kind == Entry::InSlot ? position : e.index; }

    // The `count` values at the stack's top, taken off.
    std::vector<Entry> take(int count) {
        std::vector<Entry> taken(stack_.end() - count, stack_.end());
        stack_.resize(stack_.size() - static_cast<size_t>(count));
        return taken;
    }

    // ---- R instructions ----

    // Starts the slow way of the R instruction about to be written, for the stack instruction at
    // `at`, its operands from slot s on.
    Slow& newSlow(int s, size_t at) {
        Slow slow;
        slow.instruction = out_.size();
        slow.s = s;
        slow.line = line_;
        slow.old = at;
        slows_.push_back(std::move(slow));
        return slows_.back();
    }
    // The end of an R instruction: where its slow way's start goes, and the result in slot s.
    void endR(Slow& slow, int s) {
        slow.place = out_.size();
        i32(0);
        slow.size = out_.size() - slow.instruction;
        lastR_ = true;
        stack_.push_back(Entry{});
        top_ = s + 1;
    }

    // `x op y`, its result in slot s: false where no R instruction does it.
    bool binaryAt(Op plain, Entry x, Entry y, int s, size_t at, size_t size) {
        const Family* fam = familyOf(plain);
        if (fam == nullptr) return false;
        if (isConstant(x) && (fam->ks == Op::Count_ || isConstant(y) || x.index > 255)) {
            // A constant first where none takes one: pushed where the stack would have it.
            stack_.push_back(x);
            place(s);
            stack_.pop_back();
            x = Entry{};
        }
        if (isConstant(y) && y.index > 255) {
            stack_.push_back(x);
            stack_.push_back(y);
            place(s);
            place(s + 1);
            stack_.resize(stack_.size() - 2);
            x = y = Entry{};
        }
        const Op r = isConstant(y) ? fam->sk : isConstant(x) ? fam->ks : fam->ss;
        const int bOperand = operand(x, s), cOperand = operand(y, s + 1);
        Slow& slow = newSlow(s, at);
        copyInto(slow, plain, at, size);
        // u8 A, u8 B, u8 C, u8 T (the top after it), u8 S (where the stack would have B), i32 slow.
        op(r);
        u8(s);
        u8(bOperand);
        u8(cOperand);
        u8(s + 1);
        u8(s);
        endR(slow, s);
        return true;
    }

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
                if (a > 255) break;
                stack_.push_back(constantEntry(a));
                return true;
            case Op::Pop:
                if (!noted(stack_.back())) break;
                stack_.pop_back();
                return true;
            case Op::Dup:
                // A copy of the top: what is noted for it, or the slot it is in, which nothing
                // writes while the copy above it stands.
                if (noted(stack_.back())) {
                    stack_.push_back(stack_.back());
                    return true;
                }
                if (depth - 1 > 255) break;
                stack_.push_back(Entry{Entry::Local, static_cast<uint16_t>(depth - 1)});
                return true;
            case Op::StoreLocal:
                if (storeIntoLast(a, depth, at, size)) return true;
                [[fallthrough]];
            case Op::SetLocal: {
                if (a > 255) break;
                beforeWrite(a);
                const Entry value = stack_.back();
                move(a, noted(value) ? value : Entry{Entry::Local, static_cast<uint16_t>(depth - 1)});
                stack_[static_cast<size_t>(a)] = Entry{};
                if (o == Op::StoreLocal) stack_.pop_back();
                return true;
            }
            case Op::CopyLocal: {
                if (b > 255) break;
                beforeWrite(b);
                const Entry from = localEntry(a);
                move(b, from.kind == Entry::InSlot ? Entry{Entry::Local, static_cast<uint16_t>(a)} : from);
                stack_[static_cast<size_t>(b)] = Entry{};
                return true;
            }
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
            case Op::Eq: {
                const int s = depth - 2;
                placeBelow(s);
                const std::vector<Entry> sources = take(2);
                if (binaryAt(o, sources[0], sources[1], s, at, size)) return true;
                stack_.push_back(sources[0]);
                stack_.push_back(sources[1]);
                break;
            }
            case Op::AddLocal:
            case Op::SubLocal:
            case Op::MulLocal:
            case Op::DivLocal:
            case Op::LtLocal:
            case Op::LeLocal:
            case Op::GtLocal:
            case Op::GeLocal:
            case Op::EqLocal: {
                const int s = depth - 1;
                placeBelow(s);
                const Entry x = take(1)[0];
                if (binaryAt(plainOf(o), x, localEntry(a), s, at, 1)) return true;
                stack_.push_back(x);
                break;
            }
            case Op::AddConst:
            case Op::SubConst:
            case Op::MulConst:
            case Op::DivConst:
            case Op::LtConst:
            case Op::LeConst:
            case Op::GtConst:
            case Op::GeConst:
            case Op::EqConst: {
                const int s = depth - 1;
                placeBelow(s);
                const Entry x = take(1)[0];
                if (binaryAt(plainOf(o), x, constantEntry(a), s, at, 1)) return true;
                stack_.push_back(x);
                break;
            }
            case Op::AddLocals:
            case Op::SubLocals:
            case Op::MulLocals:
            case Op::DivLocals:
            case Op::LtLocals:
            case Op::LeLocals:
            case Op::GtLocals:
            case Op::GeLocals:
            case Op::EqLocals: {
                const int s = depth;
                const Entry x = localEntry(a), y = localEntry(b);
                placeBelow(s);
                if (binaryAt(plainOf(o), x, y, s, at, 1)) return true;
                break;
            }
            case Op::GetProp:
            case Op::GetOwn: {
                if (!noted(stack_.back()) || isConstant(stack_.back())) break;  // the stack code as it is
                const int s = depth - 1;
                placeBelow(s);
                const Entry x = take(1)[0];
                const int bOperand = operand(x, s);
                Slow& slow = newSlow(s, at);
                copyInto(slow, o, at, size);
                // u8 A, u8 B, u8 T, u8 S, u16 the cache GetProp names, i32 slow.
                op(Op::RGetProp);
                u8(s);
                u8(bOperand);
                u8(s + 1);
                u8(s);
                u16(b);
                endR(slow, s);
                return true;
            }
            case Op::JumpIfFalse:
                if (jumpIntoLast(depth, at, size)) return true;
                break;
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

    // `StoreLocal x` of the result of the R instruction just written: written straight into x.
    // Nothing noted reads x, as every value noted below an R instruction is put in its slot first.
    bool storeIntoLast(int x, int depth, size_t at, size_t size) {
        if (!lastR_ || x > 255 || noted(stack_.back())) return false;
        Slow& slow = slows_.back();
        const size_t start = slow.instruction;
        const Op r = static_cast<Op>(out_[start]);
        const int s = slow.s;
        if (!storesResult(r) || out_[start + 1] != s || s != depth - 1) return false;
        out_[start + 1] = static_cast<uint8_t>(x);
        // The top after it: at s, the result having gone to x.
        out_[r == Op::RGetProp ? start + 3 : start + 4] = static_cast<uint8_t>(s);
        copyInto(slow, Op::StoreLocal, at, size);
        stack_[static_cast<size_t>(x)] = Entry{};
        stack_.pop_back();
        top_ = s;
        return true;
    }
    // `JumpIfFalse t` after a comparison just written: one instruction that compares and jumps.
    bool jumpIntoLast(int depth, size_t at, size_t size) {
        if (!lastR_) return false;
        Slow& slow = slows_.back();
        const size_t start = slow.instruction;
        const Op jumping = jumpFormOf(static_cast<Op>(out_[start]));
        if (jumping == Op::Count_) return false;
        const int s = slow.s;
        if (out_[start + 1] != s || s != depth - 1) return false;
        const int bOperand = out_[start + 2], cOperand = out_[start + 3];
        out_.resize(start);
        outLines_.resize(start);
        // u8 B, u8 C, u8 T, i32 where it jumps, i32 its slow way.
        op(jumping);
        u8(bOperand);
        u8(cOperand);
        u8(s);
        fixups_.emplace_back(out_.size(), static_cast<size_t>(readI32(code_, at + 1)));
        i32(0);
        slow.place = out_.size();
        i32(0);
        slow.size = out_.size() - start;
        copyInto(slow, Op::JumpIfFalse, at, size);
        lastR_ = false;
        stack_.pop_back();
        top_ = s;
        return true;
    }

    // An R instruction's slow way, after the function's own code.
    void writeSlow(const Slow& slow) {
        const int saved = line_;
        line_ = slow.line;
        const size_t start = out_.size();
        putI32(slow.place, static_cast<int32_t>(start));
        const size_t codeAt = out_.size();
        for (uint8_t b : slow.code) byte(b);
        for (const auto& [place, target] : slow.targets) fixups_.emplace_back(codeAt + place, target);
        op(Op::Jump);
        i32(static_cast<int32_t>(slow.instruction + slow.size));
        // Where the stack code came from had rights, so has the slow way.
        for (size_t i = 0; i < f_.rights.size(); i++)
            if (f_.rights[i].start <= slow.old && slow.old < f_.rights[i].end)
                f_.runRights.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(out_.size()), static_cast<uint32_t>(i)});
        line_ = saved;
    }

    static Op plainOf(Op joined) {
        switch (joined) {
            case Op::AddLocal: case Op::AddConst: case Op::AddLocals: return Op::Add;
            case Op::SubLocal: case Op::SubConst: case Op::SubLocals: return Op::Sub;
            case Op::MulLocal: case Op::MulConst: case Op::MulLocals: return Op::Mul;
            case Op::DivLocal: case Op::DivConst: case Op::DivLocals: return Op::Div;
            case Op::LtLocal: case Op::LtConst: case Op::LtLocals: return Op::Lt;
            case Op::LeLocal: case Op::LeConst: case Op::LeLocals: return Op::Le;
            case Op::GtLocal: case Op::GtConst: case Op::GtLocals: return Op::Gt;
            case Op::GeLocal: case Op::GeConst: case Op::GeLocals: return Op::Ge;
            default: return Op::Eq;
        }
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
};

// The stack code as it is, which the VM runs as well.
void asItIs(FunctionProto& f) {
    f.run = f.code;
    f.runLines = f.lines;
    f.runRights.clear();
    for (size_t i = 0; i < f.rights.size(); i++) f.runRights.push_back({f.rights[i].start, f.rights[i].end, static_cast<uint32_t>(i)});
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
        // ASTRA_TRANSLATE_STRICT set, a test of the translator, it is an error. A function needing
        // more than 255 slots keeps its stack code either way.
        if (std::getenv("ASTRA_TRANSLATE_STRICT") != nullptr && f.maxStack <= 255)
            throw std::runtime_error(std::string("cannot translate ") + f.name + ": " + e.what());
        asItIs(f);
    }
}

}  // namespace astra
