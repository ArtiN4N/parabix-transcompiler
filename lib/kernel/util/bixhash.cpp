/*
 *  Copyright (c) 2019 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

#include <kernel/util/bixhash.h>
#include <pablo/builder.hpp>
#include <vector>
#include <algorithm>
#include <random>

using namespace llvm;
using namespace pablo;

namespace kernel {

    
void BixHash::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());

    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    PabloAST * run = getInputStreamSet("run")[0];
    std::vector<PabloAST *> hash(mHashBits);
    // For every byte we create an in-place hash, in which each bit
    // of the byte is xor'd with one other bit.
    std::vector<unsigned> bitmix(mHashBits);
    const auto m = basis.size();
    for (unsigned i = 0; i < mHashBits; ++i) {
        bitmix[i] = i % m;
    }

    std::mt19937 rng(mSeed);

    if (mHashBits > 0 && m > 0) {
 retry_shuffle:
        std::shuffle (bitmix.begin(), bitmix.end(), rng);
        for (unsigned i = 0; i < mHashBits; i++) {
            // avoid XOR-ing a value with itself
            if ((i % m) == bitmix[i]) {
                goto retry_shuffle;
            }
        }
    }

    for (unsigned i = 0; i < mHashBits; i++) {
        hash[i] = pb.createXor(basis[i % m], basis[bitmix[i]]);
    }

    // In each step, the select stream will mark positions that are
    // to receive bits from prior locations in the symbol.   The
    // select stream must ensure that no bits from outside the symbol
    // are included in the calculated hash value.
    PabloAST * select = run;
    for (unsigned j = 0; j < mHashSteps; j++) {
        const auto shft = 1U << j;
        // Select bits from prior positions.
        std::shuffle (bitmix.begin(), bitmix.end(), rng);
        for (unsigned i = 0; i < mHashBits; i++) {
            PabloAST * priorBits = pb.createAdvance(hash[bitmix[i]], shft);
            // Mix in bits from prior positions.
            hash[i] = pb.createXor(hash[i], pb.createAnd(select, priorBits));
        }
        select = pb.createAnd(select, pb.createAdvance(select, shft));
    }
    Var * hashVar = getOutputStreamVar("hashes");
    for (unsigned i = 0; i < mHashBits; i++) {
        pb.createAssign(pb.createExtract(hashVar, pb.getInteger(i)), hash[i]);
    }
}
}
