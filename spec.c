/**
 * Simple x86_64 emulator/re-generator
 * (c) 2015, Josef Weidendorfer, GPLv2+
 */

#include "spec.h"

#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>


// forward declarations
typedef struct _DBB DBB;
typedef struct _CBB CBB;
typedef struct _EmuState EmuState;
typedef struct _CaptureConfig CaptureConfig;

void printDecodedBB(DBB* bb);
DBB* decodeBB(Rewriter* c, uint64_t f);
void freeEmuState(Rewriter*);
void freeCaptureConfig(Rewriter*);

/*------------------------------------------------------------
 * Code Storage
 */

typedef struct _CodeStorage {
    int size;
    int fullsize; /* rounded to multiple of a page size */
    int used;
    uint8_t* buf;
} CodeStorage;

CodeStorage* initCodeStorage(int size)
{
    int fullsize;
    uint8_t* buf;
    CodeStorage* cs;

    /* round up size to multiple of a page size */
    fullsize = (size + 4095) & ~4095;

    /* We do not want to use malloc as we need execute permission.
    * This will return an address aligned to a page boundary
    */
    buf = (uint8_t*) mmap(0, fullsize,
                          PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (buf == (uint8_t*)-1) {
        perror("Can not mmap code region.");
        exit(1);
    }

    cs = (CodeStorage*) malloc(sizeof(CodeStorage));
    cs->size = size;
    cs->fullsize = fullsize;
    cs->buf = buf;
    cs->used = 0;

    //fprintf(stderr, "Allocated Code Storage (size %d)\n", fullsize);

    return cs;
}

void freeCodeStorage(CodeStorage* cs)
{
    if (cs)
        munmap(cs->buf, cs->fullsize);
    free(cs);
}

/* this checks whether enough storage is available, but does
 * not change <used>.
 */
uint8_t* reserveCodeStorage(CodeStorage* cs, int size)
{
    if (cs->fullsize - cs->used < size) {
        fprintf(stderr,
                "Error: CodeStorage (size %d) too small: used %d, need %d\n",
                cs->fullsize, cs->used, size);
        exit(1);
    }
    return cs->buf + cs->used;
}

uint8_t* useCodeStorage(CodeStorage* cs, int size)
{
    uint8_t* p = cs->buf + cs->used;
    assert(cs->fullsize - cs->used >= size);
    cs->used += size;
    return p;
}

/*------------------------------------------------------------*/
/* x86_64 Analyzers
 */

typedef enum _Reg {
    Reg_None = 0,
    // general purpose (order is important, aligned to x86 encoding)
    Reg_AX, Reg_CX, Reg_DX, Reg_BX, Reg_SP, Reg_BP, Reg_SI, Reg_DI,
    Reg_8,  Reg_9,  Reg_10, Reg_11, Reg_12, Reg_13, Reg_14, Reg_15,
    Reg_IP,
    // vector regs (MMX, XMM, YMM)
    Reg_X0, Reg_X1, Reg_X2, Reg_X3, Reg_X4, Reg_X5, Reg_X6, Reg_X7,
    Reg_X8, Reg_X9, Reg_X10, Reg_X11, Reg_X12, Reg_X13, Reg_X14, Reg_X15,
    //
    Reg_Max
} Reg;

typedef enum _InstrType {
    IT_None = 0, IT_Invalid,
    IT_NOP,
    IT_CLTQ,
    IT_PUSH, IT_POP, IT_LEAVE,
    IT_MOV, IT_MOVSX, IT_LEA,
    IT_NEG,
    IT_ADD, IT_SUB, IT_IMUL,
    IT_XOR, IT_AND, IT_OR,
    IT_SHL, IT_SHR, IT_SAR,
    IT_CALL, IT_RET, IT_JMP,
    IT_JG, IT_JE, IT_JNE, IT_JLE, IT_JP,
    IT_CMP, IT_TEST,
    // SSE
    IT_PXOR, IT_MOVSD, IT_MULSD, IT_ADDSD, IT_SUBSD, IT_UCOMISD,
    //
    IT_Max
} InstrType;

typedef enum _ValType {
    VT_None = 0,
    VT_8, VT_16, VT_32, VT_64, VT_128, VT_256,
    //
    VT_Max
} ValType;

typedef enum _OpType {
    OT_None = 0,
    OT_Imm8, OT_Imm16, OT_Imm32, OT_Imm64,
    OT_Reg8, OT_Reg16, OT_Reg32, OT_Reg64, OT_Reg128, OT_Reg256,
    // mem (64bit addr): register indirect + displacement
    OT_Ind8, OT_Ind16, OT_Ind32, OT_Ind64, OT_Ind128, OT_Ind256,
    //
    OT_MAX
} OpType;

typedef struct _Operand {
    OpType type;
    Reg reg;
    Reg ireg; // with SIB
    uint64_t val; // imm or displacement
    int scale; // with SIB
} Operand;

// for passthrough instructions
typedef enum _OperandEncoding {
    OE_Invalid = 0,
    OE_None,
    OE_RM, OE_MR, OE_RMI
} OperandEncoding;

typedef enum _PrefixSet {
    PS_None = 0,
    PS_REX = 1,
    PS_66 = 2,
    PS_F2 = 4,
    PS_F3 = 8
} PrefixSet;

typedef enum _OperandForm {
    OF_None = 0,
    OF_0, // no operand or implicit
    OF_1, // 1 operand: push/pop/...
    OF_2, // 2 operands: dst = dst op src
    OF_3, // 3 operands: dst = src op src2
    OF_Max
} OperandForm;

typedef struct _Instr {
    uint64_t addr;
    int len;
    InstrType type;

    // annotation for pass-through (not used when ptLen == 0)
    int ptLen;
    PrefixSet ptPSet;
    unsigned char ptOpc[4];
    OperandEncoding ptEnc;

    ValType vtype; // without explicit operands or all operands of same type
    OperandForm form;
    Operand dst, src; //  with binary op: dst = dst op src
    Operand src2; // with ternary op: dst = src op src2
} Instr;

// a dedoced basic block
typedef struct _DBB {
    uint64_t addr;
    int count;
    Instr* instr; // pointer to first decoded instruction
} DBB;

// a captured basic block
typedef struct _CBB {
    // ID: address of original BB + EmuState at start
    uint64_t dec_addr;
    int esID;

    // instructions captured within this BB
    int count;
    Instr* instr;

    // two possible exits: next on branching or fall-through
    CBB *nextBranch, *nextFallThrough;
    // type of instruction ending this BB
    InstrType endType;

    // for code generation/relocation
    uint64_t addr;
    int size;
} CBB;


typedef struct _Rewriter {

    // decoded instructions
    int decInstrCount, decInstrCapacity;
    Instr* decInstr;

    // decoded basic blocks
    int decBBCount, decBBCapacity;
    DBB* decBB;

    // captured instructions
    int capInstrCount, capInstrCapacity;
    Instr* capInstr;

    // captured basic blocks
    int capBBCount, capBBCapacity;
    CBB* capBB;
    CBB* currentCapBB;

    // function to capture
    uint64_t func;

    // buffer for captured code
    int capCodeCapacity;
    CodeStorage* cs;

    // structs for emulator & capture config
    CaptureConfig* cc;
    EmuState* es;
    // saved emulator states
#define SAVEDSTATE_MAX 20
    int savedStateCount;
    EmuState* savedState[SAVEDSTATE_MAX];

    // stack of unfinished BBs to capture
#define CAPTURESTACK_LEN 10
    int capStackTop;
    CBB* capStack[CAPTURESTACK_LEN];

    // debug output
    Bool showDecoding, showEmuState, showEmuSteps;
} Rewriter;

// REX prefix, used in parseModRM
#define REX_MASK_B 1
#define REX_MASK_X 2
#define REX_MASK_R 4
#define REX_MASK_W 8

Rewriter* allocRewriter()
{
    Rewriter* r;
    int i;

    r = (Rewriter*) malloc(sizeof(Rewriter));

    // allocation of other members on demand, capacities may be reset

    r->decInstrCount = 0;
    r->decInstrCapacity = 0;
    r->decInstr = 0;

    r->decBBCount = 0;
    r->decBBCapacity = 0;
    r->decBB = 0;

    r->capInstrCount = 0;
    r->capInstrCapacity = 0;
    r->capInstr = 0;

    r->capBBCount = 0;
    r->capBBCapacity = 0;
    r->capBB = 0;
    r->currentCapBB = 0;
    r->capStackTop = -1;

    r->savedStateCount = 0;
    for(i=0; i< SAVEDSTATE_MAX; i++)
        r->savedState[i] = 0;

    r->capCodeCapacity = 0;
    r->cs = 0;

    r->cc = 0;
    r->es = 0;

    // default: debug off
    r->showDecoding = False;
    r->showEmuState = False;
    r->showEmuSteps = False;

    return r;
}

void initRewriter(Rewriter* r)
{
    if (r->decInstr == 0) {
        // default
        if (r->decInstrCapacity == 0) r->decInstrCapacity = 500;
        r->decInstr = (Instr*) malloc(sizeof(Instr) * r->decInstrCapacity);
    }
    r->decInstrCount = 0;

    if (r->decBB == 0) {
        // default
        if (r->decBBCapacity == 0) r->decBBCapacity = 20;
        r->decBB = (DBB*) malloc(sizeof(DBB) * r->decBBCapacity);
    }
    r->decBBCount = 0;

    if (r->capInstr == 0) {
        // default
        if (r->capInstrCapacity == 0) r->capInstrCapacity = 500;
        r->capInstr = (Instr*) malloc(sizeof(Instr) * r->capInstrCapacity);
    }
    r->capInstrCount = 0;

    if (r->capBB == 0) {
        // default
        if (r->capBBCapacity == 0) r->capBBCapacity = 20;
        r->capBB = (CBB*) malloc(sizeof(CBB) * r->capBBCapacity);
    }
    r->capBBCount = 0;
    r->currentCapBB = 0;

    if (r->cs == 0) {
        if (r->capCodeCapacity == 0) r->capCodeCapacity = 3000;
        if (r->capCodeCapacity >0)
            r->cs = initCodeStorage(r->capCodeCapacity);
    }
    if (r->cs)
        r->cs->used = 0;
}

void freeRewriter(Rewriter* r)
{
    if (!r) return;

    free(r->decInstr);
    free(r->decBB);
    free(r->capInstr);
    free(r->capBB);

    freeCaptureConfig(r);
    freeEmuState(r);

    if (r->cs)
        freeCodeStorage(r->cs);
    free(r);
}

void setRewriterDecodingCapacity(Rewriter* r,
                                 int instrCapacity, int bbCapacity)
{
    r->decInstrCapacity = instrCapacity;
    free(r->decInstr);
    r->decInstr = 0;

    r->decBBCapacity = bbCapacity;
    free(r->decBB);
    r->decBB = 0;
}

void setRewriterCaptureCapacity(Rewriter* r,
                                int instrCapacity, int bbCapacity,
                                int codeCapacity)
{
    r->capInstrCapacity = instrCapacity;
    free(r->capInstr);
    r->capInstr = 0;

    r->capBBCapacity = bbCapacity;
    free(r->capBB);
    r->capBB = 0;

    if (r->cs)
        freeCodeStorage(r->cs);
    r->cs = 0;
    r->capCodeCapacity = codeCapacity;
}


void setFunc(Rewriter* rewriter, uint64_t f)
{
    rewriter->func = f;

    // reset all decoding/state
    initRewriter(rewriter);
    resetRewriterConfig(rewriter);

    freeEmuState(rewriter);
}

void setVerbosity(Rewriter* rewriter,
                  Bool decode, Bool emuState, Bool emuSteps)
{
    rewriter->showDecoding = decode;
    rewriter->showEmuState = emuState;
    rewriter->showEmuSteps = emuSteps;
}

uint64_t generatedCode(Rewriter* c)
{
    if ((c->cs == 0) || (c->cs->used == 0))
        return 0;

    return (uint64_t) c->cs->buf;
}

int generatedCodeSize(Rewriter* c)
{
    if ((c->cs == 0) || (c->cs->used == 0))
        return 0;

    return c->cs->used;
}

void freeCode(Rewriter* c)
{
    if (c->cs)
        freeCodeStorage(c->cs);

    free(c->cc);
    free(c->es);

    free(c->decBB);
    free(c->decInstr);
    free(c);
}

ValType opValType(Operand* o)
{
    switch(o->type) {
    case OT_Imm8:
    case OT_Reg8:
    case OT_Ind8:
        return VT_8;
    case OT_Imm16:
    case OT_Reg16:
    case OT_Ind16:
        return VT_16;
    case OT_Imm32:
    case OT_Reg32:
    case OT_Ind32:
        return VT_32;
    case OT_Imm64:
    case OT_Reg64:
    case OT_Ind64:
        return VT_64;
    case OT_Reg128:
    case OT_Ind128:
        return VT_128;
    case OT_Reg256:
    case OT_Ind256:
        return VT_256;

    default: assert(0);
    }
    return 0; // invalid;
}

int opTypeWidth(Operand* o)
{
    switch(opValType(o)) {
    case VT_8: return 8;
    case VT_16: return 16;
    case VT_32: return 32;
    case VT_64: return 64;
    case VT_128: return 128;
    case VT_256: return 256;
    }
    assert(0);
    return 0;
}

Bool opIsImm(Operand* o)
{
    switch(o->type) {
    case OT_Imm8:
    case OT_Imm16:
    case OT_Imm32:
    case OT_Imm64:
        return True;
    }
    return False;
}

Bool opIsReg(Operand* o)
{
    switch(o->type) {
    case OT_Reg8:
    case OT_Reg16:
    case OT_Reg32:
    case OT_Reg64:
    case OT_Reg128:
    case OT_Reg256:
        return True;
    }
    return False;
}

Bool opIsGPReg(Operand* o)
{
    if (!opIsReg(o)) return False;
    if ((o->reg >= Reg_AX) && (o->reg <= Reg_15))
        return True;
    return False;
}

Bool opIsVReg(Operand* o)
{
    if (!opIsReg(o)) return False;
    if ((o->reg >= Reg_X0) && (o->reg <= Reg_X15))
        return True;
    return False;
}


Bool opIsInd(Operand* o)
{
    switch(o->type) {
    case OT_Ind8:
    case OT_Ind16:
    case OT_Ind32:
    case OT_Ind64:
    case OT_Ind128:
    case OT_Ind256:
        return True;
    }
    return False;
}

Bool opsAreSame(Operand* o1, Operand* o2)
{
    if (o1->type != o2->type)
        return False;
    if (opIsReg(o1))
        return (o1->reg == o2->reg);
    if (opIsImm(o1))
        return (o1->val == o2->val);
    // memory
    assert(opIsInd(o1));
    if ((o1->val != o2->val) || (o1->reg != o2->reg)) return False;
    if (o1->scale == 0) return True;
    if ((o1->scale != o2->scale) || (o1->ireg != o2->ireg)) return False;
    return True;
}

Operand* getRegOp(ValType t, Reg r)
{
    static Operand o;

    if ((r >= Reg_AX) && (r <= Reg_15)) {
        switch(t) {
        case VT_8:  o.type = OT_Reg8; break;
        case VT_16: o.type = OT_Reg16; break;
        case VT_32: o.type = OT_Reg32; break;
        case VT_64: o.type = OT_Reg64; break;
        default: assert(0);
        }
        o.reg = r;
        o.scale = 0;
        return &o;
    }

    if ((r >= Reg_X0) && (r <= Reg_X15)) {
        switch(t) {
        case VT_64:  o.type = OT_Reg64; break;
        case VT_128: o.type = OT_Reg128; break;
        case VT_256: o.type = OT_Reg256; break;
        default: assert(0);
        }
        o.reg = r;
        o.scale = 0;
        return &o;
    }
    assert(0);
}

Operand* getImmOp(ValType t, uint64_t v)
{
    static Operand o;

    switch(t) {
    case VT_8:
        o.type = OT_Imm8;
        o.val = v;
        break;

    case VT_16:
        o.type = OT_Imm16;
        o.val = v;
        break;

    case VT_32:
        o.type = OT_Imm32;
        o.val = v;
        break;

    case VT_64:
        o.type = OT_Imm64;
        o.val = v;
        break;

    default: assert(0);
    }

    return &o;
}


void copyOperand(Operand* dst, Operand* src)
{
    dst->type = src->type;
    switch(src->type) {
    case OT_Imm8:
        assert(src->val < (1l<<8));
        // fall-trough
    case OT_Imm16:
        assert(src->val < (1l<<16));
        // fall-trough
    case OT_Imm32:
        assert(src->val < (1l<<32));
        // fall-trough
    case OT_Imm64:
        dst->val = src->val;
        break;
    case OT_Reg32:
    case OT_Reg64:
    case OT_Reg128:
    case OT_Reg256:
        dst->reg = src->reg;
        break;
    case OT_Ind32:
    case OT_Ind64:
    case OT_Ind128:
    case OT_Ind256:
        assert( (src->reg == Reg_None) ||
                (src->reg == Reg_IP) ||
                ((src->reg >= Reg_AX) && (src->reg <= Reg_15)) );
        dst->reg = src->reg;
        dst->val = src->val;
        dst->scale = src->scale;
        if (src->scale >0) {
            assert((src->scale == 1) || (src->scale == 2) ||
                   (src->scale == 4) || (src->scale == 8));
            assert((src->ireg >= Reg_AX) && (src->ireg <= Reg_15));
            dst->ireg = src->ireg;
        }
        break;
    default: assert(0);
    }
}

void copyInstr(Instr* dst, Instr* src)
{
    dst->addr  = src->addr;
    dst->len   = src->len;
    dst->type  = src->type;
    dst->vtype = src->vtype;
    dst->form  = src->form;

    dst->dst.type = OT_None;
    dst->src.type = OT_None;
    dst->src2.type = OT_None;
    switch(src->form) {
    case OF_3:
        copyOperand(&(dst->src2), &(src->src2));
        // fall through
    case OF_2:
        copyOperand(&(dst->src), &(src->src));
        // fall through
    case OF_1:
        copyOperand(&(dst->dst), &(src->dst));
        // fall through
    case OF_0:
        break;
    default: assert(0);
    }

    dst->ptLen = src->ptLen;
    if (src->ptLen > 0) {
        dst->ptPSet = src->ptPSet;
        dst->ptEnc  = src->ptEnc;
        for(int j=0; j < src->ptLen; j++)
            dst->ptOpc[j] = src->ptOpc[j];
    }
}

void initSimpleInstr(Instr* i, InstrType it)
{
    i->addr = 0; // unknown: created, not parsed
    i->len = 0;

    i->type = it;
    i->ptLen = 0; // no pass-through info
    i->vtype = VT_None;
    i->form = OF_0;
    i->dst.type = OT_None;
    i->src.type = OT_None;
    i->src2.type = OT_None;
}

void initUnaryInstr(Instr* i, InstrType it, Operand* o)
{
    initSimpleInstr(i, it);
    i->form = OF_1;
    copyOperand( &(i->dst), o);
}

void initBinaryInstr(Instr* i, InstrType it, ValType vt,
                     Operand *o1, Operand *o2)
{
    if (vt != VT_None) {
        // if we specify a value type, it must match destination
        assert(vt == opValType(o1));
        // if 2nd operand is other than immediate, types also must match
        if (!opIsImm(o2))
            assert(vt == opValType(o2));
    }

    initSimpleInstr(i, it);
    i->form = OF_2;
    i->vtype = vt;
    copyOperand( &(i->dst), o1);
    copyOperand( &(i->src), o2);
}

void initTernaryInstr(Instr* i, InstrType it,
                      Operand *o1, Operand *o2, Operand* o3)
{
    initSimpleInstr(i, it);
    i->form = OF_3;
    copyOperand( &(i->dst), o1);
    copyOperand( &(i->src), o2);
    copyOperand( &(i->src2), o3);
}


void attachPassthrough(Instr* i, PrefixSet set, OperandEncoding enc,
                       int b1, int b2, int b3)
{
    assert(i->ptLen == 0);
    i->ptEnc = enc;
    i->ptPSet = set;
    assert(b1>=0);
    i->ptLen++;
    i->ptOpc[0] = (unsigned char) b1;
    if (b2 < 0) return;
    i->ptLen++;
    i->ptOpc[1] = (unsigned char) b2;
    if (b3 < 0) return;
    i->ptLen++;
    i->ptOpc[2] = (unsigned char) b3;
}

Instr* nextInstr(Rewriter* c, uint64_t a, int len)
{
    Instr* i = c->decInstr + c->decInstrCount;
    assert(c->decInstrCount < c->decInstrCapacity);
    c->decInstrCount++;

    i->addr = a;
    i->len = len;

    i->ptLen = 0;
    i->vtype = VT_None;
    i->form = OF_None;
    i->dst.type = OT_None;
    i->src.type = OT_None;
    i->src2.type = OT_None;

    return i;
}

Instr* addSimple(Rewriter* c, uint64_t a, uint64_t a2, InstrType it)
{
    Instr* i = nextInstr(c, a, a2 - a);
    i->type = it;
    i->form = OF_0;

    return i;
}

Instr* addSimpleVType(Rewriter* c, uint64_t a, uint64_t a2, InstrType it, ValType vt)
{
    Instr* i = nextInstr(c, a, a2 - a);
    i->type = it;
    i->vtype = vt;
    i->form = OF_0;

    return i;
}

Instr* addUnaryOp(Rewriter* c, uint64_t a, uint64_t a2,
                  InstrType it, Operand* o)
{
    Instr* i = nextInstr(c, a, a2 - a);
    i->type = it;
    i->form = OF_1;
    copyOperand( &(i->dst), o);

    return i;
}

Instr* addBinaryOp(Rewriter* c, uint64_t a, uint64_t a2,
                   InstrType it, ValType vt, Operand* o1, Operand* o2)
{
    if (vt != VT_None) {
        // if we specify a value type, it must match destination
        assert(vt == opValType(o1));
        // if 2nd operand is other than immediate, types also must match
        if (!opIsImm(o2))
            assert(vt == opValType(o2));
    }

    Instr* i = nextInstr(c, a, a2 - a);
    i->type = it;
    i->form = OF_2;
    i->vtype = vt;
    copyOperand( &(i->dst), o1);
    copyOperand( &(i->src), o2);

    return i;
}

Instr* addTernaryOp(Rewriter* c, uint64_t a, uint64_t a2,
                    InstrType it, Operand* o1, Operand* o2, Operand* o3)
{
    Instr* i = nextInstr(c, a, a2 - a);
    i->type = it;
    i->form = OF_3;
    copyOperand( &(i->dst), o1);
    copyOperand( &(i->src), o2);
    copyOperand( &(i->src2), o3);

    return i;
}


// r/m, r. r parsed as op2/reg and digit. Encoding see SDM 2.1
// Input: o1 or o2 may be vector registers, type may be known (or VT_None)
// Fills o1/o2/digit and returns number of bytes parsed
int parseModRM(uint8_t* p, int rex, Bool o1IsVec, Bool o2IsVec, ValType vt,
               Operand* o1, Operand* o2, int* digit)
{
    int modrm, mod, rm, reg; // modRM byte
    int sib, scale, idx, base; // SIB byte
    int64_t disp;
    Reg r;
    OpType ot;
    int o = 0;
    int hasRex = (rex>0);
    int hasDisp8 = 0, hasDisp32 = 0;

    modrm = p[o++];
    mod = (modrm & 192) >> 6;
    reg = (modrm & 56) >> 3;
    rm = modrm & 7;

    ot = (hasRex && (rex & REX_MASK_W)) ? OT_Reg64 : OT_Reg32;
    switch(vt) {
    case VT_32:  ot = OT_Reg32; break;
    case VT_64:  ot = OT_Reg64; break;
    case VT_128: ot = OT_Reg128; break;
    case VT_256: ot = OT_Reg256; break;
    }

    // r part: reg or digit, give both back to caller
    if (digit) *digit = reg;
    if (o2) {
        r = (o2IsVec ? Reg_X0 : Reg_AX) + reg;
        if (hasRex && (rex & REX_MASK_R)) r += 8;
        o2->type = ot;
        o2->reg = r;
    }

    if (mod == 3) {
        // r, r
        r = (o1IsVec ? Reg_X0 : Reg_AX) + rm;
        if (hasRex && (rex & REX_MASK_B)) r += 8;
        o1->type = ot;
        o1->reg = r;
        return o;
    }

    if (mod == 1) hasDisp8 = 1;
    if (mod == 2) hasDisp32 = 1;
    if ((mod == 0) && (rm == 5)) {
        // mod 0 + rm 5: RIP relative
        hasDisp32 = 1;
    }

    scale = 0;
    if (rm == 4) {
        // SIB
        sib = p[o++];
        scale = 1 << ((sib & 192) >> 6);
        idx   = (sib & 56) >> 3;
        base  = sib & 7;
        if ((base == 5) && (mod == 0))
            hasDisp32 = 1;
    }

    disp = 0;
    if (hasDisp8) {
        // 8bit disp: sign extend
        disp = *((signed char*) (p+o));
        o++;
    }
    if (hasDisp32) {
        disp = *((int32_t*) (p+o));
        o += 4;
    }

    ot = (hasRex && (rex & REX_MASK_W)) ? OT_Ind64 : OT_Ind32;
    switch(vt) {
    case VT_32:  ot = OT_Ind32; break;
    case VT_64:  ot = OT_Ind64; break;
    case VT_128: ot = OT_Ind128; break;
    case VT_256: ot = OT_Ind256; break;
    }

    o1->type = ot;
    o1->scale = scale;
    o1->val = (uint64_t) disp;
    if (scale == 0) {
        r = Reg_AX + rm;
        if (hasRex && (rex & REX_MASK_B)) r += 8;
        o1->reg = ((mod == 0) && (rm == 5)) ? Reg_IP : r;
        return o;
    }

    r = Reg_AX + idx;
    if (hasRex && (rex & REX_MASK_X)) r += 8;
    o1->ireg = (idx == 4) ? Reg_None : r;


    r = Reg_AX + base;
    if (hasRex && (rex & REX_MASK_B)) r += 8;
    o1->reg = ((base == 5) && (mod == 0)) ? Reg_None : r;

    // no need to use SIB if index register not used
    if (o1->ireg == Reg_None) o1->scale = 0;

    return o;
}

// decode the basic block starting at f (automatically triggered by emulator)
DBB* decodeBB(Rewriter* c, uint64_t f)
{
    Bool hasRex, hasF2, hasF3, has66;
    int rex;
    uint64_t a;
    int i, off, opc, opc2, digit, old_icount;
    Bool exitLoop;
    uint8_t* fp;
    Operand o1, o2, o3;
    ValType vt;
    InstrType it;
    Instr* ii;
    DBB* dbb;

    if (f == 0) return 0; // nothing to decode

    // already decoded?
    for(i = 0; i < c->decBBCount; i++)
        if (c->decBB[i].addr == f) return &(c->decBB[i]);

    if (c->showDecoding)
        printf("Decoding BB %lx ...\n", f);

    // start decoding of new BB beginning at f
    assert(c->decBBCount < c->decBBCapacity);
    dbb = &(c->decBB[c->decBBCount]);
    c->decBBCount++;
    dbb->addr = f;
    dbb->count = 0;
    dbb->instr = c->decInstr + c->decInstrCount;
    old_icount = c->decInstrCount;

    fp = (uint8_t*) f;
    off = 0;
    hasRex = False;
    rex = 0;
    hasF2 = False;
    hasF3 = False;
    has66 = False;
    exitLoop = False;
    while(!exitLoop) {
        a = (uint64_t)(fp + off);

        // prefixes
        while(1) {
            if ((fp[off] >= 0x40) && (fp[off] <= 0x4F)) {
                rex = fp[off] & 15;
                hasRex = True;
                off++;
                continue;
            }
            if (fp[off] == 0xF2) {
                hasF2 = True;
                off++;
                continue;
            }
            if (fp[off] == 0xF3) {
                hasF3 = True;
                off++;
                continue;
            }
            if (fp[off] == 0x66) {
                has66 = True;
                off++;
                continue;
            }
            // no further prefixes
            break;
        }

        opc = fp[off++];
        switch(opc) {

        case 0x01:
            // add r/m,r 32/64 (MR, dst: r/m, src: r)
            vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, &o2, 0);
            addBinaryOp(c, a, (uint64_t)(fp + off), IT_ADD, vt, &o1, &o2);
            break;

        case 0x03:
            // add r,r/m 32/64 (RM, dst: r, src: r/m)
            vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o2, &o1, 0);
            addBinaryOp(c, a, (uint64_t)(fp + off), IT_ADD, vt, &o1, &o2);
            break;

        case 0x0F:
            opc2 = fp[off++];
            switch(opc2) {
            case 0xAF:
                // imul r 32/64, r/m 32/64 (RM, dst: r)
                vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
                off += parseModRM(fp+off, rex, 0, 0, VT_None, &o2, &o1, 0);
                addBinaryOp(c, a, (uint64_t)(fp + off), IT_IMUL, vt, &o1, &o2);
                break;

            case 0x10:
                assert(hasF2);
                // movsd xmm2,xmm1/m64 (RM)
                off += parseModRM(fp+off, rex, 1, 1, VT_64, &o2, &o1, 0);
                ii = addBinaryOp(c, a, (uint64_t)(fp + off),
                                 IT_MOVSD, VT_64, &o1, &o2);
                attachPassthrough(ii, PS_F2, OE_RM, 0x0F, 0x10, -1);
                break;

            case 0x11:
                assert(hasF2);
                // movsd xmm2/m64,xmm1 (MR)
                off += parseModRM(fp+off, rex, 1, 1, VT_64, &o1, &o2, 0);
                ii = addBinaryOp(c, a, (uint64_t)(fp + off),
                                 IT_MOVSD, VT_64, &o1, &o2);
                attachPassthrough(ii, PS_F2, OE_MR, 0x0F, 0x11, -1);
                break;

            case 0x1F:
                off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, 0, &digit);
                switch(digit) {
                case 0:
                    // 0F 1F /0: nop r/m 32
                    addUnaryOp(c, a, (uint64_t)(fp + off), IT_NOP, &o1);
                    break;

                default:
                    addSimple(c, a, (uint64_t)(fp + off), IT_Invalid);
                    break;
                }
                break;

            case 0x2E:
                assert(has66);
                // ucomisd xmm1,xmm2/m64 (RM)
                off += parseModRM(fp+off, rex, 1, 1, VT_64, &o2, &o1, 0);
                ii = addBinaryOp(c, a, (uint64_t)(fp + off),
                                 IT_UCOMISD, VT_64, &o1, &o2);
                attachPassthrough(ii, PS_66, OE_RM, 0x0F, 0x2E, -1);
                break;

            case 0x58:
                assert(hasF2);
                // addsd xmm1,xmm2/m64 (RM)
                off += parseModRM(fp+off, rex, 1, 1, VT_64, &o2, &o1, 0);
                ii = addBinaryOp(c, a, (uint64_t)(fp + off),
                                 IT_ADDSD, VT_64, &o1, &o2);
                attachPassthrough(ii, PS_F2, OE_RM, 0x0F, 0x58, -1);
                break;

            case 0x59:
                assert(hasF2);
                // mulsd xmm1,xmm2/m64 (RM)
                off += parseModRM(fp+off, rex, 1, 1, VT_64, &o2, &o1, 0);
                ii = addBinaryOp(c, a, (uint64_t)(fp + off),
                                 IT_MULSD, VT_64, &o1, &o2);
                attachPassthrough(ii, PS_F2, OE_RM, 0x0F, 0x59, -1);
                break;

            case 0x5C:
                assert(hasF2);
                // subsd xmm1,xmm2/m64 (RM)
                off += parseModRM(fp+off, rex, 1, 1, VT_64, &o2, &o1, 0);
                ii = addBinaryOp(c, a, (uint64_t)(fp + off),
                                 IT_SUBSD, VT_64, &o1, &o2);
                attachPassthrough(ii, PS_F2, OE_RM, 0x0F, 0x5C, -1);
                break;

            case 0x84: // JE/JZ rel32
            case 0x85: // JNE/JNZ rel32
            case 0x8A: // JP rel32
            case 0x8E: // JLE/JNG rel32
            case 0x8F: // JG/JNLE rel32
                o1.type = OT_Imm64;
                o1.val = (uint64_t) (fp + off + 4 + *(int32_t*)(fp + off));
                off += 4;
                if      (opc2 == 0x84) it = IT_JE;
                else if (opc2 == 0x85) it = IT_JNE;
                else if (opc2 == 0x8A) it = IT_JP;
                else if (opc2 == 0x8E) it = IT_JLE;
                else if (opc2 == 0x8F) it = IT_JG;
                else assert(0);
                addUnaryOp(c, a, (uint64_t)(fp + off), it, &o1);
                exitLoop = True;
                break;

            case 0x7E:
                assert(has66);
                // movd/q xmm,r/m 32/64 (RM)
                vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
                off += parseModRM(fp+off, rex, 1, 0, vt, &o2, &o1, 0);
                ii = addBinaryOp(c, a, (uint64_t)(fp + off),
                                 IT_MOV, vt, &o1, &o2);
                attachPassthrough(ii, PS_66, OE_RM, 0x0F, 0x7E, -1);
                break;

            case 0xEF:
                // pxor xmm1, xmm2/m 64/128 (RM)
                vt = has66 ? VT_128 : VT_64;
                off += parseModRM(fp+off, rex, 1, 1, vt, &o2, &o1, 0);
                ii = addBinaryOp(c, a, (uint64_t)(fp + off),
                                 IT_PXOR, vt, &o1, &o2);
                attachPassthrough(ii, has66 ? PS_66 : 0, OE_RM, 0x0F, 0xEF, -1);
                break;

            default:
                addSimple(c, a, (uint64_t)(fp + off), IT_Invalid);
                break;
            }
            break;

        case 0x31:
            // xor r/m,r 32/64 (MR, dst: r/m, src: r)
            vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, &o2, 0);
            addBinaryOp(c, a, (uint64_t)(fp + off), IT_XOR, vt, &o1, &o2);
            break;

        case 0x39:
            // cmp r/m,r 32/64 (MR)
            vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, &o2, 0);
            addBinaryOp(c, a, (uint64_t)(fp + off), IT_CMP, vt, &o1, &o2);
            break;

        case 0x3B:
            // cmp r,r/m 32/64 (RM)
            vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o2, &o1, 0);
            addBinaryOp(c, a, (uint64_t)(fp + off), IT_CMP, vt, &o1, &o2);
            break;


        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            // push
            addUnaryOp(c, a, (uint64_t)(fp + off),
                       IT_PUSH, getRegOp(VT_64, Reg_AX+(opc-0x50)));
            break;

        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            // pop
            addUnaryOp(c, a, (uint64_t)(fp + off),
                       IT_POP, getRegOp(VT_64, Reg_AX+(opc-0x58)));
            break;

        case 0x63:
            // movsx r64,r/m32 (RM) mov with sign extension
            assert(rex & REX_MASK_W);
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o2, &o1, 0);
            // src is 32 bit
            switch(o2.type) {
            case OT_Reg64: o2.type = OT_Reg32; break;
            case OT_Ind64: o2.type = OT_Ind32; break;
            default: assert(0);
            }
            addBinaryOp(c, a, (uint64_t)(fp + off),
                        IT_MOVSX, VT_None, &o1, &o2);
            break;

        case 0x69:
            // imul r,r/m32/64,imm32 (RMI)
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o2, &o1, 0);
            o3.type = OT_Imm32;
            o3.val = *(uint32_t*)(fp + off);
            off += 4;
            addTernaryOp(c, a, (uint64_t)(fp + off), IT_IMUL, &o1, &o2, &o3);
            break;

        case 0x6B:
            // imul r,r/m32/64,imm8 (RMI)
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o2, &o1, 0);
            o3.type = OT_Imm8;
            o3.val = *(uint8_t*)(fp + off);
            off += 1;
            addTernaryOp(c, a, (uint64_t)(fp + off), IT_IMUL, &o1, &o2, &o3);
            break;


        case 0x74: // JE/JZ rel8
        case 0x75: // JNE/JNZ rel8
        case 0x7A: // JP rel8
        case 0x7E: // JLE/JNG rel8
        case 0x7F: // JG/JNLE rel8
            o1.type = OT_Imm64;
            o1.val = (uint64_t) (fp + off + 1 + *(int8_t*)(fp + off));
            off += 1;
            if      (opc == 0x74) it = IT_JE;
            else if (opc == 0x75) it = IT_JNE;
            else if (opc == 0x7A) it = IT_JP;
            else if (opc == 0x7E) it = IT_JLE;
            else if (opc == 0x7F) it = IT_JG;
            else assert(0);
            addUnaryOp(c, a, (uint64_t)(fp + off), it, &o1);
            exitLoop = True;
            break;

        case 0x81:
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, 0, &digit);
            switch(digit) {
            case 0:
                // 81/0: add r/m 32/64, imm32
                vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
                o2.type = OT_Imm32;
                o2.val = *(uint32_t*)(fp + off);
                off += 4;
                addBinaryOp(c, a, (uint64_t)(fp + off),
                            IT_ADD, vt, &o1, &o2);
                break;

            default:
                addSimple(c, a, (uint64_t)(fp + off), IT_Invalid);
                break;
            }
            break;

        case 0x83:
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, 0, &digit);
            switch(digit) {
            case 0:
                // 83/0: ADD r/m 32/64, imm8: Add sign-extended imm8 to r/m
                vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
                o2.type = OT_Imm8;
                o2.val = (uint8_t) (*(int8_t*)(fp + off));
                off += 1;
                addBinaryOp(c, a, (uint64_t)(fp + off), IT_ADD, vt, &o1, &o2);
                break;

            case 5:
                // 83/5: SUB r/m 32/64, imm8: Subtract sign-extended imm8 from r/m
                vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
                o2.type = OT_Imm8;
                o2.val = (int64_t) (*(int8_t*)(fp + off));
                off += 1;
                addBinaryOp(c, a, (uint64_t)(fp + off), IT_SUB, vt, &o1, &o2);
                break;

            case 7:
                // 83/7: CMP r/m 32/64, imm8 (MI)
                vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
                o2.type = OT_Imm8;
                o2.val = (int64_t) (*(int8_t*)(fp + off));
                off += 1;
                addBinaryOp(c, a, (uint64_t)(fp + off), IT_CMP, vt, &o1, &o2);
                break;

            default:
                addSimple(c, a, (uint64_t)(fp + off), IT_Invalid);
                break;
            }
            break;

        case 0x85:
            // test r/m,r 32/64 (dst: r/m, src: r)
            vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, &o2, 0);
            addBinaryOp(c, a, (uint64_t)(fp + off), IT_TEST, vt, &o1, &o2);
            break;

        case 0x89:
            // mov r/m,r 32/64 (dst: r/m, src: r)
            vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, &o2, 0);
            addBinaryOp(c, a, (uint64_t)(fp + off), IT_MOV, vt, &o1, &o2);
            break;

        case 0x8B:
            // mov r,r/m 32/64 (dst: r, src: r/m)
            vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o2, &o1, 0);
            addBinaryOp(c, a, (uint64_t)(fp + off), IT_MOV, vt, &o1, &o2);
            break;

        case 0x8D:
            // lea r32/64,m
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o2, &o1, 0);
            assert(opIsInd(&o2)); // TODO: bad code error
            addBinaryOp(c, a, (uint64_t)(fp + off),
                        IT_LEA, VT_None, &o1, &o2);
            break;

        case 0x90:
            // nop
            addSimple(c, a, (uint64_t)(fp + off), IT_NOP);
            break;

        case 0x98:
            // cltq (Intel: cdqe - sign-extend eax to rax)
            addSimpleVType(c, a, (uint64_t)(fp + off), IT_CLTQ,
                           hasRex && (rex & REX_MASK_W) ? VT_64 : VT_32);
            break;

        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            // MOV r32/64,imm32/64
            o1.reg = Reg_AX + (opc - 0xB8);
            if (rex & REX_MASK_R) o1.reg += 8;
            if (rex & REX_MASK_W) {
                vt = VT_64;
                o1.type = OT_Reg64;
                o2.type = OT_Imm64;
                o2.val = *(uint64_t*)(fp + off);
                off += 8;
            }
            else {
                vt = VT_32;
                o1.type = OT_Reg32;
                o2.type = OT_Imm32;
                o2.val = *(uint32_t*)(fp + off);
                off += 4;
            }
            addBinaryOp(c, a, (uint64_t)(fp + off), IT_MOV, vt, &o1, &o2);
            break;

        case 0xC1:
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, 0, &digit);
            switch(digit) {
            case 4:
                // shl r/m 32/64,imm8 (MI) (= sal)
                o2.type = OT_Imm8;
                o2.val = *(uint8_t*)(fp + off);
                off += 1;
                addBinaryOp(c, a, (uint64_t)(fp + off),
                            IT_SHL, VT_None, &o1, &o2);
                break;

            default:
                addSimple(c, a, (uint64_t)(fp + off), IT_Invalid);
                break;
            }
            break;

        case 0xC3:
            // ret
            addSimple(c, a, (uint64_t)(fp + off), IT_RET);
            exitLoop = True;
            break;

        case 0xC7:
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, 0, &digit);
            switch(digit) {
            case 0:
                // mov r/m 32/64, imm32
                vt = (rex & REX_MASK_W) ? VT_64 : VT_32;
                o2.type = OT_Imm32;
                o2.val = *(uint32_t*)(fp + off);
                off += 4;
                addBinaryOp(c, a, (uint64_t)(fp + off), IT_MOV, vt, &o1, &o2);
                break;

            default:
                addSimple(c, a, (uint64_t)(fp + off), IT_Invalid);
                break;
            }
            break;

        case 0xC9:
            // leave ( = mov rbp,rsp + pop rbp)
            addSimple(c, a, (uint64_t)(fp + off), IT_LEAVE);
            break;

        case 0xE8:
            // call rel32
            o1.type = OT_Imm64;
            o1.val = (uint64_t) (fp + off + 4 + *(int32_t*)(fp + off));
            off += 4;
            addUnaryOp(c, a, (uint64_t)(fp + off), IT_CALL, &o1);
            exitLoop = True;
            break;

        case 0xE9:
            // jmp rel32
            o1.type = OT_Imm64;
            o1.val = (uint64_t) (fp + off + 4 + *(int32_t*)(fp + off));
            off += 4;
            addUnaryOp(c, a, (uint64_t)(fp + off), IT_JMP, &o1);
            exitLoop = True;
            break;

        case 0xEB:
            // jmp rel8
            o1.type = OT_Imm64;
            o1.val = (uint64_t) (fp + off + 1 + *(int8_t*)(fp + off));
            off += 1;
            addUnaryOp(c, a, (uint64_t)(fp + off), IT_JMP, &o1);
            exitLoop = True;
            break;

        case 0xF7:
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, 0, &digit);
            switch(digit) {
            case 3:
                // neg r/m 32/64
                addUnaryOp(c, a, (uint64_t)(fp + off), IT_NEG, &o1);
                break;

            default:
                addSimple(c, a, (uint64_t)(fp + off), IT_Invalid);
                break;
            }
            break;

        case 0xFF:
            off += parseModRM(fp+off, rex, 0, 0, VT_None, &o1, 0, &digit);
            switch(digit) {
            case 2:
                // call r/m64
                addUnaryOp(c, a, (uint64_t)(fp + off), IT_CALL, &o1);
                exitLoop = True;
                break;

            default:
                addSimple(c, a, (uint64_t)(fp + off), IT_Invalid);
                break;
            }
            break;

        default:
            addSimple(c, a, (uint64_t)(fp + off), IT_Invalid);
            break;
        }
        hasRex = False;
        rex = 0;
        hasF2 = False;
        hasF3 = False;
        has66 = False;
    }

    assert(dbb->addr == dbb->instr->addr);
    dbb->count = c->decInstrCount - old_icount;

    if (c->showDecoding)
        printDecodedBB(dbb);

    return dbb;
}

