#include "csv_schema_validator.h"

#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/pe_ones.h>
#include <pablo/pe_var.h>


#include <re/parse/PCRE_parser.h>
#include <re/transforms/re_memoizing_transformer.h>
#include <grep/regex_passes.h>
#include <re/transforms/to_utf8.h>
#include <re/unicode/casing.h>
#include <re/printer/re_printer.h>
#include <re/adt/re_seq.h>
#include <re/unicode/resolve_properties.h>

#include <re/printer/re_printer.h>

#include <re/compile/re_compiler.h>
#include <re/transforms/re_minimizer.h>
#include <boost/container/flat_map.hpp>

#include <kernel/core/streamset.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/IR/Function.h>

#include "csv_validator_util.h"

using namespace kernel;
using namespace re;
using namespace llvm;
using namespace pablo;

namespace csv {

std::string CSVSchemaValidatorKernel::makeCSVSchemaSignature(const csv::CSVSchema & schema) {

    std::string tmp;
    tmp.reserve(1024);
    raw_string_ostream out(tmp);

    out << schema.Separator;

    for (const auto & column : schema.Column) {
//        out << '"';
//        out.write_escaped(column.Name);
//        out << '"';
//        if (column.Optional) {
//            out << 'O';
//        }
        if (column.MatchIsFalse) {
            out << 'N';
        }
//        if (column.IgnoreCase) {
//            out << 'I';
//        }
        if (column.Warning) {
            out << 'W';
        }
        out << '"';
        out.write_escaped(Printer_RE::PrintRE(column.Expression));
        out << '"';
    }

    for (const auto & key : schema.CompositeKey) {
        assert (key.Fields.size() > 0);
        char joiner = '{';
        for (const auto k : key.Fields) {
            assert (k < schema.Column.size());
            out << joiner << k;
            joiner = ',';
        }
        out << '}';
    }

    out.flush();
    return tmp;
}

CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, const csv::CSVSchema & schema, StreamSet * basisBits, StreamSet * fieldData, StreamSet * allSeperators, StreamSet * invalid, CSVSchemaValidatorOptions && options)
: CSVSchemaValidatorKernel(b, schema, makeCSVSchemaSignature(schema), basisBits, fieldData, allSeperators, invalid, std::move(options)) {

}



CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, const csv::CSVSchema & schema, std::string && signature, StreamSet * basisBits, StreamSet * fieldData, StreamSet * allSeperators, StreamSet * invalid, CSVSchemaValidatorOptions && options)
: PabloKernel(b, "csvv" + getStringHash(signature),
    {Binding{"basisBits", basisBits},
     Binding{"fieldData", fieldData, FixedRate(), LookAhead(1)},
     Binding{"allSeperators", allSeperators}},
    {Binding{"invalid", invalid}})
, mSchema(schema)
, mOptions(std::move(options))
, mSignature(std::move(signature)) {
    for (const auto & a : mOptions.mAlphabets) {
        mInputStreamSets.emplace_back(a.first->getName() + "_basis", a.second);
    }
    for (const auto & a : mOptions.mExternalBindings) {
        mInputStreamSets.emplace_back(a);
    }
    if (mOptions.mIndexStream) {
        mInputStreamSets.emplace_back("mIndexing", mOptions.mIndexStream);
    }
    if (schema.CompositeKey.size() > 0) {
        mOutputStreamSets.emplace_back("hashKeyMarkers", mOptions.mKeyMarkers);
        mOutputStreamSets.emplace_back("hashKeyRuns", mOptions.mKeyRuns);
    }
}


StringRef CSVSchemaValidatorKernel::getSignature() const {
    return mSignature;
}

#if 1


