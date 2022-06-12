/*
 *  Copyright (c) 2019 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

#include <kernel/streamutils/run_index.h>
#include <kernel/core/kernel_builder.h>
#include <pablo/builder.hpp>
#include <pablo/pe_ones.h>          // for Ones
#include <pablo/pe_var.h>           // for Var
#include <pablo/pe_zeroes.h>        // for Zeroes
#include <pablo/pe_infile.h>

#include <pablo/bixnum/bixnum.h>

using namespace pablo;

namespace kernel {

Bindings RunIndexOutputBindings(StreamSet * runIndex, StreamSet * overflow) {
    if (overflow == nullptr) return {Binding{"runIndex", runIndex}};
    return {Binding{"runIndex", runIndex}, Binding{"overflow", overflow}};
}
    
RunIndex::RunIndex(BuilderRef b,
                   StreamSet * const runMarks, StreamSet * runIndex, StreamSet * overflow, Kind kind, bool ztfFlag, Numbering n)
    : PabloKernel(b, "RunIndex-" + std::to_string(runIndex->getNumElements()) + (overflow == nullptr ? "" : "overflow") + (kind == Kind::RunOf0 ? "" : "_invert") + (n == Numbering::RunOnly ? "" : "_add1"),
           // input
{Binding{"runMarks", runMarks, FixedRate(), LookAhead(1)}},
           // output
RunIndexOutputBindings(runIndex, overflow)),
mIndexCount(runIndex->getNumElements()),
mOverflow(overflow != nullptr),
mRunKind(kind),
mZtfFlag(ztfFlag),
mNumbering(n) {
    assert(mIndexCount > 0);
    assert(mIndexCount <= 5);
}

void RunIndex::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    Var * runMarksVar = pb.createExtract(getInputStreamVar("runMarks"), pb.getInteger(0));
    PabloAST * runMarks = mRunKind == Kind::RunOf0 ? pb.createInFile(pb.createNot(runMarksVar)) : runMarksVar;
    PabloAST * runStart = nullptr;
    PabloAST * selectZero = nullptr;
    PabloAST * outputEnable = nullptr;
    PabloAST * overflowInit = nullptr;
    if (mZtfFlag) {
        overflowInit = runMarks;
        if (mNumbering == Numbering::RunPlus1) {
            runStart = pb.createNot(runMarks);
            runMarks = pb.createLookahead(runMarks, 1);
        }
        else {
            runStart = pb.createAnd(runMarks, pb.createNot(pb.createAdvance(runMarks, 1)), "runStart");
        }
        /*
                                            hello world, this is a fine morning hello world, how are you?
                e2 22 04 10 10 a4 20 41    1.....1......1....1..1.1....1.......1.....1......1...1...1...

                15 54 aa aa a5 49 4a aa    .1.1.1.1.1.1..1.1..1..1.1.1..1.1.1.1.1.1.1.1.1.1..1.1.1.1.1.1
                19 99 30 cc c6 11 93 0c    ..11....11..1..11...1....11...11..11..11....11..1..11..11..11
                00 01 c3 0f 08 02 1c 30    ....11....111....1.........1....1111....11....111............
        */
        // pb.createDebugPrint(runStart, "runStart");
        // pb.createDebugPrint(runMarks, "runMarks");
        selectZero = mRunKind == Kind::RunOf0 ? pb.createInFile(pb.createNot(runMarksVar)) : runMarksVar;
        outputEnable = selectZero;
    }
    else {
        runStart = pb.createAnd(runMarks, pb.createNot(pb.createAdvance(runMarks, 1)), "runStart");
        selectZero = runMarks;
        outputEnable = runMarks;
    }
    Var * runIndexVar = getOutputStreamVar("runIndex");
    std::vector<PabloAST *> runIndex(mIndexCount);
    PabloAST * even = nullptr;
    PabloAST * overflow = nullptr;
    if (mOverflow) {
        // marks overflowLen + 2 positions
        if (mZtfFlag) overflow = overflowInit;
        else overflow = pb.createAnd(pb.createAdvance(runMarks, 1), runMarks);
        //pb.createDebugPrint(overflow, "overflow-RI");
    }
    for (unsigned i = 0; i < mIndexCount; i++) {
        switch (i) {
            case 0: even = pb.createRepeat(1, pb.getInteger(0x55, 8)); break;
            case 1: even = pb.createRepeat(1, pb.getInteger(0x33, 8)); break;
            case 2: even = pb.createRepeat(1, pb.getInteger(0x0F, 8)); break;
            case 3: even = pb.createRepeat(1, pb.getInteger(0x00FF, 16)); break;
            case 4: even = pb.createRepeat(1, pb.getInteger(0x0000FFFF, 32)); break;
            case 5: even = pb.createRepeat(1, pb.getInteger(0x00000000FFFFFFFF, 64)); break;
        }
        PabloAST * odd = pb.createNot(even);
        PabloAST * evenStart = pb.createAnd(even, runStart);
        PabloAST * oddStart = pb.createAnd(odd, runStart);
        PabloAST * idx = pb.createOr(pb.createAnd(pb.createMatchStar(evenStart, runMarks), odd),
                                     pb.createAnd(pb.createMatchStar(oddStart, runMarks), even));
        if (mNumbering == Numbering::RunOnly || mZtfFlag) {
            idx = pb.createAnd(idx, selectZero);
        }
        for (unsigned j = 0; j < i; j++) {
            idx = pb.createOr(idx, pb.createAdvance(idx, 1<<j));
        }
        runIndex[i] = pb.createAnd(idx, outputEnable, "runidx[" + std::to_string(i) + "]");
        pb.createAssign(pb.createExtract(runIndexVar, pb.getInteger(i)), runIndex[i]);
        if (i < mIndexCount - 1) {
            selectZero = pb.createAnd(selectZero, pb.createNot(idx), "selectZero");
            outputEnable = pb.createAnd(outputEnable, pb.createAdvance(outputEnable, 1<<i), "outputEnable");
        }
        if (mOverflow) {
            overflow = pb.createAnd(overflow, pb.createAdvance(overflow, 1<<i), "overflow");
        }
    }
    if (mOverflow) {
        pb.createAssign(pb.createExtract(getOutputStreamVar("overflow"), pb.getInteger(0)), overflow);
    }
}