/*------------------------------------------------------------*/
/* x86_64 printer
 */

char* regName(Reg r, OpType t)
{
    switch(t) {
    case OT_Reg32:
        switch(r) {
        case Reg_AX: return "eax";
        case Reg_BX: return "ebx";
        case Reg_CX: return "ecx";
        case Reg_DX: return "edx";
        case Reg_DI: return "edi";
        case Reg_SI: return "esi";
        case Reg_BP: return "ebp";
        case Reg_SP: return "esp";
        case Reg_8:  return "r8";
        case Reg_9:  return "r9";
        case Reg_10: return "r10";
        case Reg_11: return "r11";
        case Reg_12: return "r12";
        case Reg_13: return "r13";
        case Reg_14: return "r14";
        case Reg_15: return "r15";
        case Reg_IP: return "eip";
        }
        break;

    case OT_Reg64:
        switch(r) {
        case Reg_AX: return "rax";
        case Reg_BX: return "rbx";
        case Reg_CX: return "rcx";
        case Reg_DX: return "rdx";
        case Reg_DI: return "rdi";
        case Reg_SI: return "rsi";
        case Reg_BP: return "rbp";
        case Reg_SP: return "rsp";
        case Reg_8:  return "r8";
        case Reg_9:  return "r9";
        case Reg_10: return "r10";
        case Reg_11: return "r11";
        case Reg_12: return "r12";
        case Reg_13: return "r13";
        case Reg_14: return "r14";
        case Reg_15: return "r15";
        case Reg_IP: return "rip";

        case Reg_X0:  return "mm0";
        case Reg_X1:  return "mm1";
        case Reg_X2:  return "mm2";
        case Reg_X3:  return "mm3";
        case Reg_X4:  return "mm4";
        case Reg_X5:  return "mm5";
        case Reg_X6:  return "mm6";
        case Reg_X7:  return "mm7";
        case Reg_X8:  return "mm8";
        case Reg_X9:  return "mm9";
        case Reg_X10: return "mm10";
        case Reg_X11: return "mm11";
        case Reg_X12: return "mm12";
        case Reg_X13: return "mm13";
        case Reg_X14: return "mm14";
        case Reg_X15: return "mm15";
        }
        break;

    case OT_Reg128:
        switch(r) {
        case Reg_X0:  return "xmm0";
        case Reg_X1:  return "xmm1";
        case Reg_X2:  return "xmm2";
        case Reg_X3:  return "xmm3";
        case Reg_X4:  return "xmm4";
        case Reg_X5:  return "xmm5";
        case Reg_X6:  return "xmm6";
        case Reg_X7:  return "xmm7";
        case Reg_X8:  return "xmm8";
        case Reg_X9:  return "xmm9";
        case Reg_X10: return "xmm10";
        case Reg_X11: return "xmm11";
        case Reg_X12: return "xmm12";
        case Reg_X13: return "xmm13";
        case Reg_X14: return "xmm14";
        case Reg_X15: return "xmm15";
        }
        break;

    case OT_Reg256:
        switch(r) {
        case Reg_X0:  return "ymm0";
        case Reg_X1:  return "ymm1";
        case Reg_X2:  return "ymm2";
        case Reg_X3:  return "ymm3";
        case Reg_X4:  return "ymm4";
        case Reg_X5:  return "ymm5";
        case Reg_X6:  return "ymm6";
        case Reg_X7:  return "ymm7";
        case Reg_X8:  return "ymm8";
        case Reg_X9:  return "ymm9";
        case Reg_X10: return "ymm10";
        case Reg_X11: return "ymm11";
        case Reg_X12: return "ymm12";
        case Reg_X13: return "ymm13";
        case Reg_X14: return "ymm14";
        case Reg_X15: return "ymm15";
        }
        break;

    }
    assert(0);
}

