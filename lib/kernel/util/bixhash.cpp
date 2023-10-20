/*
 *  Copyright (c) 2019 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 */

#include <kernel/util/bixhash.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <vector>
#include <algorithm>
#include <random>

#include <boost/dynamic_bitset/dynamic_bitset.hpp>

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

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

void BixSubHash::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());

    const auto basis = getInputStreamSet("basis");
    const auto n = basis.size();
    PabloAST * run = getInputStreamSet("runs")[0];
    const auto m = getOutputStreamSet(0)->getNumElements();
    std::vector<PabloAST *> hash(m * 2);

    std::mt19937 rng(mSeed);

    BEGIN_SCOPED_REGION

    // We want a roughly even distribution of basis bits amongst the
    // initial hash bit values but to ensure that each hash bit is
    // valuable, no value can equal the XOR of any other two values
    // (including the empty set.)

    // Each hash bit is XOR of k basis bits. To determine k, we start by
    // observing that given n basis bits there are n choose k possible
    // combinations. However, for every pair of choices we add, the XOR
    // of both may remove one from our set of potential choices.

    // Each choice has strictly k unique bits. As such, we only consider the
    // XOR of pairs in which exactly (k/2) bits differ. This means for any
    // even k, for every pair of nodes, we conceptually "select" a path of
    // k/2 length and compute the total number of "different" (k/2)-length
    // pathes in the remaining subgraph. Thus:

    //   (n choose k) - [if k is even, (n - k/2 - 1)(n - k/2 - 2)^(k/2-1) else 0] >= m

    auto power = [](const size_t n, size_t k) -> size_t {
        if (k == 0) {
            return 1UL;
        }
        auto p = n;
        while (--k) {
            p *= n;
        }
        return p;
    };

    std::function<size_t(size_t, size_t)> binomial_coeff = [&](const size_t n, const size_t k) ->size_t {
        if (k == 0 || k == n) {
            return 1UL;
        }
        return binomial_coeff(n - 1, k) + binomial_coeff(n - 1, k - 1);
    };

    // NOTE: since k is likely to be very low, just iterate to find it rather than
    // approximate it.

    size_t K = 1;
    size_t total = 0;
    size_t potential = 0;
    while (K < n) {
        total = binomial_coeff(n, K);
        if ((K & 1) == 0) {
            potential = total - (n - K / 2 - 1) * power(n - K / 2 - 2, K / 2 - 1);
        } else {
            potential = total;
        }
        if (potential >= m) {
            break;
        }
        ++K;
    }

    SmallVector<boost::dynamic_bitset<>, 32> variables(m);

    for (unsigned i = 0; i < m; ++i) {
        auto & v = variables[i];
        assert (v.size() == 0);
        v.resize(n, false);
    }

    SmallVector<size_t, 16> ordering(n);
    std::iota(ordering.begin(), ordering.end(), 0);

restart:

    auto next = ordering.end();

    for (size_t i = 0; i < m; ++i) {

        auto & Vi = variables[i];

        size_t remaining = 16;

retry:

        Vi.reset();

        do {
            if (next == ordering.end()) {
                std::shuffle(ordering.begin(), ordering.end(), rng);
                next = ordering.begin();
            }
            Vi.set(*next++);
        } while (Vi.count() < K);

        if (LLVM_LIKELY(total <= n)) {
            for (size_t j = 0; j < i; ++j) {
                const auto & Vj = variables[j];
                if (Vj == Vi) {
                    goto retry;
                }
                if (LLVM_LIKELY(potential <= n && potential < total)) {
                    for (size_t k = 0; k < j; ++k) {
                        const auto & Vk = variables[k];
                        if ((Vj ^ Vk) == Vi) {
                            if (--remaining) {
                                goto retry;
                            } else {
                                goto restart;
                            }
                        }
                    }
                }
            }
        }
    }

    std::array<PabloAST *, 3> xors;

    for (size_t i = 0; i < m; ++i) {

        const auto & Si = variables[i];
        size_t j = 0;

        auto k = Si.find_first();

        assert (k != boost::dynamic_bitset<>::npos);

        for (;;) {
            xors[j++] = basis[k];
            if (j == 3) {
                xors[0] = pb.createXor3(xors[0], xors[1], xors[2]);
                j = 1;
            }

            k = Si.find_next(k);
            if (k == boost::dynamic_bitset<>::npos) {
                break;
            }
        }
        assert (j > 0);
        if (j == 3) {
            xors[0] = pb.createXor3(xors[0], xors[1], xors[2]);
        } else if (j == 2) {
            xors[0] = pb.createXor(xors[0], xors[1]);
        }
        assert (hash[i] == nullptr);
        hash[i] = xors[0];
    }

    END_SCOPED_REGION

    // In each step, the select stream will mark positions that are
    // to receive bits from prior locations in the symbol.   The
    // select stream must ensure that no bits from outside the symbol
    // are included in the calculated hash value.
    PabloAST * select = run;

    PabloAST * carry = nullptr;

    SmallVector<size_t, 64> mix(m);
    std::iota(mix.begin(), mix.end(), 0);

    // TODO: can we reduce the number of advances without losing hashing quality?

    for (unsigned i = 0; i < mHashSteps; ++i) {

        const auto shft = 1U << i;

        std::shuffle(mix.begin(), mix.end(), rng);

        const auto x = ((i & 1) == 0) ? 0 : m;
        const auto y = (m ^ x);

        for (unsigned j = 0; j < m; ++j) {

            PabloAST * A = hash[x + mix[j]]; assert (A);
            PabloAST * B = hash[x + mix[((j + 1) % m)]]; assert (B);
            B = pb.createAnd(select, pb.createAdvance(B, shft));

            PabloAST * val = nullptr;
            if (carry == nullptr) {
                val = pb.createXor(A, B);
                carry = pb.createAnd(pb.createNot(A), B);
            } else {
                val = pb.createXor3(A, B, carry);
                carry = pb.createMajority3(pb.createNot(A), B, carry);
            }
            assert (hash[y + j] == nullptr);
            hash[y + j] = val;
        }
        #ifndef NDEBUG
        for (unsigned j = 0; j < m; ++j) {
            hash[x + j] = nullptr;
        }
        #endif
        select = pb.createAnd(select, pb.createAdvance(select, shft));
    }

    const auto f = ((mHashSteps & 1) == 0) ? 0 : m;

    Var * hashVar = getOutputStreamVar("hashes");
    for (unsigned i = 0; i < m; i++) {
        PabloAST * const expr = hash[f + i]; assert (expr);
        pb.createAssign(pb.createExtract(hashVar, pb.getInteger(i)), expr);
    }

    // if the value is still in the select span, we did not include it in the hash'ed value.
    PabloAST * const hashSpan = pb.createAnd(run, pb.createNot(select));

    pb.createAssign(pb.createExtract(getOutputStreamVar("hashSpan"), pb.getInteger(0)), hashSpan);


}

}
