// The code the VM runs (M6.7), made from a function's stack code once that is final.
//
// The compiler writes stack code, which the verifier follows knowing the stack's depth before
// each instruction (bytecode.cpp). From it, this makes the code the VM runs, with the source line
// of each of its bytes and the ranges of the function's rights in its offsets
// (docs/plans-and-gaps.html, the M6.7 plan's "The register machine").
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
//
// The stack code's joined instructions (AddLocal, MulLocals and the rest) are never in run code,
// whose instructions have their numbers: they are made R instructions, or, in a function that
// keeps its stack code, the instructions they join (GetLocal and Add).
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "bytecode.h"

namespace jade {

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
    static const Family sub{Op::RSubSS, Op::RSubSK, Op::RSubKS};
    static const Family mul{Op::RMulSS, Op::RMulSK, Op::RMulKS};
    static const Family div{Op::RDivSS, Op::RDivSK, Op::RDivKS};
    static const Family mod{Op::RModSS, Op::RModSK, Op::Count_};
    static const Family lt{Op::RLtSS, Op::RLtSK, Op::Count_};
    static const Family le{Op::RLeSS, Op::RLeSK, Op::Count_};
    static const Family gt{Op::RGtSS, Op::RGtSK, Op::Count_};
    static const Family ge{Op::RGeSS, Op::RGeSK, Op::Count_};
    static const Family eq{Op::REqSS, Op::REqSK, Op::Count_};
    static const Family shl{Op::Count_, Op::RShlSK, Op::Count_};
    static const Family shr{Op::Count_, Op::RShrSK, Op::Count_};
    static const Family bitAnd{Op::RBitAndSS, Op::RBitAndSK, Op::Count_};
    static const Family bitOr{Op::RBitOrSS, Op::RBitOrSK, Op::Count_};
    // On operands the checker knows (M6.7 step 5).
    static const Family typedAddI{Op::RAddISS, Op::RAddISK, Op::RAddIKS};
    static const Family typedSubI{Op::RSubISS, Op::RSubISK, Op::RSubIKS};
    static const Family typedMulI{Op::RMulISS, Op::RMulISK, Op::RMulIKS};
    static const Family typedAddF{Op::RAddFSS, Op::RAddFSK, Op::RAddFKS};
    static const Family typedSubF{Op::RSubFSS, Op::RSubFSK, Op::RSubFKS};
    static const Family typedMulF{Op::RMulFSS, Op::RMulFSK, Op::RMulFKS};
    static const Family typedDivF{Op::RDivFSS, Op::RDivFSK, Op::RDivFKS};
    static const Family typedLtI{Op::RLtISS, Op::RLtISK, Op::Count_};
    static const Family typedLeI{Op::RLeISS, Op::RLeISK, Op::Count_};
    static const Family typedGtI{Op::RGtISS, Op::RGtISK, Op::Count_};
    static const Family typedGeI{Op::RGeISS, Op::RGeISK, Op::Count_};
    static const Family typedEqI{Op::REqISS, Op::REqISK, Op::Count_};
    static const Family typedLtF{Op::RLtFSS, Op::RLtFSK, Op::Count_};
    static const Family typedLeF{Op::RLeFSS, Op::RLeFSK, Op::Count_};
    static const Family typedGtF{Op::RGtFSS, Op::RGtFSK, Op::Count_};
    static const Family typedGeF{Op::RGeFSS, Op::RGeFSK, Op::Count_};
    static const Family typedEqF{Op::REqFSS, Op::REqFSK, Op::Count_};
    switch (plain) {
        case Op::Add: return &add;
        case Op::Sub: return &sub;
        case Op::Mul: return &mul;
        case Op::Div: return &div;
        case Op::Mod: return &mod;
        case Op::Lt: return &lt;
        case Op::Le: return &le;
        case Op::Gt: return &gt;
        case Op::Ge: return &ge;
        case Op::Eq: return &eq;
        case Op::Shl: return &shl;
        case Op::Shr: return &shr;
        case Op::BitAnd: return &bitAnd;
        case Op::BitOr: return &bitOr;
        case Op::AddI: return &typedAddI;
        case Op::SubI: return &typedSubI;
        case Op::MulI: return &typedMulI;
        case Op::AddF: return &typedAddF;
        case Op::SubF: return &typedSubF;
        case Op::MulF: return &typedMulF;
        case Op::DivF: return &typedDivF;
        case Op::LtI: return &typedLtI;
        case Op::LeI: return &typedLeI;
        case Op::GtI: return &typedGtI;
        case Op::GeI: return &typedGeI;
        case Op::EqI: return &typedEqI;
        case Op::LtF: return &typedLtF;
        case Op::LeF: return &typedLeF;
        case Op::GtF: return &typedGtF;
        case Op::GeF: return &typedGeF;
        case Op::EqF: return &typedEqF;
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
        default:
            // Those on operands the checker knows, in the same order as their jumps.
            if (r >= Op::RLtISS && r <= Op::REqFSK)
                return static_cast<Op>(static_cast<int>(Op::RJumpLtISS) + (static_cast<int>(r) - static_cast<int>(Op::RLtISS)));
            return Op::Count_;
    }
}