char* op2string(Operand* o, ValType t)
{
    static char buf[30];
    int off = 0;
    uint64_t val;

    switch(o->type) {
    case OT_Reg32:
    case OT_Reg64:
    case OT_Reg128:
    case OT_Reg256:
        sprintf(buf, "%%%s", regName(o->reg, o->type));
        break;

    case OT_Imm8:
        val = o->val;
        assert(val < (1l<<8));
        switch(t) {
        case VT_None:
        case VT_8:
            break;
        case VT_32:
            if (val > 0x7F) val += 0xFFFFFF00;
            break;
        case VT_64:
            if (val > 0x7F) val += 0xFFFFFFFFFFFFFF00;
            break;
        default: assert(0);
        }
        sprintf(buf, "$0x%lx", val);
        break;

    case OT_Imm16:
        val = o->val;
        assert(val < (1l<<16));
        switch(t) {
        case VT_32:
            if (val > 0x7FFF) val += 0xFFFF0000;
            break;
        case VT_64:
            if (val > 0x7FFF) val += 0xFFFFFFFFFFFF0000;
            break;
        case VT_None:
            break;
        default: assert(0);
        }
        sprintf(buf, "$0x%lx", val);
        break;

    case OT_Imm32:
        val = o->val;
        assert(val < (1l<<32));
        switch(t) {
        case VT_None:
        case VT_32:
            break;
        case VT_64:
            if (val > 0x7FFFFFFF) val += 0xFFFFFFFF00000000;
            break;
        default: assert(0);
        }
        sprintf(buf, "$0x%lx", val);
        break;

    case OT_Imm64:
        sprintf(buf, "$0x%lx", o->val);
        break;

    case OT_Ind8:
    case OT_Ind16:
    case OT_Ind32:
    case OT_Ind64:
    case OT_Ind128:
    case OT_Ind256:
        if (o->val != 0) {
            if (o->val & (1l<<63))
                off = sprintf(buf, "-0x%lx", (~ o->val)+1);
            else
                off = sprintf(buf, "0x%lx", o->val);
        }
        if ((o->scale == 0) || (o->ireg == Reg_None)) {
            if (o->reg != Reg_None)
                sprintf(buf+off,"(%%%s)", regName(o->reg, OT_Reg64));
        }
        else {
            char* ri = regName(o->ireg, OT_Reg64);
            if (o->reg == Reg_None) {
                sprintf(buf+off,"(,%%%s,%d)", ri, o->scale);
            }
            else
                sprintf(buf+off,"(%%%s,%%%s,%d)",
                        regName(o->reg, OT_Reg64), ri, o->scale);
        }
        break;
    default: assert(0);
    }
    return buf;
}