void CSVSchemaValidatorKernel::generatePabloMethod() {

    using Mark = RE_Compiler::Marker;

    const auto & columns = mSchema.Column;
    const auto n = columns.size();
    std::vector<RE *> fieldExpr(n);

    RE_MemoizingTransformer memo("memoizer");
    for (unsigned i = 0; i < columns.size(); ++i) {
        const auto & col = columns[i];
        re::RE * expr = col.Expression; assert (expr);
        expr = minimizeRE(expr);
        fieldExpr[i] = expr;
    }



    PabloBuilder pb(getEntryScope());

    Integer * const pb_ZERO = pb.getInteger(0);

    Var * fd = getInputStreamVar("fieldData");

    PabloAST * fieldData = pb.createExtract(fd, pb.getInteger(CSVDataParserFieldData::FieldData));


    Var * const allSeperators = pb.createExtract(getInputStreamVar("allSeperators"), pb_ZERO);

    Var * const recordSeperators = pb.createExtract(fd, pb.getInteger(CSVDataParserFieldData::RecordSeperator));

   // pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {recordSeperators });

 //   PabloAST * const barrier = pb.createOr(allSeperators, pb.createNot(fieldDataMask));

//    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {barrier});

    RE_Compiler re_compiler(getEntryScope(), allSeperators, mOptions.mCodeUnitAlphabet);

    for (unsigned i = 0; i < mOptions.mAlphabets.size(); i++) {
        auto & alpha = mOptions.mAlphabets[i].first;
        auto basis = getInputStreamSet(alpha->getName() + "_basis");
        re_compiler.addAlphabet(alpha, basis);
    }
    if (mOptions.mIndexStream) {
        PabloAST *  indexStream = pb.createExtract(getInputStreamVar("mIndexing"), pb.getInteger(0));
        fieldData = pb.createAnd(fieldData, indexStream);
    }

    re_compiler.setIndexing(&cc::Unicode, fieldData);

    for (unsigned i = 0; i < mOptions.mExternalBindings.size(); i++) {
        auto extName = mOptions.mExternalBindings[i].getName();
        PabloAST * extStrm = pb.createExtract(getInputStreamVar(extName), pb.getInteger(0));
        unsigned offset = mOptions.mExternalOffsets[i];
        std::pair<int, int> lgthRange = mOptions.mExternalLengths[i];
        re_compiler.addPrecompiled(extName, RE_Compiler::ExternalStream(Mark(extStrm, offset), lgthRange));
    }


    auto basisBits = getInputStreamSet("basisBits");

    re_compiler.addAlphabet(mOptions.mCodeUnitAlphabet, basisBits);

    // TODO: if the number of validators equals the number of fields, just scan sequentially? We expect everything to be valid but if the
    // total schema length is "long", we won't necessarily be starting a new record every block. Can we "break up" the validation checks to
    // test if we should scan through a chunk of them based on the current position?

//    Mark startMatch = re_compiler.compileRE(makeStart(), Mark{fieldDataMask, 1}, 1);
//    PabloAST * const allStarts = startMatch.stream();

//    PabloAST * const allStarts = pb.createAnd(pb.createNot(pb.createAdvance(barrier, 1)), fieldDataMask, "allStarts");
//    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {fieldDataMask });
//    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {allStarts} );


    // I expect to see at most one UID (since databases only really support a single primary/composite key)
    // but since the logic here doesn't depend on it, I permit it for multiple independent keys.

    std::vector<bool> usedInSchemaUID(n, false);
    for (const auto & key : mSchema.CompositeKey) {
        for (const auto k : key.Fields) {
            usedInSchemaUID[k] = true;
        }
    }

    FixedArray<PabloAST *, 2> args;
    args[0] = nullptr;

    // TODO: this will fail on a blank line; should probably ignore them

    // If we go through all non separators, we'll go past any trailing ", which we want to skip. But if we don't,
    // we need another scanthru per field to reach the same position

//    PabloAST * starts = pb.createExtract(fd, pb.getInteger(CSVDataParserFieldData::StartPositions));
//    starts = pb.createLookahead(starts, 1);

//    PabloAST * const nonEnds = pb.createInFile(pb.createNot(endPositions), "nonStarts");
    PabloAST * const nonSeperators = pb.createInFile(pb.createNot(allSeperators), "nonSeparators");
    PabloAST * const fieldSeparators = pb.createAnd(allSeperators, pb.createNot(recordSeperators), "fieldSeparators");

//    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {fieldSeparators} );

//    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {allStarts});

    std::vector<PabloAST *> matches(n, nullptr);
    for (unsigned i = 0; i < n; ++i) {
        assert (columns[i].Expression);
        if (matches[i] == nullptr) {

            RE * expr = makeSeq({fieldExpr[i], makeEnd()});

            Mark match = re_compiler.compileRE(expr, Mark(fieldSeparators, 1), 1);
            matches[i] = match.stream();

            for (unsigned j = i + 1; j < n; ++j) {
                if (fieldExpr[j] == fieldExpr[i]) {
                    assert (matches[j] == nullptr);
                    matches[j] = matches[i];
                }
            }

        }
    }


    PabloAST * warning = nullptr;

    PabloAST * currentPos = recordSeperators;
    PabloAST * allSeparatorsMatches = nullptr;

    for (unsigned i = 0; i < n; ++i) {
        PabloAST * const fieldStart = currentPos;

        currentPos = pb.createAdvanceThenScanThru(fieldStart, nonSeperators);

        PabloAST * match = matches[i];

        pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {match});

        pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {currentPos});

        const auto & col = columns[i];
        if (col.MatchIsFalse) {
            match = pb.createNot(match);
        }
        if (LLVM_UNLIKELY(col.Warning)) {
            PabloAST * nextWarning = pb.createAnd(currentPos, pb.createNot(match));
            if (warning) {
                warning = pb.createOr(warning, nextWarning);
            } else {
                warning = nextWarning;
            }
            assert (false);
        } else {
            currentPos = pb.createAnd(currentPos, match);
        }

        currentPos = pb.createScanThru(currentPos, nonSeperators);

        if (i < (n - 1)) {
            currentPos = pb.createAnd(currentPos, fieldSeparators, "matchedFieldSep" + std::to_string(i + 1));
        } else {
            currentPos = pb.createAnd(currentPos, recordSeperators, "matchedRecSep");
        }

         pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {currentPos });


        if (LLVM_UNLIKELY(usedInSchemaUID[i])) {
            if (args[0] == nullptr) {
                args[0] = fieldStart;
                args[1] = currentPos;
            } else {
                args[0] = pb.createOr(args[0], fieldStart);
                args[1] = pb.createOr(args[1], currentPos);
            }
        }

        if (allSeparatorsMatches) {
            allSeparatorsMatches = pb.createOr(allSeparatorsMatches, currentPos);
        } else {
            allSeparatorsMatches = currentPos;
        }
    }

    PabloAST * result = pb.createXor(allSeparatorsMatches, nonSeperators, "result");
    Var * invalid = getOutputStreamVar("invalid");
    pb.createAssign(pb.createExtract(invalid, pb_ZERO), result);

    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {result});

    if (warning) {
        #warning add check here for 2 elements
        pb.createAssign(pb.createExtract(invalid, pb.getInteger(1)), warning);
    }

    if (mSchema.CompositeKey.size() > 0) {
        PabloAST * const keyMarkers = pb.createOr(args[0], args[1]);
        pb.createAssign(pb.createExtract(getOutputStreamVar("hashKeyMarkers"), pb_ZERO), keyMarkers);

        PabloAST * const run = pb.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, args, "run");
        PabloAST * const hashableFieldData = pb.createAnd(run, nonSeperators);
        pb.createAssign(pb.createExtract(getOutputStreamVar("hashKeyRuns"), pb_ZERO), hashableFieldData);
    }


}