// An R instruction whose result goes to slot A, its first operand, which a StoreLocal after it may
// send elsewhere; and where in it T is, the top after it.
bool storesResult(Op r) {
    switch (r) {
        case Op::RGetProp:
        case Op::RGetPath:
        case Op::RModSS:
        case Op::RModSK:
        case Op::RSubKS:
        case Op::RDivKS:
        case Op::RGetGlobal:
        case Op::RGetIndexSS:
        case Op::RGetIndexSK:
        case Op::RGetIndexPropSS:
        case Op::RGetIndexPropSK:
        case Op::RNeg:
        case Op::RNot:
        case Op::RShlSK:
        case Op::RShrSK:
        case Op::RBitAndSS:
        case Op::RBitAndSK:
        case Op::RBitOrSS:
        case Op::RBitOrSK:
        case Op::RInvoke:
        case Op::RCall: return true;
        default: return (r >= Op::RAddSS && r <= Op::REqSK) || (r >= Op::RAddISS && r <= Op::REqFSK);
    }
}
int topOperand(Op r) {
    switch (r) {
        case Op::RGetProp:
        case Op::RGetPath:
        case Op::RNeg:
        case Op::RNot:
        case Op::RInvoke:
        case Op::RCall: return 3;  // u8 A, u8 B, u8 T
        case Op::RGetGlobal: return 2;  // u8 A, u8 T
        default: return 4;  // u8 A, u8 B, u8 C, u8 T
    }
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
    // Where in the R instruction the place after its stack code's first instruction goes (an
    // RInvoke's or RCall's resume), or none.
    size_t resumePlace = SIZE_MAX;
};

// How much the translator does.
enum class Mode {
    Full,     // values noted, and R instructions wherever they do
    Joined,   // R instructions only for the stack code's joined instructions, nothing noted
    Plain,    // the stack code as it is, its joined instructions as the instructions they join
};

class Translator {
public:
    Translator(FunctionProto& f, const std::vector<int>& depths, Mode mode)
        : f_(f), code_(f.code), depths_(depths), n_(f.code.size()), notes_(mode == Mode::Full), rs_(mode != Mode::Plain), joinedOnly_(mode == Mode::Joined) {}