char* instr2string(Instr* instr, int align)
{
    static char buf[100];
    char* n = "<Invalid>";
    int oc = 0, off = 0;

    switch(instr->type) {
    case IT_NOP:     n = "nop"; break;
    case IT_RET:     n = "ret"; break;
    case IT_LEAVE:   n = "leave"; break;
    case IT_CLTQ:    n = "clt"; break;
    case IT_PUSH:    n = "push"; oc = 1; break;
    case IT_POP:     n = "pop";  oc = 1; break;
    case IT_CALL:    n = "call"; oc = 1; break;
    case IT_JMP:     n = "jmp";  oc = 1; break;
    case IT_JE:      n = "je";   oc = 1; break;
    case IT_JNE:     n = "jne";  oc = 1; break;
    case IT_JLE:     n = "jle";  oc = 1; break;
    case IT_JG:      n = "jg";   oc = 1; break;
    case IT_JP:      n = "jp";   oc = 1; break;
    case IT_MOV:     n = "mov";  oc = 2; break;
    case IT_MOVSX:   n = "movsx";  oc = 2; break;
    case IT_NEG:     n = "neg";  oc = 1; break;
    case IT_ADD:     n = "add";  oc = 2; break;
    case IT_SUB:     n = "sub";  oc = 2; break;
    case IT_IMUL:    n = "imul"; oc = 2; break;
    case IT_AND:     n = "xor";  oc = 2; break;
    case IT_OR:      n = "xor";  oc = 2; break;
    case IT_XOR:     n = "xor";  oc = 2; break;
    case IT_SHL:     n = "shl";  oc = 2; break;
    case IT_SHR:     n = "shr";  oc = 2; break;
    case IT_LEA:     n = "lea";  oc = 2; break;
    case IT_CMP:     n = "cmp";  oc = 2; break;
    case IT_TEST:    n = "test"; oc = 2; break;
    case IT_PXOR:    n = "pxor"; oc = 2; break;
    case IT_MOVSD:   n = "movsd"; oc = 2; break;
    case IT_UCOMISD: n = "ucomisd"; oc = 2; break;
    case IT_MULSD:   n = "mulsd"; oc = 2; break;
    case IT_ADDSD:   n = "addsd"; oc = 2; break;
    case IT_SUBSD:   n = "subsd"; oc = 2; break;
    }

    if (align)
        off += sprintf(buf, "%-7s", n);
    else
        off += sprintf(buf, "%s", n);

    // add value type if given
    Bool appendVType = (instr->vtype != VT_None);
    if ((instr->form == OF_2) &&
        (opIsGPReg(&(instr->dst)) ||
         opIsGPReg(&(instr->src)))) appendVType = False;
    if (appendVType) {
        char vt = ' ';
        switch(instr->vtype) {
        case VT_8:  vt = 'b'; break;
        case VT_16: vt = 'w'; break;
        case VT_32: vt = 'l'; break;
        case VT_64: vt = 'q'; break;
        }
        if (vt != ' ') {
            int nlen = strlen(n);
            if (buf[nlen] == ' ') buf[nlen] = vt;
            else {
                buf[nlen] = vt;
                buf[nlen+1] = 0;
                off++;
            }
        }
    }

    switch(instr->form) {
    case OF_0:
        assert(instr->dst.type == OT_None);
        assert(instr->src.type == OT_None);
        assert(instr->src2.type == OT_None);
        break;

    case OF_1:
        assert(instr->dst.type != OT_None);
        assert(instr->src.type == OT_None);
        assert(instr->src2.type == OT_None);
        off += sprintf(buf+off, " %s", op2string(&(instr->dst), instr->vtype));
        break;

    case OF_2:
        assert(instr->dst.type != OT_None);
        assert(instr->src.type != OT_None);
        assert(instr->src2.type == OT_None);
        off += sprintf(buf+off, " %s", op2string(&(instr->src), instr->vtype));
        off += sprintf(buf+off, ",%s", op2string(&(instr->dst), instr->vtype));
        break;

    case OF_3:
        assert(instr->dst.type != OT_None);
        assert(instr->src.type != OT_None);
        assert(instr->src2.type != OT_None);
        off += sprintf(buf+off, " %s", op2string(&(instr->src2), instr->vtype));
        off += sprintf(buf+off, ",%s", op2string(&(instr->src), instr->vtype));
        off += sprintf(buf+off, ",%s", op2string(&(instr->dst), instr->vtype));
        break;

    default: assert(0);
    }

    return buf;
}

char* bytes2string(Instr* instr, int start, int count)
{
    static char buf[100];
    int off = 0, i, j;
    for(i = start, j=0; i < instr->len && j<count; i++, j++) {
        uint8_t b = ((uint8_t*) instr->addr)[i];
        off += sprintf(buf+off, " %02x", b);
    }
    for(;j<count;j++)
        off += sprintf(buf+off, "   ");
    return buf;
}

void printDecodedBB(DBB* bb)
{
    int i;
    for(i = 0; i < bb->count; i++) {
        Instr* instr = bb->instr + i;
        printf("  %p: %s  %s\n", (void*) instr->addr,
               bytes2string(instr, 0, 7), instr2string(instr, 1));
        if (instr->len > 7)
            printf("  %p: %s\n", (void*) instr->addr + 7,
                   bytes2string(instr, 7, 7));
        if (instr->len > 14)
            printf("  %p: %s\n", (void*) instr->addr + 14,
                   bytes2string(instr, 14, 7));
    }
}

void printDecodedBBs(Rewriter* c)
{
    int i;
    for(i=0; i< c->decBBCount; i++) {
        printf("BB %lx (%d instructions):\n", c->decBB[i].addr, c->decBB[i].count);
        printDecodedBB(c->decBB + i);
    }
}

/*------------------------------------------------------------*/
/* x86_64 code generation
 */

// generator helpers: return number of bytes written

int genRet(uint8_t* buf)
{
    buf[0] = 0xc3;
    return 1;
}

int genPush(uint8_t* buf, Operand* o)
{
    assert(o->type == OT_Reg64);
    assert((o->reg >= Reg_AX) && (o->reg <= Reg_DI));
    buf[0] = 0x50 + (o->reg - Reg_AX);
    return 1;
}

int genPop(uint8_t* buf, Operand* o)
{
    assert(o->type == OT_Reg64);
    assert((o->reg >= Reg_AX) && (o->reg <= Reg_DI));
    buf[0] = 0x58 + (o->reg - Reg_AX);
    return 1;
}

// return 0 - 15 for RAX - R15
int GPRegEncoding(Reg r)
{
    assert((r >= Reg_AX) && (r <= Reg_15));
    return r - Reg_AX;
}

// return 0 - 15 for XMM0 - XMM15
int VRegEncoding(Reg r)
{
    assert((r >= Reg_X0) && (r <= Reg_X15));
    return r - Reg_X0;
}

uint8_t* calcModRMDigit(Operand* o1, int digit, int* prex, int* plen)
{
    static uint8_t buf[10];
    int modrm, r1;
    int o = 0;
    ValType vt;

    assert((digit>=0) && (digit<8));
    assert(opIsReg(o1) || opIsInd(o1));

    vt = opValType(o1);
    if (vt == VT_64) *prex |= REX_MASK_W;

    modrm = (digit & 7) << 3;

    if (opIsReg(o1)) {
        // r,r: mod 3
        modrm |= 192;
        if (opIsGPReg(o1))
            r1 = GPRegEncoding(o1->reg);
        else if (opIsVReg(o1))
            r1 = VRegEncoding(o1->reg);
        else
            assert(0);
        if (r1 & 8) *prex |= REX_MASK_B;
        modrm |= (r1 & 7);
        buf[o++] = modrm;
    }
    else {
        int useDisp8 = 0, useDisp32 = 0, useSIB = 0;
        int sib = 0;
        int64_t v = (int64_t) o1->val;
        if (v != 0) {
            if ((v >= -128) && (v<128)) useDisp8 = 1;
            else if ((v >= -((int64_t)1<<31)) &&
                     (v < ((int64_t)1<<31))) useDisp32 = 1;
            else assert(0);

            if (useDisp8)
                modrm |= 64;
            if (useDisp32)
                modrm |= 128;
        }

        if (o1->scale == 0) {
            assert(o1->reg != Reg_SP); // rm 4 reserved for SIB encoding
            if (o1->reg == Reg_None) {
                useDisp32 = 1; // encoding needs disp32
                useDisp8 = 0;
                modrm &= 63; // mod needs to be 00
                useSIB = 1;
                sib = (4 << 3) + 5; // index 4 (= none) + base 5 (= none)
            }
            else {
                if (o1->reg == Reg_IP) {
                    // should not happen, we converted RIP-rel to absolute
                    assert(0);
                    // RIP relative
                    r1 = 5;
                    modrm &= 63;
                    useDisp32 = 1;
                }
                else {
                    r1 = GPRegEncoding(o1->reg);
                    assert((modrm >63) || (r1 != 5)); // do not use RIP encoding
                }
                if (r1 & 8) *prex |= REX_MASK_B;
                modrm |= (r1 & 7);
            }
        }
        else {
            // SIB
            int ri, rb;
            useSIB = 1;
            if      (o1->scale == 2) sib |= 64;
            else if (o1->scale == 4) sib |= 128;
            else if (o1->scale == 8) sib |= 192;
            else
                assert(o1->scale == 1);

            ri = GPRegEncoding(o1->ireg);
            if (ri & 8) *prex |= REX_MASK_X;
            sib |= (ri & 7) <<3;

            if (o1->reg == Reg_None) {
                // encoding requires disp32 with mod = 00 / base 5 = none
                useDisp32 = 1;
                useDisp8 = 0;
                modrm &= 63;
                sib |= 5;
            }
            else {
                if (o1->reg == Reg_BP) {
                    // cannot use mod == 00
                    if (modrm & 192 == 0) {
                        modrm |= 64;
                        useDisp8 = 1;
                    }
                }
                rb = GPRegEncoding(o1->reg);
                if (rb & 8) *prex |= REX_MASK_B;
                sib |= (rb & 7);
            }
        }

        if (useSIB)
            modrm |= 4; // signal SIB in modrm
        buf[o++] = modrm;
        if (useSIB)
            buf[o++] = sib;
        if (useDisp8)
            buf[o++] = (int8_t) v;
        if (useDisp32) {
            *(int32_t*)(buf+o) = (int32_t) v;
            o += 4;
        }
    }

    *plen = o;
    return buf;
}

uint8_t* calcModRM(Operand* o1, Operand* o2, int* prex, int* plen)
{
    int r2; // register offset encoding for operand 2

    assert(opValType(o1) == opValType(o2));

    if (opIsGPReg(o2)) {
        assert(opIsGPReg(o1) || opIsInd(o1));
        r2 = GPRegEncoding(o2->reg);
    }
    else if (opIsVReg(o2)) {
        assert(opIsVReg(o1) || opIsInd(o1));
        r2 = VRegEncoding(o2->reg);
    }
    else assert(0);

    if (r2 & 8) *prex |= REX_MASK_R;
    return calcModRMDigit(o1, r2 & 7, prex, plen);
}


// Operand o1: r/m, o2: r
int genModRM(uint8_t* buf, int opc, int opc2, Operand* o1, Operand* o2)
{
    int rex = 0, len = 0;
    int o = 0;
    uint8_t* rmBuf;

    rmBuf = calcModRM(o1, o2, &rex, &len);
    if (rex)
        buf[o++] = 0x40 | rex;
    buf[o++] = (uint8_t) opc;
    if (opc2 >=0)
        buf[o++] = (uint8_t) opc2;
    while(len>0) {
        buf[o++] = *rmBuf++;
        len--;
    }
    return o;
}

// Operand o1: r/m, o2: r, o3: imm
int genModRMI(uint8_t* buf, int opc, int opc2,
              Operand* o1, Operand* o2, Operand* o3)
{
    int rex = 0, len = 0;
    int o = 0;
    uint8_t* rmBuf;

    rmBuf = calcModRM(o1, o2, &rex, &len);
    if (rex)
        buf[o++] = 0x40 | rex;
    buf[o++] = (uint8_t) opc;
    if (opc2 >=0)
        buf[o++] = (uint8_t) opc2;
    while(len>0) {
        buf[o++] = *rmBuf++;
        len--;
    }
    assert(opIsImm(o3));
    switch(opValType(o3)) {
    case VT_8:
        buf[o++] = (uint8_t) o3->val;
        break;
    case VT_32:
        *(uint32_t*)(buf+o) = (uint32_t) o3->val;
        o += 4;
        break;
    default: assert(0);
    }

    return o;
}

// Operand o1: r/m, o2: imm
int genDigitMI(uint8_t* buf, int opc, int digit, Operand* o1, Operand* o2)
{
    int rex = 0, len = 0;
    int o = 0;
    uint8_t* rmBuf;

    assert(opIsImm(o2));
    rmBuf = calcModRMDigit(o1, digit, &rex, &len);
    if (rex)
        buf[o++] = 0x40 | rex;
    buf[o++] = (uint8_t) opc;
    while(len>0) {
        buf[o++] = *rmBuf++;
        len--;
    }

    // immediate
    switch(o2->type) {
    case OT_Imm8:
        *(uint8_t*)(buf + o) = (uint8_t) o2->val;
        o += 1;
        break;

    case OT_Imm32:
        *(uint32_t*)(buf + o) = (uint32_t) o2->val;
        o += 4;
        break;

    default: assert(0);
    }

    return o;
}