Bindings AccumRunIndexOutputBindings(StreamSet * accumRunIndex, StreamSet * accumOverflow) {
    if (accumOverflow == nullptr) return {Binding{"accumRunIndex", accumOverflow}};
    return {Binding{"accumRunIndex", accumRunIndex}, Binding{"accumOverflow", accumOverflow}};
}

AccumRunIndex::AccumRunIndex(BuilderRef b, unsigned offset,
                   StreamSet * const runMarks, StreamSet * runIndex, StreamSet * overflow, StreamSet * accumRunIndex, StreamSet * accumOverflow)
    : PabloKernel(b, "AccumRunIndex-" + std::to_string(runIndex->getNumElements()) + (overflow == nullptr ? "" : "overflow"),
           // input
{Binding{"runMarks", runMarks, FixedRate(), LookAhead(1)}, Binding{"runIndex", runIndex}, Binding{"overflow", overflow}},
           // output
AccumRunIndexOutputBindings(accumRunIndex, accumOverflow)),
mIndexCount(accumRunIndex->getNumElements()),
mOffset(offset),
mOverflow(accumOverflow != nullptr) {
    assert(mIndexCount > 0);
    assert(mIndexCount <= 5);
}

void AccumRunIndex::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * runMarks = pb.createLookahead(getInputStreamSet("runMarks")[0], 1);
    PabloAST * symEndPos = pb.createNot(runMarks);
    std::vector<PabloAST *> runIndex = getInputStreamSet("runIndex");
    PabloAST * overflow = pb.createExtract(getInputStreamVar("overflow"), pb.getInteger(0));
    // symbol is already marked for overflow or is following an overflow mark
    PabloAST * curOverflow = pb.createOr(overflow, pb.createIndexedAdvance(overflow, symEndPos, 1));
    overflow = pb.createNot(overflow);
    // output
    std::vector<PabloAST *> accumRunIndex(mIndexCount);
    Var * accumRunIndexVar = getOutputStreamVar("accumRunIndex");
    Var * accumOverflowVar = getOutputStreamVar("accumOverflow");
    BixNumCompiler bnc(pb);
    BixNum curSymLen(mIndexCount);
    BixNum prevSymLen(mIndexCount);

    for (unsigned i = 0; i < mIndexCount; i++) {
        // check ri[i] against symEndPos; set 0 if symEndPos is 0 => all the RI bixnum prior to sym end set to 0
        runIndex[i] = pb.createAnd(runIndex[i], symEndPos);
    }
    //propagte last sym's length to cur sym stopping right before next sym's start pos
    for (unsigned int i = 0; i < 32; i++) { // max symbol byte_length restricted to 32 (allow initializng at compile time)
        for (unsigned ii = 0; ii < mIndexCount; ii++) {
            PabloAST * priorBits = pb.createAdvance(runIndex[ii], 1);
            priorBits = pb.createAnd3(priorBits, runMarks, overflow);
            runIndex[ii] = pb.createOr(runIndex[ii], priorBits);
        }
    }
    // remove 1 byte symbols like LF -> mark consecutive 1-byte sym
    PabloAST * byteLenSym = pb.createAnd(symEndPos, pb.createAdvance(symEndPos, 1));
    // IndexedAdvance by consecutive k-1 positions to mark invalid k-symbol phrase positions
    // -> symbol preceded by a 1-byte symbol; Eg: 1world (skip these so that the length can be safely calculated)
    // TODO : change indexedAdvance idx to calculate RI for k-symbol phrases; k > 2
    byteLenSym = pb.createIndexedAdvance(byteLenSym, symEndPos, 1);
    // eliminate single-byte symbols
    symEndPos = pb.createAnd(symEndPos, pb.createXor(symEndPos, pb.createAdvance(symEndPos, 1)));
    for (unsigned i = 0; i < mIndexCount; i++) {
        curSymLen[i] = pb.createAnd(runIndex[i], symEndPos);
        prevSymLen[i] = pb.createAnd(pb.createAdvance(runIndex[i], 1), symEndPos);
    }
    // first symbol has no prev symbol
    // IndexedAdvance by consecutive k-1 positions to mark invalid k-gram phrase positions
    PabloAST * notFirstSymSum = pb.createIndexedAdvance(symEndPos, symEndPos, 1);
    //pb.createDebugPrint(notFirstSymSum, "notFirstSymSum");
    PabloAST * inRangeFinal = pb.createAnd(pb.createNot(byteLenSym), notFirstSymSum);
    // add k for k-symbol phrase length calculation
    BixNum sum = bnc.AddModular(bnc.AddModular(prevSymLen, curSymLen), mOffset); //bnc.AddModular(prevSymLen, curSymLen);
    curOverflow = pb.createOr(curOverflow, bnc.ULT(sum, curSymLen));
    //pb.createDebugPrint(curOverflow, "curOverflow");
    inRangeFinal = pb.createAnd(inRangeFinal, pb.createXor(inRangeFinal, curOverflow));
    //pb.createDebugPrint(inRangeFinal, "inRangeFinal");
    //pb.createDebugPrint(pb.createAnd(symEndPos, inRangeFinal), "included");
    for (unsigned i = 0; i < mIndexCount; i++) {
        // if either of the operands is 0, ignore the sum in final run -> avoids 1-byte symbols like LF
        pb.createAssign(pb.createExtract(accumRunIndexVar, pb.getInteger(i)), pb.createAnd3(sum[i], symEndPos, inRangeFinal));
    }
    pb.createAssign(pb.createExtract(accumOverflowVar, pb.getInteger(0)), curOverflow);

}