#else

void CSVSchemaValidatorKernel::generatePabloMethod() {

    const auto & columns = mSchema.Column;
    const auto n = columns.size();
    std::vector<RE *> fieldExpr(n);

    RE_MemoizingTransformer memo("memoizer");
    for (unsigned i = 0; i < columns.size(); ++i) {
        const auto & col = columns[i];
        re::RE * expr = col.Expression; assert (expr);
        fieldExpr[i] = memo.transformRE(expr);
    }

    PabloBuilder pb(getEntryScope());

    Var * const recordSeperators = pb.createExtract(getInputStreamVar("recordSeperators"), pb.getInteger(0));

    RE_Compiler re_compiler(getEntryScope(), recordSeperators, mOptions.mCodeUnitAlphabet);

    for (unsigned i = 0; i < mOptions.mAlphabets.size(); i++) {
        auto & alpha = mOptions.mAlphabets[i].first;
        auto basis = getInputStreamSet(alpha->getName() + "_basis");
        re_compiler.addAlphabet(alpha, basis);
    }

    PabloAST * indexStream = nullptr;
    if (mOptions.mIndexStream) {
        indexStream = pb.createExtract(getInputStreamVar("mIndexing"), pb.getInteger(0));
        re_compiler.setIndexing(&cc::Unicode, indexStream);
    }
    for (unsigned i = 0; i < mOptions.mExternalBindings.size(); i++) {
        auto extName = mOptions.mExternalBindings[i].getName();
        PabloAST * extStrm = pb.createExtract(getInputStreamVar(extName), pb.getInteger(0));
        unsigned offset = mOptions.mExternalOffsets[i];
        std::pair<int, int> lgthRange = mOptions.mExternalLengths[i];
        re_compiler.addPrecompiled(extName, RE_Compiler::ExternalStream(RE_Compiler::Marker(extStrm, offset), lgthRange));
    }

    auto basisBits = getInputStreamSet("basisBits");

    Integer * const pb_ZERO = pb.getInteger(0);

 //   Var * const UTFindex = pb.createExtract(getInputStreamVar("UTFindex"), pb_ZERO);
 //   pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {UTFindex });

    Var * const fieldDataMask = pb.createExtract(getInputStreamVar("fieldData"), pb_ZERO);
    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {fieldDataMask });
    assert (fieldDataMask->getType() == getStreamTy());
    PabloAST * textIndex = fieldDataMask;
    if (indexStream) {
        textIndex = pb.createAnd(fieldDataMask, indexStream, "textIndex");
    }

    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {textIndex });

    re_compiler.setIndexing(mOptions.mCodeUnitAlphabet, textIndex);

    re_compiler.addAlphabet(mOptions.mCodeUnitAlphabet, basisBits);

    Var * const allSeperators = pb.createExtract(getInputStreamVar("allSeperators"), pb_ZERO);