    void translate() {
        f_.runTops.clear();
        if (rs_ && f_.maxStack > 255) fail("more than 255 slots");
        findLeaders();
        findCaptured();
        newAt_.assign(n_ + 1, 0);
        bool known = false;  // whether the state is what the code before leaves, falling through
        size_t at = 0;
        while (at < n_) {
            const size_t size = instructionSize(code_, at);
            const int depth = depths_[at];
            line_ = f_.lines[at];
            if (at == skip_) {
                newAt_[at] = static_cast<uint32_t>(out_.size());
                skip_ = SIZE_MAX;
                at += size;
                continue;
            }
            if (at < skipUntil_) {
                // Read through by the RGetPath before it.
                newAt_[at] = static_cast<uint32_t>(out_.size());
                at += size;
                continue;
            }
            if (depth < 0) {
                // Never reached: left out. (Copied, a joined instruction in it would be read as the
                // R instruction of the same number by what reads the run code through, D160.)
                if (known) canonical();
                known = false;
                newAt_[at] = static_cast<uint32_t>(out_.size());
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
            // Where the VM's top is as this instruction's run code starts (where it is jumped to,
            // the stack's depth): kept for native code, which may go in at any instruction.
            if (top_ >= 0 && top_ <= 0xFFFF) f_.runTops.emplace_back(newAt_[at], static_cast<uint16_t>(top_));
            known = one(at, size, depth);
            // A function's result checked just after an instruction that gives one of that type
            // always (an int from AddI, whose operands the checker knew, D178): nothing to check.
            const size_t next = at + size;
            if (known && skip_ == SIZE_MAX && next + 1 < n_ && !leader_[next] && static_cast<Op>(code_[next]) == Op::CheckReturn &&
                alwaysGives(static_cast<Op>(code_[at])) == code_[next + 1])
                skip_ = next;
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
        if (pathCaches_ != 0) {
            // The caches the RGetPaths and RInvokes were given, after the stack code's, all empty yet.
            f_.cacheCount += pathCaches_;
            f_.caches.reset(new std::atomic<uint64_t>[f_.cacheCount]());
        }
    }

private:
    FunctionProto& f_;
    const std::vector<uint8_t>& code_;
    const std::vector<int>& depths_;
    const size_t n_;
    const bool notes_;       // whether values pushed are noted
    const bool rs_;          // whether R instructions are made
    const bool joinedOnly_;  // only of the stack code's joined instructions
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
    // An instruction already done, by the R instruction before it: a Pop after a SetProp.
    size_t skip_ = SIZE_MAX;
    // And all those before this, read through by an RGetPath (G14); and the caches RGetPaths
    // were given, after the stack code's.
    size_t skipUntil_ = 0;
    uint32_t pathCaches_ = 0;
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
        loadBody(const_cast<FunctionProto&>(nested));  // its own nested functions are in its body
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
        byte(static_cast<int>(o) & 0xFF);
        byte(static_cast<int>(o) >> 8);
    }
    Op opAt(size_t start) const { return jade::opAt(&out_[start]); }
    // Where operand k (from 1) of the instruction written at `start` is.
    static size_t operandAt(size_t start, int k) { return start + kOpBytes - 1 + static_cast<size_t>(k); }
    void putI32(size_t place, int32_t v) {
        const uint32_t u = static_cast<uint32_t>(v);
        for (int i = 0; i < 4; i++) out_[place + static_cast<size_t>(i)] = static_cast<uint8_t>(u >> (8 * i));
    }
    // An instruction of the stack code, as it is; a place it jumps to is moved where it now is.
    void copy(size_t at, size_t size) {
        lastR_ = false;
        const size_t start = out_.size();
        op(static_cast<Op>(code_[at]));
        for (size_t i = 1; i < size; i++) byte(code_[at + i]);
        const int t = targetOperand(static_cast<Op>(code_[at]));
        if (t != 0) fixups_.emplace_back(operandAt(start, t), static_cast<size_t>(readI32(code_, at + static_cast<size_t>(t))));
    }
    // The same into a slow way's stack code, as instruction `plain` with the operands of the one at `at`.
    void copyInto(Slow& slow, Op plain, size_t at, size_t size) {
        const size_t start = slow.code.size();
        slow.code.push_back(static_cast<uint8_t>(static_cast<int>(plain) & 0xFF));
        slow.code.push_back(static_cast<uint8_t>(static_cast<int>(plain) >> 8));
        for (size_t i = 1; i < size; i++) slow.code.push_back(code_[at + i]);
        const int t = targetOperand(plain);
        if (t != 0) slow.targets.emplace_back(operandAt(start, t), static_cast<size_t>(readI32(code_, at + static_cast<size_t>(t))));
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
    void endR(Slow& slow, int s) { finishR(slow, true, s); }
    // The same, with a result in slot s or none, the top then at s.
    void finishR(Slow& slow, bool result, int s) {
        slow.place = out_.size();
        i32(0);
        slow.size = out_.size() - slow.instruction;
        lastR_ = result;
        if (result) {
            stack_.push_back(Entry{});
            top_ = s + 1;
        } else {
            top_ = s;
        }
    }
    // A value an R instruction may take by a slot: in its slot, or noted as a local's.
    static bool inASlot(const Entry& e) { return e.kind != Entry::Constant; }
    // The same, or a constant it names by a byte.
    static bool byByte(const Entry& e) { return e.kind != Entry::Constant || e.index <= 255; }
    // Whether the instruction at `next`, not jumped to, reads a field of what the one before
    // gives, or does past a ?. : GetProp reads a value kept in an object through to that field at
    // once, where a GetProp as its own instruction would copy it (M6.6 part 5).
    bool readsOn(size_t next) const {
        if (next >= n_ || leader_[next]) return false;
        const Op o = static_cast<Op>(code_[next]);
        return o == Op::GetProp || o == Op::GetOwn || o == Op::GetPropRef || o == Op::GetOwnRef || o == Op::JumpIfNull;
    }
    // `a.b.c` or `a.b.c.d` from the instruction at `at`, a GetPropRef: two or three fields read
    // through values held in one another, the last a plain one, as one RGetPath, its slow way the
    // stack code of all of them (G14). Only from a GetPropRef, which the checker gives a value kept
    // inside an object: through objects (`a.next.value`) the path could never be kept, and each
    // read would try first. False where the code is not that.
    bool pathAt(size_t at, size_t size, int depth) {
        if (!notes_ || !rs_ || stack_.empty() || !noted(stack_.back()) || isConstant(stack_.back()) || depth - 1 > 255) return false;
        std::vector<std::pair<size_t, size_t>> steps{{at, size}};
        size_t next = at + size;
        while (steps.size() < 3 && next < n_ && !leader_[next]) {
            const Op o = static_cast<Op>(code_[next]);
            if (o != Op::GetProp && o != Op::GetOwn && o != Op::GetPropRef && o != Op::GetOwnRef) break;
            const size_t stepSize = instructionSize(code_, next);
            steps.emplace_back(next, stepSize);
            next += stepSize;
        }
        if (steps.size() < 2 || readsOn(next)) return false;  // one alone, or longer: as it is
        const Op last = static_cast<Op>(code_[steps.back().first]);
        if (last != Op::GetProp && last != Op::GetOwn) return false;
        if (f_.cacheCount + pathCaches_ >= 0xFFFF) return false;
        const int s = depth - 1;
        placeBelow(s);
        const Entry x = take(1)[0];
        const int bOperand = operand(x, s);
        Slow& slow = newSlow(s, at);
        for (const auto& [stepAt, stepSize] : steps) copyInto(slow, static_cast<Op>(code_[stepAt]), stepAt, stepSize);
        op(Op::RGetPath);
        u8(s);
        u8(bOperand);
        u8(s + 1);
        u8(s);
        u16(static_cast<int>(f_.cacheCount + pathCaches_++));
        u8(static_cast<int>(steps.size()));
        for (size_t k = 0; k < 3; k++) u16(k < steps.size() ? readU16(code_, steps[k].first + 1) : 0);
        endR(slow, s);
        skipUntil_ = next;
        return true;
    }

    // Whether the instruction after the one at `at` is a Pop, not jumped to.
    bool popAfter(size_t at, size_t size) const {
        const size_t next = at + size;
        return next < n_ && !leader_[next] && static_cast<Op>(code_[next]) == Op::Pop;
    }

    // `x op y`, its result in slot s: false where no R instruction does it.
    bool binaryAt(Op plain, Entry x, Entry y, int s, size_t at, size_t size) {
        const Family* fam = familyOf(plain);
        if (fam == nullptr) return false;
        // One that takes a constant second only (a shift's count): told before anything is written.
        if (fam->ss == Op::Count_ && (!isConstant(y) || y.index > 255)) return false;
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

    // The type an instruction's result always has, where it has one: kTypeInt, kTypeFloat or
    // kTypeBool; else 0.
    static int alwaysGives(Op o) {
        switch (o) {
            case Op::AddI:
            case Op::SubI:
            case Op::MulI: return kTypeInt;
            case Op::AddF:
            case Op::SubF:
            case Op::MulF:
            case Op::DivF: return kTypeFloat;
            case Op::LtI:
            case Op::LeI:
            case Op::GtI:
            case Op::GeI:
            case Op::EqI:
            case Op::LtF:
            case Op::LeF:
            case Op::GtF:
            case Op::GeF:
            case Op::EqF: return kTypeBool;
            default: return 0;
        }
    }

    // ---- One instruction ----

    // Translates the instruction at `at`; false where the code after it is not reached from it.
    bool one(size_t at, size_t size, int depth) {
        const Op o = static_cast<Op>(code_[at]);
        const int a = size >= 3 ? readU16(code_, at + 1) : 0;
        const int b = size >= 5 ? readU16(code_, at + 3) : 0;
        // A function's places, where it has none to give (D149): nothing to run.
        if (o == Op::EnterPlaces && code_[at + 1] == 0) return true;
        if (!notes_ && (o == Op::GetLocal || o == Op::Const || o == Op::Pop || o == Op::Dup || o == Op::GetProp || o == Op::GetOwn ||
                        o == Op::SetLocal || o == Op::CopyLocal || o == Op::SetProp || o == Op::SetOwn || o == Op::GetGlobal ||
                        o == Op::GetUpvalue || o == Op::GetIndex || o == Op::Neg || o == Op::Not))
            return asStackCode(o, at, size);
        // A plain operation: an R instruction only in full; in a function keeping its stack code, never.
        if ((joinedOnly_ || !rs_) && familyOf(o) != nullptr) return asStackCode(o, at, size);
        if (!rs_ && (o == Op::StoreLocal || o == Op::JumpIfFalse)) return asStackCode(o, at, size);
        if (joinedOnly_ && o == Op::JumpIfFalse && !lastR_) return asStackCode(o, at, size);
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
                if (!notes_) break;
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
            case Op::Mod:
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
            case Op::Eq:
            case Op::Shl:
            case Op::Shr:
            case Op::BitAnd:
            case Op::BitOr:
            case Op::AddI:
            case Op::SubI:
            case Op::MulI:
            case Op::LtI:
            case Op::LeI:
            case Op::GtI:
            case Op::GeI:
            case Op::EqI:
            case Op::AddF:
            case Op::SubF:
            case Op::MulF:
            case Op::DivF:
            case Op::LtF:
            case Op::LeF:
            case Op::GtF:
            case Op::GeF:
            case Op::EqF:
            {
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
            case Op::ModLocal:
            case Op::LtLocal:
            case Op::LeLocal:
            case Op::GtLocal:
            case Op::GeLocal:
            case Op::EqLocal: {
                const int s = depth - 1;
                placeBelow(s);
                const Entry x = take(1)[0];
                if (rs_ && binaryAt(plainOf(o), x, localEntry(a), s, at, 1)) return true;
                return expanded(plainOf(o), x, localEntry(a), s);
            }
            case Op::AddConst:
            case Op::SubConst:
            case Op::MulConst:
            case Op::DivConst:
            case Op::ModConst:
            case Op::LtConst:
            case Op::LeConst:
            case Op::GtConst:
            case Op::GeConst:
            case Op::EqConst: {
                const int s = depth - 1;
                placeBelow(s);
                const Entry x = take(1)[0];
                if (rs_ && binaryAt(plainOf(o), x, constantEntry(a), s, at, 1)) return true;
                return expanded(plainOf(o), x, constantEntry(a), s);
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
                if (rs_ && binaryAt(plainOf(o), x, y, s, at, 1)) return true;
                return expanded(plainOf(o), x, y, s);
            }
            case Op::GetPropRef:
            case Op::GetOwnRef:
                if (pathAt(at, size, depth)) return true;
                break;
            case Op::GetProp:
            case Op::GetOwn: {
                if (!noted(stack_.back()) || isConstant(stack_.back())) break;  // the stack code as it is
                if (readsOn(at + size)) break;  // a field of its field: GetProp reads through, copying nothing
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
            case Op::JumpIfFalse: {
                if (jumpIntoLast(depth, at, size)) return true;
                if (!inASlot(stack_.back())) break;
                const int s = depth - 1;
                placeBelow(s);
                const Entry x = take(1)[0];
                const int bOperand = operand(x, s);
                Slow& slow = newSlow(s, at);
                copyInto(slow, o, at, size);
                // u8 B, u8 T, i32 target, i32 slow.
                op(Op::RJumpIfFalse);
                u8(bOperand);
                u8(s);
                fixups_.emplace_back(out_.size(), static_cast<size_t>(readI32(code_, at + 1)));
                i32(0);
                finishR(slow, false, s);
                return true;
            }
            case Op::SetProp:
            case Op::SetOwn: {
                const Entry obj = stack_[stack_.size() - 2], value = stack_.back();
                if (!inASlot(obj) || !byByte(value)) break;
                const int s = depth - 2;
                placeBelow(s);
                take(2);
                const bool statement = popAfter(at, size);
                const int objOperand = operand(obj, s), valueOperand = operand(value, s + 1);
                Slow& slow = newSlow(s, at);
                copyInto(slow, o, at, size);
                if (statement) {
                    // The Pop after it, done by it: nothing left on the stack.
                    copyInto(slow, Op::Pop, at + size, 1);
                    skip_ = at + size;
                }
                // u8 A, u8 B, u8 C, u8 T, u8 S, u16 cache, i32 slow.
                op(isConstant(value) ? Op::RSetPropSK : Op::RSetPropSS);
                u8(s);
                u8(objOperand);
                u8(valueOperand);
                u8(statement ? s : s + 1);
                u8(s);
                u16(b);
                finishR(slow, !statement, s);
                lastR_ = false;
                return true;
            }
            case Op::GetGlobal: {
                const int s = depth;
                placeBelow(s);
                Slow& slow = newSlow(s, at);
                copyInto(slow, o, at, size);
                // u8 A, u8 T, u8 S, u16 global, i32 slow.
                op(Op::RGetGlobal);
                u8(s);
                u8(s + 1);
                u8(s);
                u16(a);
                endR(slow, s);
                return true;
            }
            case Op::GetUpvalue: {
                const int s = depth;
                placeBelow(s);
                // u8 A, u8 T, u16 upvalue: nothing to go slow for.
                op(Op::RGetUpvalue);
                u8(s);
                u8(s + 1);
                u16(a);
                stack_.push_back(Entry{});
                top_ = s + 1;
                return true;
            }
            case Op::GetIndexRef: {
                // `items[i].x`, the field a plain one, read on (not a field of it next): one R
                // instruction, its slow way the two of the stack code.
                const size_t next = at + size;
                if (next >= n_ || leader_[next]) break;
                const Op read = static_cast<Op>(code_[next]);
                if (read != Op::GetProp && read != Op::GetOwn) break;
                const size_t readSize = instructionSize(code_, next);
                if (readsOn(next + readSize)) break;
                const Entry obj = stack_[stack_.size() - 2], index = stack_.back();
                if (!inASlot(obj) || !byByte(index)) break;
                const int s = depth - 2;
                placeBelow(s);
                take(2);
                const int objOperand = operand(obj, s), indexOperand = operand(index, s + 1);
                Slow& slow = newSlow(s, at);
                copyInto(slow, o, at, size);
                copyInto(slow, read, next, readSize);
                skip_ = next;
                // u8 A, u8 B, u8 C, u8 T, u8 S, u16 cache, i32 slow.
                op(isConstant(index) ? Op::RGetIndexPropSK : Op::RGetIndexPropSS);
                u8(s);
                u8(objOperand);
                u8(indexOperand);
                u8(s + 1);
                u8(s);
                u16(readU16(code_, next + 3));
                endR(slow, s);
                return true;
            }
            case Op::GetIndex: {
                const Entry obj = stack_[stack_.size() - 2], index = stack_.back();
                if (!inASlot(obj) || !byByte(index)) break;
                const int s = depth - 2;
                placeBelow(s);
                take(2);
                const int objOperand = operand(obj, s), indexOperand = operand(index, s + 1);
                Slow& slow = newSlow(s, at);
                copyInto(slow, o, at, size);
                // u8 A, u8 B, u8 C, u8 T, u8 S, i32 slow.
                op(isConstant(index) ? Op::RGetIndexSK : Op::RGetIndexSS);
                u8(s);
                u8(objOperand);
                u8(indexOperand);
                u8(s + 1);
                u8(s);
                endR(slow, s);
                return true;
            }
            case Op::Invoke:
            case Op::InvokeOwn:
                if (invokeAt(o, at, size, depth)) return true;
                break;
            case Op::Call:
                if (callAt(at, size, depth)) return true;
                break;
            case Op::Neg:
            case Op::Not: {
                if (!inASlot(stack_.back())) break;
                const int s = depth - 1;
                placeBelow(s);
                const Entry x = take(1)[0];
                const int bOperand = operand(x, s);
                Slow& slow = newSlow(s, at);
                copyInto(slow, o, at, size);
                // u8 A, u8 B, u8 T, u8 S, i32 slow.
                op(o == Op::Neg ? Op::RNeg : Op::RNot);
                u8(s);
                u8(bOperand);
                u8(s + 1);
                u8(s);
                endR(slow, s);
                return true;
            }
            default: break;
        }
        return asStackCode(o, at, size);
    }

    // `receiver.name(x1, x2)`, up to four arguments, as an RInvoke: false where the code is not that.
    bool invokeAt(Op o, size_t at, size_t size, int depth) {
        if (!notes_ || !rs_ || joinedOnly_) return false;
        const int argc = code_[at + 3];
        if (!callStart(Op::RInvoke, o, at, size, depth, argc, o == Op::InvokeOwn ? 16 : 0)) return false;
        u16(readU16(code_, at + 1));
        if (f_.cacheCount + pathCaches_ >= 0xFFFF) fail("too many caches");
        u16(static_cast<int>(f_.cacheCount + pathCaches_++));
        callEnd(depth - argc - 1);
        return true;
    }

    // `f(x1, x2)`, up to four arguments, as an RCall: false where the code is not that.
    bool callAt(size_t at, size_t size, int depth) {
        if (!notes_ || !rs_ || joinedOnly_) return false;
        const int argc = code_[at + 1];
        if (!callStart(Op::RCall, Op::Call, at, size, depth, argc, 0)) return false;
        callEnd(depth - argc - 1);
        return true;
    }

    // What an RCall and an RInvoke begin with (instructions.inc): the function or receiver at slot
    // s, under `argc` arguments, written as `r`, the stack's instruction `o` its slow way; false,
    // with nothing written, where they are not all where a byte can name them.
    bool callStart(Op r, Op o, size_t at, size_t size, int depth, int argc, int flags) {
        const int s = depth - argc - 1;
        if (argc > 4 || s < 0 || s + argc + 1 > 255) return false;
        const Entry callee = stack_[static_cast<size_t>(s)];
        if (!inASlot(callee)) return false;
        for (int k = 0; k < argc; k++)
            if (!byByte(stack_[static_cast<size_t>(s + 1 + k)])) return false;
        placeBelow(s);
        const std::vector<Entry> taken = take(argc + 1);
        int x[4] = {0, 0, 0, 0};
        for (int k = 0; k < argc; k++) {
            x[k] = operand(taken[static_cast<size_t>(k + 1)], s + 1 + k);
            if (isConstant(taken[static_cast<size_t>(k + 1)])) flags |= 1 << k;
        }
        Slow& slow = newSlow(s, at);
        copyInto(slow, o, at, size);
        op(r);
        u8(s);
        u8(operand(callee, s));
        u8(s + 1);
        u8(s);
        u8(argc);
        u8(flags);
        for (int k = 0; k < 4; k++) u8(x[k]);
        return true;
    }
    // And what they end with: the slow way's start and its resume, the result left at slot s.
    void callEnd(int s) {
        Slow& slow = slows_.back();
        slow.place = out_.size();
        i32(0);
        slow.resumePlace = out_.size();
        i32(0);
        slow.size = out_.size() - slow.instruction;
        lastR_ = true;
        stack_.push_back(Entry{});
        top_ = s + 1;
    }

    // The stack as the stack code has it, and the instruction as it is.
    bool asStackCode(Op o, size_t at, size_t size) {
        canonical();
        copy(at, size);
        const size_t next = at + size;
        if (terminal(o) || next >= n_ || depths_[next] < 0) return false;
        reset(depths_[next]);
        return true;
    }

    // A joined instruction as the instructions it joins: `x op y`, x at slot s, y pushed after it.
    bool expanded(Op plain, Entry x, Entry y, int s) {
        stack_.push_back(x);
        stack_.push_back(y);
        canonical();
        op(plain);
        stack_.resize(stack_.size() - 2);
        stack_.push_back(Entry{});
        top_ = s + 1;
        return true;
    }

    // `StoreLocal x` of the result of the R instruction just written: written straight into x.
    // Nothing noted reads x, as every value noted below an R instruction is put in its slot first.
    bool storeIntoLast(int x, int depth, size_t at, size_t size) {
        if (!lastR_ || x > 255 || noted(stack_.back())) return false;
        Slow& slow = slows_.back();
        const size_t start = slow.instruction;
        const Op r = opAt(start);
        const int s = slow.s;
        if (!storesResult(r) || out_[operandAt(start, 1)] != s || s != depth - 1) return false;
        out_[operandAt(start, 1)] = static_cast<uint8_t>(x);
        // The top after it: at s, the result having gone to x.
        out_[operandAt(start, topOperand(r))] = static_cast<uint8_t>(s);
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
        const Op jumping = jumpFormOf(opAt(start));
        if (jumping == Op::Count_) return false;
        const int s = slow.s;
        if (out_[operandAt(start, 1)] != s || s != depth - 1) return false;
        const int bOperand = out_[operandAt(start, 2)], cOperand = out_[operandAt(start, 3)];
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
        if (slow.resumePlace != SIZE_MAX)
            putI32(slow.resumePlace, static_cast<int32_t>(codeAt + kOpBytes + instructionSize(code_, slow.old) - 1));
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
            case Op::ModLocal: case Op::ModConst: return Op::Mod;
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

// Where in an instruction's operands (counted from the first) offsets in the code are: the place
// it jumps to and the start of its slow way, as many as it has, -1 for none.
struct CodeOffsets {
    int first = -1, second = -1;
};
CodeOffsets codeOffsets(Op o) {
    switch (o) {
        case Op::MoveS:
        case Op::MoveK:
        case Op::SetTop:
        case Op::Translate:
        case Op::RGetUpvalue: return {};
        case Op::RGetProp: return {6};
        case Op::RGetPath: return {13};
        case Op::RInvoke: return {14, 18};
        case Op::RCall: return {10, 14};
        case Op::RSetPropSS:
        case Op::RSetPropSK:
        case Op::RGetIndexPropSS:
        case Op::RGetIndexPropSK: return {7};
        case Op::RGetGlobal:
        case Op::RGetIndexSS:
        case Op::RGetIndexSK: return {5};
        case Op::RNeg:
        case Op::RNot: return {4};
        case Op::RJumpIfFalse: return {2, 6};
        case Op::RModSS:
        case Op::RModSK:
        case Op::RSubKS:
        case Op::RDivKS:
        case Op::RShlSK:
        case Op::RShrSK:
        case Op::RBitAndSS:
        case Op::RBitAndSK:
        case Op::RBitOrSS:
        case Op::RBitOrSK: return {5};
        default: break;
    }
    if ((o >= Op::RJumpLtSS && o <= Op::RJumpEqSK) || (o >= Op::RJumpLtISS && o <= Op::RJumpEqFSK)) return {3, 7};
    if ((o >= Op::RAddSS && o <= Op::REqSK) || (o >= Op::RAddISS && o <= Op::REqFSK)) return {5};
    const int t = targetOperand(o);
    return {t - 1};
}

// The code made, given kThreaded bytes before each instruction (D160): every offset in it, in its
// instructions, its line table and its rights, moved to where that instruction now starts. The
// code given has opcodes of `opBytes` (the translator's, kOpBytes; the stack code kept as it is,
// a byte), the code made of kOpBytes.
void threadCode(FunctionProto& f, size_t opBytes) {
    const std::vector<uint8_t>& old = f.run;
    auto opOf = [&](size_t at) { return opBytes == 1 ? static_cast<Op>(old[at]) : jade::opAt(&old[at]); };
    auto sizeOf = [&](size_t at) {
        const char* name = nullptr;
        return opBytes == 1 ? instructionSize(old, at) : runSize(old, at, name);
    };
    std::vector<uint32_t> moved(old.size() + 1, UINT32_MAX);  // an instruction's start, now
    size_t now = 0;
    for (size_t at = 0; at < old.size();) {
        moved[at] = static_cast<uint32_t>(now);
        const size_t size = sizeOf(at);
        now += kThreaded + kOpBytes + size - opBytes;
        at += size;
        if (at > old.size()) throw std::runtime_error("an instruction runs past the code's end");
    }
    moved[old.size()] = static_cast<uint32_t>(now);
    auto where = [&](uint32_t offset) -> uint32_t {
        if (offset > old.size() || moved[offset] == UINT32_MAX) throw std::runtime_error("an offset into an instruction");
        return moved[offset];
    };
    std::vector<uint8_t> run;
    std::vector<int> lines;
    run.reserve(moved[old.size()]);
    lines.reserve(run.capacity());
    for (size_t at = 0; at < old.size();) {
        const size_t size = sizeOf(at);
        const Op o = opOf(at);
        const size_t operands = run.size() + kThreaded + kOpBytes;  // where its operands are, now
        for (size_t i = 0; i < kThreaded; i++) {
            run.push_back(0);
            lines.push_back(f.runLines[at]);
        }
        run.push_back(static_cast<uint8_t>(static_cast<int>(o) & 0xFF));
        run.push_back(static_cast<uint8_t>(static_cast<int>(o) >> 8));
        lines.push_back(f.runLines[at]);
        lines.push_back(f.runLines[at]);
        for (size_t i = opBytes; i < size; i++) {
            run.push_back(old[at + i]);
            lines.push_back(f.runLines[at + i]);
        }
        const CodeOffsets offsets = codeOffsets(o);
        for (const int place : {offsets.first, offsets.second}) {
            if (place < 0) continue;
            if (opBytes + static_cast<size_t>(place) + 4 > size) throw std::runtime_error("an offset past its instruction");
            int32_t target;
            std::memcpy(&target, &old[at + opBytes + static_cast<size_t>(place)], 4);
            const int32_t there = static_cast<int32_t>(where(static_cast<uint32_t>(target)));
            std::memcpy(&run[operands + static_cast<size_t>(place)], &there, 4);
        }
        at += size;
    }
    for (FunctionProto::RunRights& r : f.runRights) {
        r.start = where(r.start);
        r.end = where(r.end);
    }
    f.run = std::move(run);
    f.runLines = std::move(lines);
    f.regions.clear();
    if (opBytes == kOpBytes && !f.runTops.empty()) {
        // Each where an instruction starts; one inside an instruction (a comparison the jump
        // after it was joined to) is dropped.
        std::erase_if(f.runTops, [&](const auto& t) { return t.first >= moved.size() || moved[t.first] == UINT32_MAX; });
        for (auto& [at, top] : f.runTops) at = where(at);
        if (makeNativeRegions != nullptr) makeNativeRegions(f);
    }
    f.runTops.clear();
    f.runTops.shrink_to_fit();
    if (writeRunAddresses != nullptr) writeRunAddresses(f);
}

}  // namespace

void (*writeRunAddresses)(FunctionProto& f) = nullptr;
void (*makeNativeRegions)(FunctionProto& f) = nullptr;

void runLater(FunctionProto& f) {
    std::vector<uint8_t> run(kThreaded, 0);
    run.push_back(static_cast<uint8_t>(static_cast<int>(Op::Translate) & 0xFF));
    run.push_back(static_cast<uint8_t>(static_cast<int>(Op::Translate) >> 8));
    f.run = std::move(run);
    f.runLines.assign(f.run.size(), f.lines.empty() ? 0 : f.lines[0]);
    f.runRights.clear();
    f.addressed.done.store(false, std::memory_order_relaxed);
    if (writeRunAddresses != nullptr) writeRunAddresses(f);
}

// The length of the run code's instruction whose opcode is at `at`, and its name.
size_t runSize(const std::vector<uint8_t>& run, size_t at, const char*& name) {
    const Op o = jade::opAt(&run[at]);
    name = opName(o);
    switch (o) {
        case Op::MoveS:
        case Op::MoveK: return kOpBytes + 2;
        case Op::SetTop: return kOpBytes + 1;
        case Op::Translate: return kOpBytes;
        case Op::RGetProp: return kOpBytes + 10;
        case Op::RGetPath: return kOpBytes + 17;
        case Op::RInvoke: return kOpBytes + 22;
        case Op::RCall: return kOpBytes + 18;
        case Op::RSetPropSS:
        case Op::RSetPropSK:
        case Op::RGetIndexPropSS:
        case Op::RGetIndexPropSK: return kOpBytes + 11;
        case Op::RGetUpvalue: return kOpBytes + 4;
        case Op::RNeg:
        case Op::RNot: return kOpBytes + 8;
        case Op::RJumpIfFalse: return kOpBytes + 10;
        case Op::RGetGlobal:
        case Op::RGetIndexSS:
        case Op::RGetIndexSK:
        case Op::RModSS:
        case Op::RModSK:
        case Op::RSubKS:
        case Op::RDivKS:
        case Op::RShlSK:
        case Op::RShrSK:
        case Op::RBitAndSS:
        case Op::RBitAndSK:
        case Op::RBitOrSS:
        case Op::RBitOrSK: return kOpBytes + 9;
        default: break;
    }
    if ((o >= Op::RJumpLtSS && o <= Op::RJumpEqSK) || (o >= Op::RJumpLtISS && o <= Op::RJumpEqFSK)) return kOpBytes + 11;
    if ((o >= Op::RAddSS && o <= Op::REqSK) || (o >= Op::RAddISS && o <= Op::REqFSK)) return kOpBytes + 9;
    if (o >= Op::Count_) throw std::runtime_error("an instruction the run code does not have");
    return kOpBytes + operandBytes(o);
}

// JADE_DUMP_RUN=name: the run code made for each function of that name, to standard error, for
// looking at what the translator does.
void dump(const FunctionProto& f) {
    static const char* const wanted = std::getenv("JADE_DUMP_RUN");
    if (wanted == nullptr || f.name != wanted) return;
    std::fprintf(stderr, "== %s: %zu bytes of stack code, %zu of run code\n", f.name.c_str(), f.code.size(), f.run.size());
    for (size_t at = 0; at + kThreaded < f.run.size();) {
        const char* name = "?";
        const size_t size = runSize(f.run, at + kThreaded, name);
        std::fprintf(stderr, "%5zu  line %-4d %-18s", at, f.sourceLine(f.runLines[at]), name);
        for (size_t i = kOpBytes; i < size && at + kThreaded + i < f.run.size(); i++) std::fprintf(stderr, " %3d", f.run[at + kThreaded + i]);
        std::fprintf(stderr, "\n");
        at += kThreaded + size;
    }
}

void translate(FunctionProto& f, const std::vector<int>& depths) {
    if (f.chooser || f.code.empty() || depths.size() != f.code.size()) {
        asItIs(f);
        threadCode(f, 1);
        return;
    }
    // The register code in full, unless JADE_REGISTER_CODE says otherwise, for testing and timing
    // the other ways (the M6.7 plan, docs/plans-and-gaps.html): =joined makes only the stack code's joined instructions R
    // instructions, as the default was until part D, and =plain makes every function keep its stack
    // code, as one of more than 255 slots does. Any other value is the default.
    static const char* const asked = std::getenv("JADE_REGISTER_CODE");
    static const Mode mode = asked == nullptr                    ? Mode::Full
                             : std::string(asked) == "joined"    ? Mode::Joined
                             : std::string(asked) == "plain"     ? Mode::Plain
                                                                 : Mode::Full;
    try {
        Translator(f, depths, mode).translate();
        threadCode(f, kOpBytes);
        dump(f);
        return;
    } catch (const std::exception& e) {
        // Code the translator does not expect keeps its stack code, which runs as before; with
        // JADE_TRANSLATE_STRICT set, a test of the translator, it is an error. A function needing
        // more than 255 slots keeps its stack code either way.
        if (std::getenv("JADE_TRANSLATE_STRICT") != nullptr && f.maxStack <= 255)
            throw std::runtime_error(std::string("cannot translate ") + f.name + ": " + e.what());
    }
    Translator(f, depths, Mode::Plain).translate();
    threadCode(f, kOpBytes);
    dump(f);
}

}  // namespace jade