Bindings AccumRunIndexNewOutputBindings(StreamSet * phraseRunIndex, StreamSet * phraseOverflow) {
    if (phraseOverflow == nullptr) return {Binding{"phraseRunIndex", phraseOverflow}};
    return {Binding{"phraseRunIndex", phraseRunIndex}, Binding{"phraseOverflow", phraseOverflow}};
}

AccumRunIndexNew::AccumRunIndexNew(BuilderRef b, unsigned numSym,
                                   StreamSet * const runMarks, StreamSet * runIndex, StreamSet * overflow, StreamSet * phraseRunIndex, StreamSet * phraseOverflow)
    : PabloKernel(b, "AccumRunIndexNew-" + std::to_string(runIndex->getNumElements()) + (overflow == nullptr ? "" : "overflow"),
           // input
{Binding{"runMarks", runMarks, FixedRate(), LookAhead(1)}, Binding{"runIndex", runIndex, FixedRate(), LookAhead(1)}, Binding{"overflow", overflow}},
           // output
AccumRunIndexNewOutputBindings(phraseRunIndex, phraseOverflow)),
mIndexCount(phraseRunIndex->getNumElements() - 3),
mNumSym(numSym),
mOverflow(phraseOverflow != nullptr) {
    assert(mIndexCount <= 5);
}