// Operand o1: r (gets part of opcode), o2: imm
int genOI(uint8_t* buf, int opc, Operand* o1, Operand* o2)
{
    int rex = 0, len = 0;
    int o = 0, r;

    assert(opIsReg(o1));
    assert(opIsImm(o2));

    r = GPRegEncoding(o1->reg);
    if (r & 8) rex |= REX_MASK_B;
    if (opValType(o1) == VT_64) rex |= REX_MASK_W;

    if (rex)
        buf[o++] = 0x40 | rex;
    buf[o++] = (uint8_t) (opc + (r & 7));

    // immediate
    switch(o2->type) {
    case OT_Imm8:
        *(uint8_t*)(buf + o) = (uint8_t) o2->val;
        o += 1;
        break;

    case OT_Imm32:
        *(uint32_t*)(buf + o) = (uint32_t) o2->val;
        o += 4;
        break;

    case OT_Imm64:
        *(uint64_t*)(buf + o) = o2->val;
        o += 8;
        break;

    default: assert(0);
    }

    return o;
}


int genMov(uint8_t* buf, Operand* src, Operand* dst)
{
    switch(dst->type) {
    case OT_Ind32:
    case OT_Ind64:
        // dst memory
        assert(opValType(src) == opValType(dst));
        switch(src->type) {
        case OT_Reg32:
        case OT_Reg64:
            // use 'mov r/m,r 32/64' (0x89)
            return genModRM(buf, 0x89, -1, dst, src);

        case OT_Imm32:
            // use 'mov r/m 32/64, imm 32' (0xC7/0)
            return genDigitMI(buf, 0xC7, 0, dst, src);

        default: assert(0);
        }
        break;

    case OT_Reg32:
    case OT_Reg64:
        // dst reg
        switch(src->type) {
        case OT_Ind32:
        case OT_Ind64:
        case OT_Reg32:
        case OT_Reg64:
            if (opValType(src) == opValType(dst)) {
                // use 'mov r,r/m 32/64' (0x8B)
                return genModRM(buf, 0x8B, -1, src, dst);
            }
            else if ((opValType(src) == VT_32) &&
                     (opValType(dst) == VT_64)) {
                src->type = (src->type == OT_Reg32) ? OT_Reg64 : OT_Ind64;
                // use 'movsx r64 ,r/m 32' (0x63)
                return genModRM(buf, 0x63, -1, src, dst);
            }
            break;

        case OT_Imm32:
            // use 'mov r/m 32/64, imm 32' (0xC7/0)
            return genDigitMI(buf, 0xC7, 0, dst, src);

        case OT_Imm64: {
            // try to convert 64-bit immediate to 32bit if value fits
            Operand o;
            int64_t v = (int64_t) src->val;
            if ((v < (1l<<31)) && (v > -(1l<<31))) {;
                o.val = (uint32_t) v;
                o.type = OT_Imm32;
                return genDigitMI(buf, 0xC7, 0, dst, &o);
            }
            o.val = src->val;
            o.type = OT_Imm64;
            return genOI(buf, 0xB8, dst, &o);
        }

        default: assert(0);
        }
        break;

    default: assert(0);
    }
    return 0;
}

int genAdd(uint8_t* buf, Operand* src, Operand* dst)
{
    Operand o; // used for immediates with reduced width

    if (src->type == OT_Imm64) {
        // reduction possible if signed 64bit fits into signed 32bit
        int64_t v = (int64_t) src->val;
        if ((v > -(1l << 31)) && (v < (1l << 31))) {
            o.type = OT_Imm32;
            o.val = (uint32_t) (int32_t) v;
            src = &o;
        }
    }

    if (src->type == OT_Imm32) {
        // reduction possible if signed 32bit fits into signed 8bit
        int32_t v = (int32_t) src->val;
        if ((v > -(1<<7)) && (v < (1<<7))) {
            o.type = OT_Imm8;
            o.val = (uint8_t) (int8_t) v;
            src = &o;
        }
    }

    switch(src->type) {
    case OT_Reg32:
    case OT_Reg64:
        assert(opValType(src) == opValType(dst));
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
        case OT_Ind32:
        case OT_Ind64:
            // use 'add r/m,r 32/64' (0x01)
            return genModRM(buf, 0x01, -1, dst, src);

        default: assert(0);
        }
        break;

    case OT_Ind32:
    case OT_Ind64:
        assert(opValType(src) == opValType(dst));
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
            // use 'add r,r/m 32/64' (0x03)
            return genModRM(buf, 0x03, -1, src, dst);

        default: assert(0);
        }
        break;

    case OT_Imm8:
        // src imm8
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
        case OT_Ind32:
        case OT_Ind64:
            // use 'add r/m 32/64, imm8' (0x83/0)
            return genDigitMI(buf, 0x83, 0, dst, src);

        default: assert(0);
        }
        break;

    case OT_Imm32:
        // src imm
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
        case OT_Ind32:
        case OT_Ind64:
            // use 'add r/m 32/64, imm32' (0x81/0)
            return genDigitMI(buf, 0x81, 0, dst, src);

        default: assert(0);
        }
        break;

    default: assert(0);
    }
    return 0;
}

int genSub(uint8_t* buf, Operand* src, Operand* dst)
{
    switch(src->type) {
    case OT_Reg32:
    case OT_Reg64:
        assert(opValType(src) == opValType(dst));
        // src reg
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
        case OT_Ind32:
        case OT_Ind64:
            // use 'sub r/m,r 32/64' (0x29 MR)
            return genModRM(buf, 0x29, -1, dst, src);

        default: assert(0);
        }
        break;

    case OT_Imm8:
        // src imm32
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
        case OT_Ind32:
        case OT_Ind64:
            // use 'sub r/m 32/64, imm8' (0x83/5 MI)
            return genDigitMI(buf, 0x83, 5, dst, src);

        default: assert(0);
        }
        break;

    case OT_Imm32:
        // src imm32
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
        case OT_Ind32:
        case OT_Ind64:
            // use 'sub r/m 32/64, imm32' (0x81/5 MI)
            return genDigitMI(buf, 0x81, 5, dst, src);

        default: assert(0);
        }
        break;

    default: assert(0);
    }
    return 0;
}

int genIMul(uint8_t* buf, Operand* src, Operand* dst)
{
    Operand o; // used for immediates with reduced width

    if (src->type == OT_Imm32) {
        // reduction possible if signed 32bit fits into signed 8bit
        int32_t v = (int32_t) src->val;
        if ((v > -(1<<7)) && (v < (1<<7))) {
            o.type = OT_Imm8;
            o.val = (uint8_t) (int8_t) v;
            src = &o;
        }
    }

    switch(src->type) {
    case OT_Reg32:
    case OT_Ind32:
    case OT_Reg64:
    case OT_Ind64:
        assert(opValType(src) == opValType(dst));
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
            // use 'imul r,r/m 32/64' (0x0F 0xAF RM)
            return genModRM(buf, 0x0F, 0xAF, src, dst);

        default: assert(0);
        }
        break;

    case OT_Imm8:
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
            // use 'imul r,r/m 32/64,imm8' (0x6B/r RMI)
            return genModRMI(buf, 0x6B, -1, dst, dst, src);

        default: assert(0);
        }
        break;

    case OT_Imm32:
        assert(opValType(src) == opValType(dst));
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
            // use 'imul r,r/m 32/64,imm32' (0x69/r RMI)
            return genModRMI(buf, 0x69, -1, dst, dst, src);

        default: assert(0);
        }
        break;

    default: assert(0);
    }
    return 0;
}

int genXor(uint8_t* buf, Operand* src, Operand* dst)
{
    switch(src->type) {
    // src reg
    case OT_Reg32:
    case OT_Reg64:
        assert(opValType(src) == opValType(dst));
        switch(dst->type) {
        case OT_Reg32:
        case OT_Ind32:
        case OT_Reg64:
        case OT_Ind64:
            // use 'xor r/m,r 32/64' (0x31 MR)
            return genModRM(buf, 0x31, -1, dst, src);

        default: assert(0);
        }
        break;

    default: assert(0);
    }
    return 0;
}

int genShl(uint8_t* buf, Operand* src, Operand* dst)
{
    switch(src->type) {
    // src reg
    case OT_Imm8:
        switch(dst->type) {
        case OT_Reg32:
        case OT_Ind32:
        case OT_Reg64:
        case OT_Ind64:
            // use 'shl r/m 32/64,imm8' (0xC1/4 MI)
            return genDigitMI(buf, 0xC1, 4, dst, src);

        default: assert(0);
        }
        break;

    default: assert(0);
    }
    return 0;
}


int genLea(uint8_t* buf, Operand* src, Operand* dst)
{
    assert(opIsInd(src));
    assert(opIsGPReg(dst));
    switch(dst->type) {
    case OT_Reg32:
    case OT_Reg64:
        // use 'lea r/m,r 32/64' (0x8d)
        return genModRM(buf, 0x8d, -1, src, dst);

    default: assert(0);
    }
    return 0;
}

int genCltq(uint8_t* buf, ValType vt)
{
    switch(vt) {
    case VT_32: buf[0] = 0x98; return 1;
    case VT_64: buf[0] = 0x48; buf[1] = 0x98; return 2;
    default: assert(0);
    }
    return 0;
}


int genCmp(uint8_t* buf, Operand* src, Operand* dst)
{
    switch(src->type) {
    case OT_Imm8:
        // src imm8
        switch(dst->type) {
        case OT_Reg32:
        case OT_Reg64:
        case OT_Ind32:
        case OT_Ind64:
            // use 'cmp r/m 32/64, imm8' (0x83/7 MI)
            return genDigitMI(buf, 0x83, 5, dst, src);

        default: assert(0);
        }
        break;

    default: assert(0);
    }
    return 0;
}

// Pass-through: parser forwarding opcodes, provides encoding
int genPassThrough(uint8_t* buf, Instr* instr)
{
    int o = 0;

    assert(instr->ptLen > 0);
    if (instr->ptPSet & PS_66) buf[o++] = 0x66;
    if (instr->ptPSet & PS_F2) buf[o++] = 0xF2;
    if (instr->ptPSet & PS_F3) buf[o++] = 0xF3;

    // FIXME: REX prefix pass-through: combine with RM encoding changes

    if (instr->ptLen < 2) instr->ptOpc[1] = -1;
    assert(instr->ptLen < 3);

    switch(instr->ptEnc) {
    case OE_MR:
        o += genModRM(buf+o, instr->ptOpc[0], instr->ptOpc[1],
                &(instr->dst), &(instr->src));
        break;

    case OE_RM:
        o += genModRM(buf+o, instr->ptOpc[0], instr->ptOpc[1],
                &(instr->src), &(instr->dst));
        break;

    default: assert(0);
    }

    return o;
}

void resetCapturing(Rewriter* r)
{
    // only to be called after initRewriter()
    assert(r->capInstr != 0);
    assert(r->capBB != 0);

    r->capBBCount = 0;
    r->capInstrCount = 0;
    r->currentCapBB = 0;

    r->capStackTop = -1;
    r->savedStateCount = 0;
}

// return 0 if not found
CBB *findCaptureBB(Rewriter* r, uint64_t f, int esID)
{
    int i;

    for(i = 0; i < r->capBBCount; i++)
        if ((r->capBB[i].dec_addr == f) && (r->capBB[i].esID == esID))
            return &(r->capBB[i]);

    return 0;
}

// allocate an BB structure to collect instructions for capturing
CBB* allocCaptureBB(Rewriter* c, uint64_t f, int esID)
{
    CBB* bb;

    // already captured?
    bb = findCaptureBB(c, f, esID);
    if (bb) return 0;

    // start capturing of new BB beginning at f
    assert(c->capBBCount < c->capBBCapacity);
    bb = &(c->capBB[c->capBBCount]);
    c->capBBCount++;
    bb->dec_addr = f;
    bb->esID = esID;
    bb->count = 0;
    bb->instr = c->capInstr + c->capInstrCount;
    bb->nextBranch = 0;
    bb->nextFallThrough = 0;
    bb->endType = IT_None;
    bb->addr = 0;
    bb->size = 0;

    return bb;
}

int pushCaptureBB(Rewriter* r, CBB* bb)
{
    assert(r->capStackTop + 1 < CAPTURESTACK_LEN);
    r->capStackTop++;
    r->capStack[r->capStackTop] = bb;

    return r->capStackTop;
}

CBB* popCaptureBB(Rewriter* r)
{
    CBB* bb = r->currentCapBB;
    assert(r->capStack[r->capStackTop] == bb);
    r->capStackTop--;
    r->currentCapBB = 0;

    return bb;
}

// capture a new instruction
void capture(Rewriter* c, Instr* instr)
{
    CBB* bb = c->currentCapBB;
    if (bb == 0) return;

    if (c->showEmuSteps)
        printf("Capture '%s' (from BB 0x%lx + %d)\n",
               instr2string(instr, 0), bb->dec_addr, bb->count);

    assert(c->capInstrCount < c->capInstrCapacity);
    copyInstr(bb->instr + bb->count, instr);
    c->capInstrCount++;
    bb->count++;
}

// generate code for a captured BB
void generate(Rewriter* c, CBB* bb)
{
    uint8_t* buf;
    int used, i, usedTotal;

    if (bb == 0) return;
    if (c->cs == 0) return;

    if (c->showEmuSteps)
        printf("Generating code for BB from %lx, esID %d (%d instructions)\n",
               bb->dec_addr, bb->esID, bb->count);

    usedTotal = 0;
    for(i = 0; i < bb->count; i++) {
        Instr* instr = bb->instr + i;

        buf = reserveCodeStorage(c->cs, 15);

        if (instr->ptLen > 0) {
            used = genPassThrough(buf, instr);
        }
        else {
            switch(instr->type) {
            case IT_ADD:
                used = genAdd(buf, &(instr->src), &(instr->dst));
                break;
            case IT_CLTQ:
                used = genCltq(buf, instr->vtype);
                break;
            case IT_CMP:
                used = genCmp(buf, &(instr->src), &(instr->dst));
                break;
            case IT_IMUL:
                used = genIMul(buf, &(instr->src), &(instr->dst));
                break;
            case IT_XOR:
                used = genXor(buf, &(instr->src), &(instr->dst));
                break;
            case IT_SHL:
                used = genShl(buf, &(instr->src), &(instr->dst));
                break;
            case IT_LEA:
                used = genLea(buf, &(instr->src), &(instr->dst));
                break;
            case IT_MOV:
            case IT_MOVSX: // converting move
                used = genMov(buf, &(instr->src), &(instr->dst));
                break;
            case IT_POP:
                used = genPop(buf, &(instr->dst));
                break;
            case IT_PUSH:
                used = genPush(buf, &(instr->dst));
                break;
            case IT_RET:
                used = genRet(buf);
                break;
            case IT_SUB:
                used = genSub(buf, &(instr->src), &(instr->dst));
                break;
            default: assert(0);
            }
        }
        assert(used < 15);

        instr->addr = (uint64_t) buf;
        instr->len = used;
        usedTotal += used;

        if (c->showEmuSteps) {
            printf("  Instr %2d: %-32s", i, instr2string(instr, 1));
            printf(" %2d bytes: %s\n", used, bytes2string(instr, 0, used));
        }

        useCodeStorage(c->cs, used);
    }

    bb->size = usedTotal;
    bb->addr = (bb->count > 0) ? bb->instr[0].addr : 0;
}


/*------------------------------------------------------------*/
/* x86_64 capturing emulator
 * trace execution in the emulator to capture code to generate
 *
 * We maintain states (known/static vs unknown/dynamic at capture time)
 * for registers and values on stack. To be able to do the latter, we
 * assume that the known values on stack do not get changed by
 * memory writes with dynamic address. This assumption should be fine,
 * as such behavior is dangerous and potentially a bug.
 *
 * At branches to multiple possible targets, we need to travers each path by
 * saving emulator state. After emulating one path, we roll back and
 * go the other path. As this may happen recursively, we do a kind of
 * back-tracking, with emulator states stored as stacks.
 * To allow for fast saving/restoring of emulator states, each part of
 * the emulation state (registers, bytes on stack) is given by a
 * EmuStateEntry (linked) list with the current value/state in front.
 * Saving copies the complete EmuState, inheriting the individual states.
 */

// emulator capture states
typedef enum _CaptureState {
    CS_DEAD = 0,      // uninitialized, should be invalid to access
    CS_DYNAMIC,       // data unknown at code generation time
    CS_STATIC,        // data known at code generation time
    CS_STACKRELATIVE, // address with known offset from stack top at start
    CS_STATIC2,       // same as static + indirection from memory static
    CS_Max
} CaptureState;

