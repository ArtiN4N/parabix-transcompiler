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

void CSVSchemaValidatorKernel::generatePabloMethod() {

    using Mark = RE_Compiler::Marker;

    const auto & columns = mSchema.Column;
    const auto n = columns.size();
    if (LLVM_UNLIKELY(n == 0)) {
        report_fatal_error("CSV schema must have at least one column");
    }

    std::vector<RE *> fieldExpr(n);

    RE_MemoizingTransformer memo("memoizer");
    for (unsigned i = 0; i < n; ++i) {
        const auto & col = columns[i];
        re::RE * expr = col.Expression; assert (expr);
        expr = minimizeRE(expr);
        fieldExpr[i] = expr;
    }



    PabloBuilder pb(getEntryScope());

    Integer * const pb_ZERO = pb.getInteger(0);

    Var * fd = getInputStreamVar("fieldData");

    Var * const allSeparators = pb.createExtract(getInputStreamVar("allSeperators"), pb_ZERO);

    RE_Compiler re_compiler(getEntryScope(), allSeparators, &cc::Unicode);

    for (unsigned i = 0; i < mOptions.mAlphabets.size(); i++) {
        auto & alpha = mOptions.mAlphabets[i].first;
        auto basis = getInputStreamSet(alpha->getName() + "_basis");
        re_compiler.addAlphabet(alpha, basis);
    }

    Var * const recordSeparatorsAndNonText = pb.createExtract(fd, pb.getInteger(CSVDataParserFieldData::RecordSeparatorsAndNonText));

    PabloAST * fieldData = pb.createOr(pb.createNot(recordSeparatorsAndNonText), allSeparators, "fieldData");

    assert (mOptions.mIndexStream);

    PabloAST *  indexStream = pb.createExtract(getInputStreamVar("mIndexing"), pb.getInteger(0));
    fieldData = pb.createAnd(fieldData, indexStream, "fieldData");
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

    std::vector<PabloAST *> matches(n, nullptr);
    for (unsigned i = 0; i < n; ++i) {
        assert (columns[i].Expression);
        if (matches[i] == nullptr) {

            RE * expr = makeSeq({makeStart(), fieldExpr[i], makeEnd()});

            Mark match = re_compiler.compileRE(expr, Mark(allSeparators, 1), 1);

            matches[i] = match.stream();

            for (unsigned j = i + 1; j < n; ++j) {
                if (fieldExpr[j] == fieldExpr[i]) {
                    assert (matches[j] == nullptr);
                    matches[j] = matches[i];
                }
            }

        }
    }

    PabloAST * const recordSeparators = pb.createAnd(recordSeparatorsAndNonText, allSeparators, "recordSeparators");
    PabloAST * const fieldSeparators = pb.createAnd(allSeparators, pb.createNot(recordSeparators), "fieldSeparators");
    PabloAST * const nonSeperators = pb.createInFile(pb.createNot(allSeparators), "nonSeparators");

    // TODO: if we started with a carry bit set initially, we wouldn't need the start position stream.
    // However, this would need to be aware of whether we're ignoring the first line or not.
    PabloAST * currentPos = pb.createExtract(fd, pb.getInteger(CSVDataParserFieldData::StartPositions));

    PabloAST * warning = nullptr;

    PabloAST * allSeparatorsMatches = nullptr;

    for (unsigned i = 0; i < n; ++i) {

        PabloAST * const fieldStart = currentPos;
        if (i == 0) {
            currentPos = pb.createScanThru(fieldStart, nonSeperators, "endOfField" + std::to_string(i));
        } else {
            currentPos = pb.createAdvanceThenScanThru(fieldStart, nonSeperators, "endOfField" + std::to_string(i));
        }

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
            assert (false);
        } else {
            currentPos = pb.createAnd(currentPos, match);
        }

        if (allSeparatorsMatches) {
            allSeparatorsMatches = pb.createOr(allSeparatorsMatches, currentPos);
        } else {
            allSeparatorsMatches = currentPos;
        }

        if (i < (n - 1)) {
            currentPos = pb.createAnd(currentPos, fieldSeparators, "matchedFieldSep" + std::to_string(i + 1));
        } else {
            currentPos = pb.createAnd(currentPos, recordSeparators, "matchedRecSep");
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

    }

    assert (allSeparatorsMatches);
    assert (nonSeperators);


//    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, pb.createInFile(pb.createOnes(), "CSVValidator:createInFile"));

    PabloAST * result = pb.createInFile(pb.createXor(allSeparatorsMatches, allSeparators), "result");
    Var * invalid = getOutputStreamVar("invalid");
    pb.createAssign(pb.createExtract(invalid, pb_ZERO), result);

    if (warning) {
        #warning add check here for 2 elements
        pb.createAssign(pb.createExtract(invalid, pb.getInteger(1)), warning);
    }



    if (mSchema.CompositeKey.size() > 0) {
        #warning this won't work with noHeader set if the key field is the first one

        args[0] = pb.createAdvance(args[0], 1);

        PabloAST * const keyMarkers = pb.createOr(args[0], args[1]);
        pb.createAssign(pb.createExtract(getOutputStreamVar("hashKeyMarkers"), pb_ZERO), keyMarkers);

        PabloAST * const run = pb.createIntrinsicCall(pablo::Intrinsic::SpanUpTo, args, "run");
       // PabloAST * const hashableFieldData = pb.createAnd(run, nonSeperators);
        pb.createAssign(pb.createExtract(getOutputStreamVar("hashKeyRuns"), pb_ZERO), run);
    }


}

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