void AccumRunIndexNew::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    PabloAST * runMarks = pb.createLookahead(getInputStreamSet("runMarks")[0], 1);
    PabloAST * symEndPos = pb.createNot(runMarks);
    std::vector<PabloAST *> runIndex = getInputStreamSet("runIndex");
    PabloAST * overflow = pb.createExtract(getInputStreamVar("overflow"), pb.getInteger(0));
    // symbol is already marked for overflow or is following an overflow mark
    PabloAST * curOverflow = pb.createOr(overflow, pb.createIndexedAdvance(overflow, symEndPos, 1));
    overflow = pb.createNot(overflow);
    // output
    std::vector<PabloAST *> phraseRunIndex(mIndexCount);
    Var * phraseRunIndexVar = getOutputStreamVar("phraseRunIndex");
    Var * phraseOverflowVar = getOutputStreamVar("phraseOverflow");
    BixNumCompiler bnc(pb);
    BixNum curSymLen(mIndexCount);
    BixNum prevSymLen(mIndexCount);
    for (unsigned i = 0; i < mIndexCount; i++) {
        curSymLen[i] = pb.createLookahead(runIndex[i], 1);
        // check ri[i] against symEndPos; set 0 if symEndPos is 0 => all the RI bixnum prior to symEndPos set to 0
        runIndex[i] = pb.createAnd(runIndex[i], symEndPos);
    }
    //propagte last sym's length to cur sym stopping right before next sym's start pos
    for (unsigned int i = 0; i < 32; i++) { // max symbol byte_length restricted to 32 (allow initializng at compile time)
        for (unsigned ii = 0; ii < mIndexCount; ii++) {
            PabloAST * priorBits = pb.createAdvance(runIndex[ii], 1);
            priorBits = pb.createAnd3(priorBits, runMarks, overflow);
            runIndex[ii] = pb.createOr(runIndex[ii], priorBits);
        }
    }
    // previous symbol length is advanced to be aligned with the next symbol last byte
    for (unsigned i = 0; i < mIndexCount; i++) {
        prevSymLen[i] = pb.createAnd(pb.createAdvance(runIndex[i], 1), symEndPos);
    }
    // first symbol has no prev symbol
    // IndexedAdvance by consecutive k-1 positions to mark invalid k-gram phrase positions
    PabloAST * notFirstSymSum = pb.createIndexedAdvance(symEndPos, symEndPos, 1);
    PabloAST * inRangeFinal = notFirstSymSum;

    // for 3-sym phrases, 2 and 3-sym phrases got to be incremented by 1 and 2 respectively
    BixNum sum = bnc.AddModular(bnc.AddModular(prevSymLen, runIndex), (mNumSym-1)); //bnc.AddModular(prevSymLen, runIndex);
    curOverflow = pb.createOr(curOverflow, bnc.ULT(sum, curSymLen));
    inRangeFinal = pb.createAnd(inRangeFinal, pb.createXor(inRangeFinal, curOverflow));
    for (unsigned i = 0; i < mIndexCount; i++) {
        pb.createAssign(pb.createExtract(phraseRunIndexVar, pb.getInteger(i)), pb.createOr(curSymLen[i], pb.createAnd(sum[i], inRangeFinal)));
    }
    for (unsigned sym = 0; sym < mNumSym; sym++) {
        PabloAST * ones = pb.createOnes();
        if (sym == 0) {
            pb.createAssign(pb.createExtract(phraseRunIndexVar, pb.getInteger(5)), pb.createAnd(ones, runMarks));
        }
        else if (sym == 1) {
            pb.createAssign(pb.createExtract(phraseRunIndexVar, pb.getInteger(6)), pb.createAnd(ones, symEndPos));
        }
    }
    pb.createAssign(pb.createExtract(phraseRunIndexVar, pb.getInteger(7)), pb.createZeroes());
    pb.createAssign(pb.createExtract(phraseOverflowVar, pb.getInteger(0)), curOverflow);

}

}