typedef enum _FlagType {
    FT_Carry = 0, FT_Zero, FT_Sign,
    FT_Max
} FlagType;

#define MAX_CALLDEPTH 5

// emulator state. for memory, use the real memory apart from stack
typedef struct _EmuState {

    // when saving an EmuState, remember root
    EmuState* parent;
    
    // general registers: Reg_AX .. Reg_R15
    uint64_t reg[Reg_Max];
    CaptureState reg_state[Reg_Max];

    // x86 flags: carry (CF), zero (ZF), sign (SF)
    // TODO: overflow, parity, auxiliary carry
    Bool flag[FT_Max];
    CaptureState flag_state[FT_Max];

    // stack
    int stackSize;
    uint8_t* stack; // real memory backing
    uint64_t stackStart, stackAccessed, stackTop; // virtual stack boundaries
    // capture state of stack
    CaptureState *stackState;

    // own return stack
    uint64_t ret_stack[MAX_CALLDEPTH];
    int depth;

} EmuState;

// a single value with type and capture state
typedef struct _EmuValue {
    uint64_t val;
    ValType type;
    CaptureState state;
} EmuValue;

#define CC_MAXPARAM 5
typedef struct _CaptureConfig
{
    CaptureState par_state[CC_MAXPARAM];
     // does function to rewrite return floating point?
    Bool hasReturnFP;
} CaptureConfig;

char captureState2Char(CaptureState s)
{
    assert((s >= 0) && (s < CS_Max));
    assert(CS_Max == 5);
    return "-DSR2"[s];
}

Bool stateIsStatic(CaptureState s)
{
    if ((s == CS_STATIC) || (s == CS_STATIC2)) return True;
    return False;
}

void freeCaptureConfig(Rewriter* r)
{
    free(r->cc);
    r->cc = 0;
}

void resetRewriterConfig(Rewriter* c)
{
    CaptureConfig* cc;
    int i;

    if (c->cc)
        freeCaptureConfig(c);

    cc = (CaptureConfig*) malloc(sizeof(CaptureConfig));
    for(i=0; i < CC_MAXPARAM; i++)
        cc->par_state[i] = CS_DYNAMIC;
    cc->hasReturnFP = False;

    c->cc = cc;
}


CaptureConfig* getCaptureConfig(Rewriter* c)
{
    if (c->cc == 0)
        resetRewriterConfig(c);

    return c->cc;
}

void setRewriterStaticPar(Rewriter* c, int staticParPos)
{
    CaptureConfig* cc = getCaptureConfig(c);

    assert((staticParPos >= 0) && (staticParPos < CC_MAXPARAM));
    cc->par_state[staticParPos] = CS_STATIC2;
}

void setRewriterReturnFP(Rewriter* c)
{
    CaptureConfig* cc = getCaptureConfig(c);
    cc->hasReturnFP = True;
}

EmuValue emuValue(uint64_t v, ValType t, CaptureState s)
{
    EmuValue ev;
    ev.val = v;
    ev.type = t;
    ev.state = s;

    return ev;
}

EmuValue staticEmuValue(uint64_t v, ValType t)
{
    EmuValue ev;
    ev.val = v;
    ev.type = t;
    ev.state = CS_STATIC;

    return ev;
}

void resetEmuState(EmuState* es)
{
    int i;
    static Reg calleeSave[] = {
        Reg_BP, Reg_BX, Reg_12, Reg_13, Reg_14, Reg_15, Reg_None };

    es->parent = 0;

    for(i=0; i<Reg_Max; i++) {
        es->reg[i] = 0;
        es->reg_state[i] = CS_DEAD;
    }

    for(i=0; i<FT_Max; i++) {
        es->flag[i] = False;
        es->flag_state[i] = CS_DEAD;
    }

    for(i=0; i< es->stackSize; i++)
        es->stack[i] = 0;
    for(i=0; i< es->stackSize; i++)
        es->stackState[i] = CS_DEAD;

    // use real addresses for now
    es->stackStart = (uint64_t) es->stack;
    es->stackTop = es->stackStart + es->stackSize;
    es->stackAccessed = es->stackTop;

    // calling convention:
    //  rbp, rbx, r12-r15 have to be preserved by callee
    for(i=0; calleeSave[i] != Reg_None; i++)
        es->reg_state[calleeSave[i]] = CS_DYNAMIC;

    // RIP always known
    es->reg_state[Reg_IP] = CS_STATIC;
}

EmuState* allocEmuState(int size)
{
    EmuState* es;

    es = (EmuState*) malloc(sizeof(EmuState));
    es->stackSize = size;
    es->stack = (uint8_t*) malloc(size);
    es->stackState = (CaptureState*) malloc(sizeof(CaptureState) * size);

    return es;
}

void freeEmuState(Rewriter* r)
{
    if (!r->es) return;

    free(r->es->stack);
    free(r->es->stackState);
    free(r->es);
    r->es = 0;
}

// are the capture states of a memory resource from different EmuStates equal?
// this is required for compatibility of generated code points, and
// compatibility is needed to be able to jump between such code points
Bool csIsEqual(EmuState* es1, CaptureState s1, uint64_t v1,
               EmuState* es2, CaptureState s2, uint64_t v2)
{
    // normalize meta states: CS_STATIC2 is equivalent to CS_STATIC
    if (s1 == CS_STATIC2) s1 = CS_STATIC;
    if (s2 == CS_STATIC2) s2 = CS_STATIC;
    
    if (s1 != s2) return False;
    
    switch(s1) {
    case CS_STATIC:
        // for static capture states, values have to be equal
        return (v1 == v2);
        
    case CS_STACKRELATIVE:
        // FIXME: in reality: same offset from a versioned anchor
        // for now: assume same anchor version (within same rewriting action)
        if (es1->parent != es2->parent) return False;
        return (v1 == v2);
        
    default:
        break;
    }
    return True;
}

// states are equal if metainformation is equal and static data is the same
Bool esIsEqual(EmuState* es1, EmuState* es2)
{
    int i;
    
    // same state for registers?
    for(i = 0; i < Reg_Max; i++) {
        if (!csIsEqual(es1, es1->reg_state[i], es1->reg[i],
                       es2, es2->reg_state[i], es2->reg[i]))
            return False;
    }
    
    // same state for registers?
    for(i = 0; i < FT_Max; i++) {
        if (!csIsEqual(es1, es1->flag_state[i], es1->flag[i],
                       es2, es2->flag_state[i], es2->flag[i]))
            return False;
    }

    // TODO: Stack. May need to explicitly remember types/offsets
}

void copyEmuState(EmuState* dst, EmuState* src)
{
    int i;

    dst->parent = src->parent;

    for(i=0; i<Reg_Max; i++) {
        dst->reg[i] = src->reg[i];
        dst->reg_state[i] = src->reg_state[i];
    }

    for(i = 0; i < FT_Max; i++) {
        dst->flag[i] = src->flag[i];
        dst->flag_state[i] = src->flag_state[i];
    }

    dst->stackTop = src->stackTop;
    dst->stackAccessed = src->stackAccessed;
    if (src->stackSize < dst->stackSize) {
        // stack to restore is smaller than at destination:
        // fill start of destination with DEAD entries
        int diff = dst->stackSize - src->stackSize;

        dst->stackStart = src->stackStart - diff;
        for(i = 0; i < diff; i++) {
            dst->stackState[i] = CS_DEAD;
            dst->stack[i] = 0;
        }
        for(i = 0; i < src->stackSize; i++) {
            dst->stack[i+diff] = src->stack[i];
            dst->stackState[i+diff] = src->stackState[i];
        }
    }
    else {
        // stack to restore is larger than at destination:
        // make sure that start of source was never accessed
        int diff = src->stackSize - dst->stackSize;
        assert(src->stackAccessed - src->stackStart >= diff);

        dst->stackStart = src->stackStart + diff;
        for(i = 0; i < dst->stackSize; i++) {
            dst->stack[i] = src->stack[i+diff];
            dst->stackState[i] = src->stackState[i+diff];
        }
    }
    assert(dst->stackTop == dst->stackStart + dst->stackSize);
}

EmuState* cloneEmuState(EmuState* src)
{
    EmuState* dst;

    // allocate only stack space that was accessed in source
    dst = allocEmuState(src->stackTop - src->stackAccessed);
    copyEmuState(dst, src);

    // remember that we cloned dst from src
    dst->parent = src;

    return dst;
}

// checks current state against already saved states, and returns an ID
// (which is the index in the saved state list of the rewriter)
int saveEmuState(Rewriter* r)
{
    int i;

    for(i = 0; i < r->savedStateCount; i++)
        if (esIsEqual(r->es, r->savedState[i]))
            return i;

    assert(i < SAVEDSTATE_MAX);
    r->savedState[i] = cloneEmuState(r->es);
    r->savedStateCount++;

    return i;
}

void restoreEmuState(Rewriter* r, int esID)
{
    assert((esID >= 0) && (esID < r->savedStateCount));
    assert(r->savedState[esID] != 0);
    copyEmuState(r->es, r->savedState[esID]);
}

void printEmuState(EmuState* es)
{
    int i, spOff, spMin, spMax, o, oo;

    printf("Emulation State:\n");

    printf("  Call stack (current depth %d): ", es->depth);
    for(i=0; i<es->depth; i++)
        printf(" %p", (void*) es->ret_stack[i]);
    printf("%s\n", (es->depth == 0) ? " (empty)":"");

    printf("  Registers:\n");
    for(i=Reg_AX; i<Reg_8; i++) {
        printf("    %%%-3s = 0x%016lx %c", regName(i, OT_Reg64),
               es->reg[i], captureState2Char( es->reg_state[i] ));
        printf("    %%%-3s = 0x%016lx %c\n", regName(i+8, OT_Reg64),
               es->reg[i+8], captureState2Char( es->reg_state[i+8] ));
    }
    printf("    %%%-3s = 0x%016lx %c\n", regName(Reg_IP, OT_Reg64),
           es->reg[Reg_IP], captureState2Char( es->reg_state[Reg_IP] ));

    printf("  Flags: CF %d %c  ZF %d %c  SF %d %c\n",
           es->flag[FT_Carry], captureState2Char(es->flag_state[FT_Carry]),
           es->flag[FT_Zero], captureState2Char(es->flag_state[FT_Zero]),
           es->flag[FT_Sign], captureState2Char(es->flag_state[FT_Sign]));

    spOff = es->reg[Reg_SP] - es->stackStart;
    spMax = spOff /8*8 + 24;
    spMin = spOff /8*8 - 32;
    if (es->stackStart + spMin < es->stackAccessed)
        spMin = (es->stackAccessed - es->stackStart)/8*8;
    if (es->stackStart + spMax > es->stackTop)
        spMax = es->stackSize -1;
    if (spMin >= spMax)
        printf("  Stack: (empty)\n");
    else {
        printf("  Stack:\n");
        for(o = spMin; o < spMax; o += 8) {
            printf("   %016lx ", (uint64_t) (es->stackStart + o));
            for(oo = o; oo < o+8 && oo <= spMax; oo++) {
                printf(" %s%02x %c", (oo == spOff) ? "*" : " ", es->stack[oo],
                       captureState2Char(es->stackState[oo]));
            }
            printf("\n");
        }
        printf("   %016lx  %s\n",
               (uint64_t) (es->stackStart + o), (o == spOff) ? "*" : " ");
    }
}

char combineState(CaptureState s1, CaptureState s2,
                  Bool isSameValue)
{
    // dead/invalid: combining with something invalid makes result invalid
    if ((s1 == CS_DEAD) || (s2 == CS_DEAD)) return CS_DEAD;

    // if both are static, static-ness is preserved
    if (stateIsStatic(s1) && stateIsStatic(s2)) {
        if ((s1 == CS_STATIC2) || (s2 == CS_STATIC2)) return CS_STATIC2;
        return CS_STATIC;
    }

    // stack-relative handling:
    // depends on combining of sub-state of one value or combining two values
    if (isSameValue) {
        // if both are stack-relative, it is preserved
        if ((s1 == CS_STACKRELATIVE) && (s2 == CS_STACKRELATIVE))
            return CS_STACKRELATIVE;
    }
    else {
        // STACKRELATIVE is preserved if other is STATIC (FIXME: only ADD!)
        if ((s1 == CS_STACKRELATIVE) && stateIsStatic(s2))
            return CS_STACKRELATIVE;
        if (stateIsStatic(s1) && (s2 == CS_STACKRELATIVE))
            return CS_STACKRELATIVE;
    }

    return CS_DYNAMIC;
}

char combineState4Flags(CaptureState s1, CaptureState s2)
{
    CaptureState s;

    s = combineState(s1, s2, 0);
    // STACKRELATIVE/STATIC2 makes no sense for flags
    if (s == CS_STACKRELATIVE) s = CS_DYNAMIC;
    if (s == CS_STATIC2) s = CS_STATIC;

    return s;
}

// v1 - v2
CaptureState setFlagsSub(EmuState* es, EmuValue* v1, EmuValue* v2)
{
    CaptureState s;

    s = combineState4Flags(v1->state, v2->state);
    es->flag_state[FT_Carry] = s;
    es->flag_state[FT_Zero] = s;
    es->flag_state[FT_Sign] = s;

    assert(v1->type == v2->type);

    es->flag[FT_Carry] = (v1->val < v2->val);
    es->flag[FT_Zero]  = (v1->val == v2->val);
    switch(v1->type) {
    case VT_8:
        es->flag[FT_Sign] = (((v1->val - v2->val) & (1l<<7)) != 0);
        break;
    case VT_32:
        es->flag[FT_Sign] = (((v1->val - v2->val) & (1l<<31)) != 0);
        break;
    case VT_64:
        es->flag[FT_Sign] = (((v1->val - v2->val) & (1l<<63)) != 0);
        break;
    default: assert(0);
    }

    return s;
}

void setFlagsAdd(EmuState* es, EmuValue* v1, EmuValue* v2)
{
    CaptureState s;

    s = combineState4Flags(v1->state, v2->state);
    es->flag_state[FT_Carry] = s;
    es->flag_state[FT_Zero] = s;
    es->flag_state[FT_Sign] = s;

    assert(v1->type == v2->type);

    switch(v1->type) {
    case VT_8:
        es->flag[FT_Carry] = (v1->val + v2->val >= (1<<8));
        es->flag[FT_Zero]  = ((v1->val + v2->val) & ((1<<8)-1) == 0);
        es->flag[FT_Sign]  = (((v1->val + v2->val) & (1l<<7)) != 0);
        break;
    case VT_32:
        es->flag[FT_Carry] = (v1->val + v2->val >= (1l<<32));
        es->flag[FT_Zero]  = ((v1->val + v2->val) & ((1l<<32)-1) == 0);
        es->flag[FT_Sign]  = (((v1->val + v2->val) & (1l<<31)) != 0);
        break;
    case VT_64:
        es->flag[FT_Carry] = ((v1->val + v2->val) < v1->val);
        es->flag[FT_Zero]  = ((v1->val + v2->val) == 0);
        es->flag[FT_Sign]  = (((v1->val + v2->val) & (1l<<63)) != 0);
        break;
    default: assert(0);
    }
}

// for bitwise operations: And, Xor
CaptureState setFlagsBit(EmuState* es, InstrType it,
                         EmuValue* v1, EmuValue* v2, Bool sameOperands)
{
    CaptureState s;
    uint64_t res;

    assert(v1->type == v2->type);

    s = combineState4Flags(v1->state, v2->state);
    // xor op,op results in known zero
    if ((it == IT_XOR) && sameOperands) s = CS_STATIC;

    // carry always cleared (TODO: also overflow)
    es->flag[FT_Carry] = 0;
    es->flag_state[FT_Carry] = CS_STATIC;

    es->flag_state[FT_Zero] = s;
    es->flag_state[FT_Sign] = s;

    switch(it) {
    case IT_AND: res = v1->val & v2->val; break;
    case IT_XOR: res = v1->val ^ v2->val; break;
    default: assert(0);
    }

    es->flag[FT_Zero]  = (res == 0);
    switch(v1->type) {
    case VT_8:
        es->flag[FT_Sign] = ((res & (1l<<7)) != 0);
        break;
    case VT_32:
        es->flag[FT_Sign] = ((res & (1l<<31)) != 0);
        break;
    case VT_64:
        es->flag[FT_Sign] = ((res & (1l<<63)) != 0);
        break;
    default: assert(0);
    }

    return s;
}