//    re_compiler.addPrecompiled(FieldSepName, RE_Compiler::ExternalStream(RE_Compiler::Marker(allSeperators, 1), std::make_pair<int,int>(1, 1)));

    // TODO: if the number of validators equals the number of fields, just scan sequentially? We expect everything to be valid but if the
    // total schema length is "long", we won't necessarily be starting a new record every block. Can we "break up" the validation checks to
    // test if we should scan through a chunk of them based on the current position?

    using Mark = RE_Compiler::Marker;

    std::vector<PabloAST *> matches(n, nullptr);
    for (unsigned i = 0; i < n; ++i) {
        assert (columns[i].Expression);
        if (matches[i] == nullptr) {

            errs() << "I: " << i << " -> ";

            errs() << Printer_RE::PrintRE(fieldExpr[i]);

            errs() << "\n\n";

            Mark match = re_compiler.compileRE(fieldExpr[i], Mark{textIndex, 1}, 1);
            matches[i] = match.stream();

            pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {matches[i]  });

            for (unsigned j = i + 1; j < n; ++j) {
                if (fieldExpr[j] == fieldExpr[i]) {
                    assert (matches[j] == nullptr);
                    matches[j] = matches[i];
                }
            }


        }
    }

    RE_Compiler::Marker startMatch = re_compiler.compileRE(makeStart());
    PabloAST * const allStarts = startMatch.stream();

    PabloAST * currentPos = allStarts;
    PabloAST * allSeparatorsMatches = nullptr;

    PabloAST * const nonSeparators = pb.createInFile(pb.createNot(allSeperators), "nonSeperators");
    PabloAST * const fieldSeparators = pb.createXor(allSeperators, recordSeperators, "fieldSeparators");

    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {fieldSeparators} );

    // I expect to see at most one UID (since databases only really support a single primary/composite key)
    // but since the logic here doesn't depend on it, I permit it for multiple independent keys.

    std::vector<bool> usedInSchemaUID(n, false);
    for (const auto & key : mSchema.CompositeKey) {
        for (const auto k : key.Fields) {
            usedInSchemaUID[k] = true;
        }
    }

    FixedArray<PabloAST *, 2> args;
    args[0] = nullptr;

    // TODO: this will fail on a blank line; should probably ignore them

    // If we go through all non separators, we'll go past any trailing ", which we want to skip. But if we don't,
    // we need another scanthru per field to reach the same position

  //  pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {allStarts});

    PabloAST * warning = nullptr;

    for (unsigned i = 0; i < n; ++i) {
        PabloAST * const fieldStart = currentPos;
        currentPos = pb.createAdvanceThenScanThru(fieldStart, nonSeparators);

        PabloAST * match = matches[i];

        const auto & col = columns[i];
        if (col.MatchIsFalse) {
            match = pb.createNot(match);
        }
        if (LLVM_UNLIKELY(col.Warning)) {
            PabloAST * nextWarning = pb.createAnd(currentPos, pb.createNot(match));
            if (warning) {
                warning = pb.createOr(warning, nextWarning);
            } else {
                warning = nextWarning;
            }
        } else {
            currentPos = pb.createAnd(currentPos, matches[i]);
        }
        if (i < (n - 1)) {
            currentPos = pb.createAnd(currentPos, fieldSeparators, "matchedFieldSep" + std::to_string(i + 1));
        } else {
            currentPos = pb.createAnd(currentPos, recordSeperators, "matchedRecSep");
        }

        if (LLVM_UNLIKELY(usedInSchemaUID[i])) {
            if (args[0] == nullptr) {
                args[0] = fieldStart;
                args[1] = currentPos;
            } else {
                args[0] = pb.createOr(args[0], fieldStart);
                args[1] = pb.createOr(args[1], currentPos);
            }
        }

        if (allSeparatorsMatches) {
            allSeparatorsMatches = pb.createOr(allSeparatorsMatches, currentPos);
        } else {
            allSeparatorsMatches = currentPos;
        }
    }

    PabloAST * result = pb.createXor(allSeparatorsMatches, allSeperators, "result");
    Var * invalid = getOutputStreamVar("invalid");
    pb.createAssign(pb.createExtract(invalid, pb_ZERO), result);
    if (warning) {
        #warning add check here for 2 elements
        pb.createAssign(pb.createExtract(invalid, pb.getInteger(1)), warning);
    }

    if (mSchema.CompositeKey.size() > 0) {
        PabloAST * const keyMarkers = pb.createOr(args[0], args[1]);
        pb.createAssign(pb.createExtract(getOutputStreamVar("hashKeyMarkers"), pb_ZERO), keyMarkers);

        PabloAST * const run = pb.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, args, "run");
        PabloAST * const hashableFieldData = pb.createAnd(run, nonSeparators);
        pb.createAssign(pb.createExtract(getOutputStreamVar("hashKeyRuns"), pb_ZERO), hashableFieldData);
    }


}

#endif

CSVSchemaValidatorOptions::CSVSchemaValidatorOptions(const cc::Alphabet * codeUnitAlphabet)
: mCodeUnitAlphabet(codeUnitAlphabet) {

}

unsigned round_up_to_blocksize(unsigned offset) {
    unsigned lookahead_blocks = (codegen::BlockSize - 1 + offset) / codegen::BlockSize;
    return lookahead_blocks * codegen::BlockSize;
}

void CSVSchemaValidatorOptions::addExternal(std::string name, StreamSet * strm, unsigned offset, std::pair<int, int> lengthRange) {
    if (offset == 0) {
        mExternalBindings.emplace_back(name, strm);
    } else {
        unsigned ahead = round_up_to_blocksize(offset);
        mExternalBindings.emplace_back(name, strm, FixedRate(), LookAhead(ahead));
    }
    mExternalOffsets.push_back(offset);
    mExternalLengths.push_back(lengthRange);
}

void CSVSchemaValidatorOptions::addAlphabet(const cc::Alphabet * a, StreamSet * basis) {
    mAlphabets.emplace_back(a, basis);
}


void CSVSchemaValidatorOptions::setIndexing(StreamSet * idx) {
    mIndexStream = idx;
}

}