// if addr on stack, return true and stack offset in <off>,
//  otherwise return false
// the returned offset is static only if address is stack-relative
Bool getStackOffset(EmuState* es, EmuValue* addr, EmuValue* off)
{
    if ((addr->val >= es->stackStart) && (addr->val < es->stackTop)) {
        off->type = VT_32;
        off->state = (addr->state == CS_STACKRELATIVE) ? CS_STATIC : CS_DYNAMIC;
        off->val = addr->val - es->stackStart;
        return True;
    }
    return False;
}

CaptureState getStackState(EmuState* es, EmuValue* off)
{
    if (off->state == CS_STATIC) {
        if (off->val >= es->stackSize) return CS_DEAD;
        if (off->val < es->stackAccessed - es->stackStart) return CS_DEAD;
        return es->stackState[off->val];
    }
    return CS_DYNAMIC;
}

void getStackValue(EmuState* es, EmuValue* v, EmuValue* off)
{
    int i, count;
    CaptureState state;

    assert((off->val >= 0) && (off->val < es->stackSize));

    switch(v->type) {
    case VT_32:
        v->val = *(uint32_t*) (es->stack + off->val);
        count = 4;
        break;

    case VT_64:
        v->val = *(uint64_t*) (es->stack + off->val);
        count = 8;
        break;

    default: assert(0);
    }

    if (off->state == CS_STATIC) {
        state = getStackState(es, off);
        for(i=1; i<count; i++)
            state = combineState(state, es->stackState[off->val + i], 1);
    }
    else
        state = CS_DYNAMIC;

    v->state = state;
}


void setStackValue(EmuState* es, EmuValue* v, EmuValue* off)
{
    uint32_t* a32;
    uint64_t* a64;
    int i, count;

    switch(v->type) {
    case VT_32:
        a32 = (uint32_t*) (es->stack + off->val);
        *a32 = (uint32_t) v->val;
        count = 4;
        break;

    case VT_64:
        a64 = (uint64_t*) (es->stack + off->val);
        *a64 = (uint64_t) v->val;
        count = 8;
        break;

    default: assert(0);
    }

    if (off->state == CS_STATIC) {
        for(i=0; i<count; i++)
            es->stackState[off->val + i] = v->state;
    }

    if (es->stackStart + off->val < es->stackAccessed)
        es->stackAccessed = es->stackStart + off->val;
}

void getRegValue(EmuValue* v, EmuState* es, Reg r, ValType t)
{
    v->type = t;
    v->val = es->reg[r];
    v->state = es->reg_state[r];
}

void setRegValue(EmuValue* v, EmuState* es, Reg r, ValType t)
{
    assert(v->type == t);
    es->reg[r] = v->val;
    es->reg_state[r] = v->state;
}

void getMemValue(EmuValue* v, EmuValue* addr, EmuState* es, ValType t,
                 Bool shouldBeStack)
{
    EmuValue off;
    int isOnStack;

    isOnStack = getStackOffset(es, addr, &off);
    if (isOnStack) {
        v->type = t;
        getStackValue(es, v, &off);
        return;
    }

    assert(!shouldBeStack);
    v->state = CS_DYNAMIC;
    // explicit request to make memory access result static
    if (addr->state == CS_STATIC2) v->state = CS_STATIC2;

    v->type = t;
    switch(t) {
    case VT_32: v->val = *(uint32_t*) addr->val; break;
    case VT_64: v->val = *(uint64_t*) addr->val; break;
    default: assert(0);
    }
}

void setMemValue(EmuValue* v, EmuValue* addr, EmuState* es, ValType t,
                 int shouldBeStack)
{
    EmuValue off;
    uint32_t* a32;
    uint64_t* a64;
    Bool isOnStack;

    assert(v->type == t);
    isOnStack = getStackOffset(es, addr, &off);
    if (isOnStack) {
        setStackValue(es, v, &off);
        return;
    }

    assert(!shouldBeStack);

    switch(t) {
    case VT_32:
        a32 = (uint32_t*) addr->val;
        *a32 = (uint32_t) v->val;
        break;

    case VT_64:
        a64 = (uint64_t*) addr->val;
        *a64 = (uint64_t) v->val;
        break;

    default: assert(0);
    }
}

void addRegToValue(EmuValue* v, EmuState* es, Reg r, int scale)
{
    if (r == Reg_None) return;

    v->state = combineState(v->state, es->reg_state[r], 0);
    v->val += scale * es->reg[r];
}


void getOpAddr(EmuValue* v, EmuState* es, Operand* o)
{
    assert(opIsInd(o));

    v->type = VT_64;
    v->val = o->val;
    v->state = CS_STATIC;

    if (o->reg != Reg_None)
        addRegToValue(v, es, o->reg, 1);

    if (o->scale > 0)
        addRegToValue(v, es, o->ireg, o->scale);
}

// returned value should be casted to expected type (8/16/32 bit)
void getOpValue(EmuValue* v, EmuState* es, Operand* o)
{
    EmuValue addr;

    switch(o->type) {
    case OT_Imm8:
    case OT_Imm16:
    case OT_Imm32:
    case OT_Imm64:
        *v = staticEmuValue(o->val, opValType(o));
        return;

    case OT_Reg32:
        v->type = VT_32;
        v->val = (uint32_t) es->reg[o->reg];
        v->state = es->reg_state[o->reg];
        return;

    case OT_Reg64:
        v->type = VT_64;
        v->val = (uint64_t) es->reg[o->reg];
        v->state = es->reg_state[o->reg];
        return;

    case OT_Ind32:
    case OT_Ind64:
        getOpAddr(&addr, es, o);
        getMemValue(v, &addr, es, opValType(o), 0);
        return;

    default: assert(0);
    }
}

// only the bits of v are used which are required for operand type
void setOpValue(EmuValue* v, EmuState* es, Operand* o)
{
    EmuValue addr;

    assert(v->type == opValType(o));
    switch(o->type) {
    case OT_Reg32:
        es->reg[o->reg] = (uint32_t) v->val;
        es->reg_state[o->reg] = v->state;
        return;

    case OT_Reg64:
        es->reg[o->reg] = v->val;
        es->reg_state[o->reg] = v->state;
        return;

    case OT_Ind32:
    case OT_Ind64:
        getOpAddr(&addr, es, o);
        setMemValue(v, &addr, es, opValType(o), 0);
        return;

    default: assert(0);
    }
}

// false if not on stack or stack offset not static/known
Bool keepsCaptureState(EmuState* es, Operand* o)
{
    EmuValue addr;
    EmuValue off;
    Bool isOnStack;

    assert(!opIsImm(o));
    if (opIsGPReg(o)) return 1;

    getOpAddr(&addr, es, o);
    isOnStack = getStackOffset(es, &addr, &off);
    if (!isOnStack) return 0;
    return stateIsStatic(off.state);
}

void applyStaticToInd(Operand* o, EmuState* es)
{
    if (!opIsInd(o)) return;

    if ((o->reg != Reg_None) && stateIsStatic(es->reg_state[o->reg])) {
        o->val += es->reg[o->reg];
        o->reg = Reg_None;
    }
    if ((o->scale > 0) && stateIsStatic(es->reg_state[o->ireg])) {
        o->val += o->scale * es->reg[o->ireg];
        o->scale = 0;
    }
}

// both MOV and MOVSX (sign extend 32->64)
void captureMov(Rewriter* c, Instr* orig, EmuState* es, EmuValue* res)
{
    Instr i;
    Operand *o;

    // data movement from orig->src to orig->dst, value is res

    if (res->state == CS_DEAD) return;

    o = &(orig->src);
    if (stateIsStatic(res->state)) {
        // no need to update data if capture state is maintained
        if (keepsCaptureState(es, &(orig->dst))) return;

        // source is static, use immediate
        o = getImmOp(res->type, res->val);
    }
    initBinaryInstr(&i, orig->type, orig->vtype, &(orig->dst), o);
    applyStaticToInd(&(i.dst), es);
    applyStaticToInd(&(i.src), es);
    capture(c, &i);
}

// dst = dst op src
void captureBinaryOp(Rewriter* c, Instr* orig, EmuState* es, EmuValue* res)
{
    EmuValue opval;
    Instr i;
    Operand *o;

    if (res->state == CS_DEAD) return;

    if (stateIsStatic(res->state)) {
        // no need to update data if capture state is maintained
        if (keepsCaptureState(es, &(orig->dst))) return;

        // if result is known and goes to memory, generate imm store
        initBinaryInstr(&i, IT_MOV, res->type,
                        &(orig->dst), getImmOp(res->type, res->val));
        applyStaticToInd(&(i.dst), es);
        capture(c, &i);
        return;
    }

    // if dst (= 2.op) known/constant and a reg/stack, we need to update it
    getOpValue(&opval, es, &(orig->dst));
    if (keepsCaptureState(es, &(orig->dst)) && stateIsStatic(opval.state)) {

        // - instead of adding src to 0, we can move the src to dst
        // - instead of multiply src with 1, move
        // TODO: mulitply with 0: here too late, state of result gets static
        if (((orig->type == IT_ADD) && (opval.val == 0)) ||
            ((orig->type == IT_IMUL) && (opval.val == 1))) {
            initBinaryInstr(&i, IT_MOV, opval.type,
                            &(orig->dst), &(orig->src));
            applyStaticToInd(&(i.dst), es);
            applyStaticToInd(&(i.src), es);
            capture(c, &i);
            return;
        }

        initBinaryInstr(&i, IT_MOV, opval.type,
                        &(orig->dst), getImmOp(opval.type, opval.val));
        capture(c, &i);
    }

    o = &(orig->src);
    getOpValue(&opval, es, &(orig->src));
    if (stateIsStatic(opval.state)) {
        // if 1st source (=src) is known/constant and a reg, make it immediate

        if (((orig->type == IT_ADD) && (opval.val == 0)) ||
            ((orig->type == IT_SHL) && (opval.val == 0)) ||
            ((orig->type == IT_SHR) && (opval.val == 0)) ||
            ((orig->type == IT_IMUL) && (opval.val == 1))) {
            // adding 0 / multiplying with 1 changes nothing...
            return;
        }
        o = getImmOp(opval.type, opval.val);
    }
    initBinaryInstr(&i, orig->type, res->type, &(orig->dst), o);
    applyStaticToInd(&(i.dst), es);
    applyStaticToInd(&(i.src), es);
    capture(c, &i);
}

void captureNeg(Rewriter* c, Instr* orig, EmuState* es, EmuValue* res)
{
    Instr i;

    if (stateIsStatic(res->state)) return;

    initUnaryInstr(&i, IT_NEG, &(orig->dst));
    applyStaticToInd(&(i.dst), es);
    capture(c, &i);
}

void captureLea(Rewriter* c, Instr* orig, EmuState* es, EmuValue* res)
{
    Instr i;

    if (stateIsStatic(res->state)) return;

    initBinaryInstr(&i, IT_LEA, orig->vtype, &(orig->dst), &(orig->src));
    applyStaticToInd(&(i.src), es);
    capture(c, &i);
}

void captureCmp(Rewriter* c, Instr* orig, EmuState* es, CaptureState s)
{
    Instr i;

    if (stateIsStatic(s)) return;

    initBinaryInstr(&i, IT_CMP, orig->vtype, &(orig->dst), &(orig->src));
    applyStaticToInd(&(i.dst), es);
    applyStaticToInd(&(i.src), es);
    capture(c, &i);
}

void captureTest(Rewriter* c, Instr* orig, EmuState* es, CaptureState s)
{
    Instr i;

    if (stateIsStatic(s)) return;

    initBinaryInstr(&i, IT_TEST, orig->vtype, &(orig->dst), &(orig->src));
    applyStaticToInd(&(i.dst), es);
    applyStaticToInd(&(i.src), es);
    capture(c, &i);
}

void captureRet(Rewriter* c, Instr* orig, EmuState* es)
{
    EmuValue v;
    Instr i;

    // when returning an integer: if AX state is static, load constant
    if (!c->cc->hasReturnFP) {
        getRegValue(&v, es, Reg_AX, VT_64);
        if (stateIsStatic(v.state)) {
            initBinaryInstr(&i, IT_MOV, VT_64,
                            getRegOp(VT_64, Reg_AX), getImmOp(v.type, v.val));
            capture(c, &i);
        }
    }
    capture(c, orig);
}

void capturePassThrough(Rewriter* c, Instr* orig, EmuState* es)
{
    Instr i;

    assert(orig->ptLen >0);
    initSimpleInstr(&i, orig->type);
    i.ptLen  = orig->ptLen;
    i.ptEnc  = orig->ptEnc;
    i.ptPSet = orig->ptPSet;
    for(int j=0; j<orig->ptLen; j++)
        i.ptOpc[j] = orig->ptOpc[j];

    switch(orig->ptEnc) {
    case OE_MR:
        assert(opIsReg(&(orig->dst)) || opIsInd(&(orig->dst)));
        assert(opIsReg(&(orig->src)));

        i.form = OF_2;
        copyOperand( &(i.dst), &(orig->dst));
        copyOperand( &(i.src), &(orig->src));
        applyStaticToInd(&(i.dst), es);
        break;

    case OE_RM:
        assert(opIsReg(&(orig->src)) || opIsInd(&(orig->src)));
        assert(opIsReg(&(orig->dst)));

        i.form = OF_2;
        copyOperand( &(i.dst), &(orig->dst));
        copyOperand( &(i.src), &(orig->src));
        applyStaticToInd(&(i.src), es);
        break;

    default: assert(0);
    }
    capture(c, &i);
}

// this ends a captured BB, queuing new paths to be traced
uint64_t captureExit(Rewriter* r, InstrType it,
                     uint64_t branchTarget, uint64_t fallthroughTarget)
{
    CBB *bb;
    int esID;

    bb = popCaptureBB(r);
    bb->endType = it;

    esID = -1;
    if (branchTarget > 0) {
        esID = saveEmuState(r);
        CBB* newCBB = allocCaptureBB(r, branchTarget, esID);
        bb->nextBranch = newCBB;
        pushCaptureBB(r, newCBB);
    }
    if (fallthroughTarget > 0) {
        if (esID < 0) esID = saveEmuState(r);
        CBB* newCBB = allocCaptureBB(r, fallthroughTarget, esID);
        bb->nextFallThrough = newCBB;
        pushCaptureBB(r, newCBB);
    }
}

// return 0 to fall through to next instruction, or return address to jump to
uint64_t emulateInstr(Rewriter* c, EmuState* es, Instr* instr)
{
    EmuValue vres, v1, v2, addr;
    CaptureState s;
    ValType vt;

    if (instr->ptLen > 0) {
        // pass-through: no effect on emu-state, no emulation done
        // still, emu-state have influence memory access
        capturePassThrough(c, instr, es);
        return 0;
    }

    switch(instr->type) {

    case IT_ADD:
        getOpValue(&v1, es, &(instr->dst));
        getOpValue(&v2, es, &(instr->src));

        vt = opValType(&(instr->dst));
        // sign-extend src/v2 if needed
        if (instr->src.type == OT_Imm8) {
            // sign-extend to 64bit (may be cutoff later)
            v2.val = (int64_t) (int8_t) v2.val;
            v2.type = (vt == VT_32) ? OT_Imm32 : OT_Imm64;
        }

        setFlagsAdd(es, &v1, &v2);
        assert(v1.type == v2.type);

        switch(vt) {
        case VT_32:
            v1.val = ((uint32_t) v1.val + (uint32_t) v2.val);
            break;

        case VT_64:
            v1.val = v1.val + v2.val;
            break;

        default:assert(0);
        }

        v1.state = combineState(v1.state, v2.state, 0);
        // for capture we need state of dst, do before setting dst
        captureBinaryOp(c, instr, es, &v1);
        setOpValue(&v1, es, &(instr->dst));
        break;

    case IT_IMUL:
        getOpValue(&v1, es, &(instr->dst));
        getOpValue(&v2, es, &(instr->src));

        assert(opIsGPReg(&(instr->dst)));
        assert(v1.type == v2.type);
        switch(instr->src.type) {
        case OT_Reg32:
        case OT_Ind32:
            vres.type = VT_32;
            vres.val = (uint64_t) ((int32_t) v1.val * (int32_t) v2.val);
            break;

        case OT_Reg64:
        case OT_Ind64:
            vres.type = VT_64;
            vres.val = (uint64_t) ((int64_t) v1.val * (int64_t) v2.val);
            break;

        default:assert(0);
        }

        // optimization: muliply with static 0 results in static 0
        if ((stateIsStatic(v1.state) && (v1.val == 0)) ||
            (stateIsStatic(v2.state) && (v2.val == 0)))
            vres.state = CS_STATIC;
        else
            vres.state = combineState(v1.state, v2.state, 0);

        // for capture we need state of dst, do before setting dst
        captureBinaryOp(c, instr, es, &vres);
        setOpValue(&vres, es, &(instr->dst));
        break;

    case IT_CALL:
        getOpValue(&v1, es, &(instr->dst));
        assert(es->depth < MAX_CALLDEPTH);
        assert(stateIsStatic(v1.state)); // call target must be known

        // push address of instruction after CALL onto stack
        es->reg[Reg_SP] -= 8;
        addr = emuValue(es->reg[Reg_SP], VT_64, es->reg_state[Reg_SP]);
        v2.state = CS_DYNAMIC;
        v2.type = VT_64;
        v2.val = instr->addr + instr->len;
        setMemValue(&v2, &addr, es, VT_64, 1);

        es->ret_stack[es->depth++] = v2.val;

        // address to jump to
        return v1.val;

    case IT_CLTQ:
        switch(instr->vtype) {
        case VT_32:
            es->reg[Reg_AX] = (int32_t) (int16_t) es->reg[Reg_AX];
            break;
        case VT_64:
            es->reg[Reg_AX] = (int64_t) (int32_t) es->reg[Reg_AX];
            break;
        default: assert(0);
        }
        if (!stateIsStatic(es->reg_state[Reg_AX]))
            capture(c, instr);
        break;

    case IT_CMP:
        getOpValue(&v1, es, &(instr->dst));
        getOpValue(&v2, es, &(instr->src));

        vt = opValType(&(instr->dst));
        // sign-extend src/v2 if needed
        if (instr->src.type == OT_Imm8) {
            // sign-extend to 64bit (may be cutoff later)
            v2.val = (int64_t) (int8_t) v2.val;
            v2.type = (vt == VT_32) ? OT_Imm32 : OT_Imm64;
        }
        s = setFlagsSub(es, &v1, &v2);
        captureCmp(c, instr, es, s);
        break;

    case IT_JE:
        assert(es->flag_state[FT_Zero] == CS_STATIC);
        if (es->flag[FT_Zero] == True) return instr->dst.val;
        return instr->addr + instr->len;

    case IT_JNE:
        if (es->flag_state[FT_Zero] == CS_STATIC) {
            if (es->flag[FT_Zero] == False) return instr->dst.val;
            return instr->addr + instr->len;
        }
        return captureExit(c, IT_JNE, instr->dst.val, instr->addr + instr->len);

    case IT_JLE:
        assert(es->flag_state[FT_Zero] == CS_STATIC);
        assert(es->flag_state[FT_Sign] == CS_STATIC);
        if ((es->flag[FT_Zero] == True) || (es->flag[FT_Sign] == True)) return instr->dst.val;
        return instr->addr + instr->len;

    case IT_JG:
        assert(es->flag_state[FT_Zero] == CS_STATIC);
        assert(es->flag_state[FT_Sign] == CS_STATIC);
        if ((es->flag[FT_Zero] == False) && (es->flag[FT_Sign] == False)) return instr->dst.val;
        return instr->addr + instr->len;

    case IT_JP:
        // FIXME: assume P flag always cleared => fall through
        return instr->addr + instr->len;

    case IT_JMP:
        assert(instr->dst.type == OT_Imm64);

        // address to jump to
        return instr->dst.val;

    case IT_LEA:
        switch(instr->dst.type) {
        case OT_Reg32:
        case OT_Reg64:
            assert(opIsInd(&(instr->src)));
            getOpAddr(&v1, es, &(instr->src));
            if (opValType(&(instr->dst)) == VT_32) {
                v1.val = (uint32_t) v1.val;
                v1.type = VT_32;
            }
            captureLea(c, instr, es, &v1);
            // may overwrite a state needed for correct capturing
            setOpValue(&v1, es, &(instr->dst));
            break;

        default:assert(0);
        }
        break;

    case IT_LEAVE: {
        // leave = mov rbp,rsp + pop rbp

        Instr i;
        // mov rbp,rsp
        initSimpleInstr(&i, IT_MOV);
        copyOperand( &(i.src), getRegOp(VT_64, Reg_BP) );
        copyOperand( &(i.dst), getRegOp(VT_64, Reg_SP) );
        getOpValue(&v1, es, &(i.src));
        setOpValue(&v1, es, &(i.dst));
        captureMov(c, &i, es, &v1);
        // pop rbp
        initUnaryInstr(&i, IT_POP, getRegOp(VT_64, Reg_BP));
        addr = emuValue(es->reg[Reg_SP], VT_64, es->reg_state[Reg_SP]);
        getMemValue(&v1, &addr, es, VT_64, 1);
        setOpValue(&v1, es, &(i.dst));
        es->reg[Reg_SP] += 8;
        if (!stateIsStatic(v1.state))
            capture(c, &i);
        break;
    }

    case IT_MOV:
    case IT_MOVSX: // converting move
        switch(instr->src.type) {
        case OT_Reg32:
        case OT_Ind32:
        case OT_Imm32: {
            ValType dst_t = opValType(&(instr->dst));
            assert(dst_t == VT_32 || dst_t == VT_64);
            getOpValue(&v1, es, &(instr->src));
            if (dst_t == VT_64) {
                // also a regular mov may sign-extend: imm32->64
                // assert(instr->type == IT_MOVSX);
                // sign extend lower 32 bit to 64 bit
                v1.val = (int64_t) (int32_t) v1.val;
                v1.type = VT_64;
            }
            captureMov(c, instr, es, &v1);
            setOpValue(&v1, es, &(instr->dst));
            break;
        }

        case OT_Reg64:
        case OT_Ind64:
        case OT_Imm64:
            assert(opValType(&(instr->dst)) == VT_64);
            getOpValue(&v1, es, &(instr->src));
            captureMov(c, instr, es, &v1);
            setOpValue(&v1, es, &(instr->dst));
            break;

        default:assert(0);
        }
        break;

    case IT_NOP:
        // nothing to do
        break;

    case IT_NEG:
        getOpValue(&v1, es, &(instr->dst));
        switch(instr->dst.type) {
        case OT_Reg32:
        case OT_Ind32:
            v1.val = (uint64_t)(- ((int64_t) v1.val));
            break;


        case OT_Reg64:
        case OT_Ind64:
            v1.val = (uint32_t)(- ((int32_t) v1.val));
            break;

        default:assert(0);
        }
        captureNeg(c, instr, es, &v1);
        setOpValue(&v1, es, &(instr->dst));
        break;


    case IT_POP:
        switch(instr->dst.type) {
        case OT_Reg32:
            addr = emuValue(es->reg[Reg_SP], VT_64, es->reg_state[Reg_SP]);
            getMemValue(&v1, &addr, es, VT_32, 1);
            setOpValue(&v1, es, &(instr->dst));
            es->reg[Reg_SP] += 4;
            if (!stateIsStatic(v1.state))
                capture(c, instr);
            break;

        case OT_Reg64:
            addr = emuValue(es->reg[Reg_SP], VT_64, es->reg_state[Reg_SP]);
            getMemValue(&v1, &addr, es, VT_64, 1);
            setOpValue(&v1, es, &(instr->dst));
            es->reg[Reg_SP] += 8;
            if (!stateIsStatic(v1.state))
                capture(c, instr);
            break;

        default: assert(0);
        }
        break;

    case IT_PUSH:
        switch(instr->dst.type) {
        case OT_Reg32:
            es->reg[Reg_SP] -= 4;
            addr = emuValue(es->reg[Reg_SP], VT_64, es->reg_state[Reg_SP]);
            getOpValue(&v1, es, &(instr->dst));
            setMemValue(&v1, &addr, es, VT_32, 1);
            if (!stateIsStatic(v1.state))
                capture(c, instr);
            break;

        case OT_Reg64:
            es->reg[Reg_SP] -= 8;
            addr = emuValue(es->reg[Reg_SP], VT_64, es->reg_state[Reg_SP]);
            getOpValue(&v1, es, &(instr->dst));
            setMemValue(&v1, &addr, es, VT_64, 1);
            if (!stateIsStatic(v1.state))
                capture(c, instr);
            break;

        default: assert(0);
        }
        break;

    case IT_RET:
        es->depth--;
        if (es->depth >= 0) {
            // pop return address from stack
            addr = emuValue(es->reg[Reg_SP], VT_64, es->reg_state[Reg_SP]);
            getMemValue(&v1, &addr, es, VT_64, 1);
            es->reg[Reg_SP] += 8;

            assert(v1.val == es->ret_stack[es->depth]);
            return es->ret_stack[es->depth];
        }
        break;

    case IT_SHL:
        // FIXME: do flags (shifting into CF, set OF)
        getOpValue(&v1, es, &(instr->dst));
        getOpValue(&v2, es, &(instr->src));

        switch(opValType(&(instr->dst))) {
        case VT_32:
            v1.val = (uint32_t) (v1.val << (v2.val & 31));
            break;

        case VT_64:
            v1.val = v1.val << (v2.val & 63);
            break;

        default: assert(0);
        }
        v1.state = combineState(v1.state, v2.state, 0);
        captureBinaryOp(c, instr, es, &v1);
        setOpValue(&v1, es, &(instr->dst));
        break;

    case IT_SUB:
        getOpValue(&v1, es, &(instr->dst));
        getOpValue(&v2, es, &(instr->src));

        vt = opValType(&(instr->dst));
        // sign-extend src/v2 if needed
        if (instr->src.type == OT_Imm8) {
            // sign-extend to 64bit (may be cutoff later)
            v2.val = (int64_t) (int8_t) v2.val;
            v2.type = (vt == VT_32) ? OT_Imm32 : OT_Imm64;
        }

        setFlagsSub(es, &v1, &v2);
        assert(v1.type == v2.type);

        switch(vt) {
        case VT_32:
            v1.val = ((uint32_t) v1.val - (uint32_t) v2.val);
            break;

        case VT_64:
            v1.val = v1.val - v2.val;
            break;

        default: assert(0);
        }

        v1.state = combineState(v1.state, v2.state, 0);
        // for capturing we need state of original dst, do before setting dst
        captureBinaryOp(c, instr, es, &v1);
        setOpValue(&v1, es, &(instr->dst));
        break;

    case IT_TEST:
        getOpValue(&v1, es, &(instr->dst));
        getOpValue(&v2, es, &(instr->src));

        assert(v1.type == v2.type);
        s = setFlagsBit(es, IT_AND, &v1, &v2, False);
        captureTest(c, instr, es, s);
        break;

    case IT_XOR:
        getOpValue(&v1, es, &(instr->dst));
        getOpValue(&v2, es, &(instr->src));

        assert(v1.type == v2.type);
        v1.state = setFlagsBit(es, IT_XOR, &v1, &v2,
                               opsAreSame(&(instr->dst), &(instr->src)));
        v1.val = v1.val ^ v2.val;
        // for capturing we need state of original dst
        captureBinaryOp(c, instr, es, &v1);
        setOpValue(&v1, es, &(instr->dst));
        break;


    default: assert(0);
    }
    return 0;
}


uint64_t rewrite(Rewriter* c, ...)
{
    // calling convention x86-64: parameters are stored in registers
    static Reg parReg[5] = { Reg_DI, Reg_SI, Reg_DX, Reg_CX, Reg_8 };

    int i, esID;
    uint64_t par[5];
    EmuState* es;
    DBB *dbb;
    CBB *cbb;
    Instr* instr;
    uint64_t bb_addr, nextbb_addr;

    // setup int parameters for virtual CPU according to x86_64 calling conv.
    // see https://en.wikipedia.org/wiki/X86_calling_conventions
    asm("mov %%rsi, %0;" : "=r" (par[0]) : );
    asm("mov %%rdx, %0;" : "=r" (par[1]) : );
    asm("mov %%rcx, %0;" : "=r" (par[2]) : );
    asm("mov %%r8, %0;"  : "=r" (par[3]) : );
    asm("mov %%r9, %0;"  : "=r" (par[4]) : );

    if (!c->es)
        c->es = allocEmuState(1024);
    resetEmuState(c->es);
    es = c->es;

    resetCapturing(c);
    if (c->cs)
        c->cs->used = 0;

    for(i=0;i<5;i++) {
        es->reg[parReg[i]] = par[i];
        es->reg_state[parReg[i]] = c->cc ? c->cc->par_state[i] : CS_DYNAMIC;
    }

    es->reg[Reg_SP] = (uint64_t) (es->stackStart + es->stackSize);
    es->reg_state[Reg_SP] = CS_STACKRELATIVE;
    es->depth = 0;

    if (c->showEmuState) {
        es->reg[Reg_IP] = c->func;
        printEmuState(es);
    }

    // queue BB at c->func for being captured
    esID = saveEmuState(c);
    cbb = allocCaptureBB(c, c->func, esID);
    pushCaptureBB(c, cbb);
    assert(c->capStackTop == 0);

    // and start with this CBB
    bb_addr = cbb->dec_addr;
    c->currentCapBB = cbb;
    if (c->showEmuSteps)
        printf("Tracing BB (%lx|%d), capture stack at %d ...\n",
               cbb->dec_addr, cbb->esID, c->capStackTop);

    while(1) {
        if (c->currentCapBB == 0) {
            while(c->capStackTop >= 0) {
                cbb = c->capStack[c->capStackTop];
                if (cbb->count == 0) break;
                // cbb already handled; go to previous item on capture stack
                c->capStackTop--;
            }
            // all paths captured?
            if (c->capStackTop < 0) break;

            assert(cbb != 0);
            assert(cbb->count == 0); // should have no instructions yet
            restoreEmuState(c, cbb->esID);
            bb_addr = cbb->dec_addr;
            c->currentCapBB = cbb;

            if (c->showEmuSteps)
                printf("Tracing BB (%lx|%d), capture stack at %d ...\n",
                       cbb->dec_addr, cbb->esID, c->capStackTop);
        }

        dbb = decodeBB(c, bb_addr);
        for(i = 0; i < dbb->count; i++) {
            instr = dbb->instr + i;

            if (c->showEmuSteps)
                printf("Emulate '%p: %s'\n",
                       (void*) instr->addr, instr2string(instr, 0));

            // for RIP-relative accesses
            es->reg[Reg_IP] = instr->addr + instr->len;

            nextbb_addr = emulateInstr(c, es, instr);

            if (c->showEmuState) {
                if (nextbb_addr != 0) es->reg[Reg_IP] = nextbb_addr;
                printEmuState(es);
            }

            if (nextbb_addr != 0) break;
        }
        if (i == dbb->count) {
            // fall through at end of BB
            nextbb_addr = instr->addr + instr->len;
        }
        if (es->depth < 0) {
            // finish this path
            assert(instr->type == IT_RET);
            captureRet(c, instr, es);

            // go to next path to trace
            cbb = popCaptureBB(c);
            cbb->endType = IT_RET;
        }
        bb_addr = nextbb_addr;
    }

    // FIXME: Generating code for multiple CBBs wrong
    for(i=0; i < c->capBBCount; i++)
        generate(c, c->capBB + i);

    // return value according to calling convention
    return es->reg[Reg_AX];
}

