#include "csv_schema_validator.h"

#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>

#include <re/parse/PCRE_parser.h>
#include <re/transforms/re_memoizing_transformer.h>
#include <grep/regex_passes.h>
#include <re/transforms/to_utf8.h>
#include <re/unicode/casing.h>
#include <re/printer/re_printer.h>
#include <re/adt/re_seq.h>



#include <re/compile/re_compiler.h>
#include <boost/container/flat_map.hpp>

#include <kernel/core/streamset.h>
#include <kernel/core/kernel_builder.h>
#include <llvm/IR/Function.h>

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
        if (column.Optional) {
            out << 'O';
        }
        if (column.MatchIsFalse) {
            out << 'N';
        }
        if (column.IgnoreCase) {
            out << 'I';
        }
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

CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, const csv::CSVSchema & schema, StreamSet * basisBits, StreamSet * UTFindex, StreamSet * fieldData, StreamSet * recordSeperators, StreamSet * allSeperators, StreamSet * invalid, StreamSet * keyMarkers, StreamSet * keyRuns)
: CSVSchemaValidatorKernel(b, schema, makeCSVSchemaSignature(schema), basisBits, UTFindex, fieldData, recordSeperators, allSeperators, invalid, keyMarkers, keyRuns) {

}

CSVSchemaValidatorKernel::CSVSchemaValidatorKernel(BuilderRef b, const csv::CSVSchema & schema, std::string && signature, StreamSet * basisBits, StreamSet * UTFindex, StreamSet * fieldData, StreamSet * recordSeperators, StreamSet * allSeperators, StreamSet * invalid, StreamSet * keyMarkers, StreamSet * keyRuns)
: PabloKernel(b, "csvv" + getStringHash(signature),
    {Binding{"basisBits", basisBits}, Binding{"UTFindex", UTFindex}, Binding{"fieldData", fieldData},
     Binding{"allSeperators", allSeperators}, Binding{"recordSeperators", recordSeperators}},
    {Binding{"invalid", invalid}})
, mSchema(schema)
, mSignature(std::move(signature)) {
    if (schema.CompositeKey.size() > 0) {
        mOutputStreamSets.emplace_back("hashKeyMarkers", keyMarkers);
        mOutputStreamSets.emplace_back("hashKeyRuns", keyRuns);
    }
}


StringRef CSVSchemaValidatorKernel::getSignature() const {
    return mSignature;
}

// const static std::string FieldSepName = ":";

void CSVSchemaValidatorKernel::generatePabloMethod() {

    const auto & columns = mSchema.Column;
    const auto n = columns.size();
    std::vector<RE *> fieldExpr(n);

    RE_MemoizingTransformer memo("memoizer");
    for (unsigned i = 0; i < columns.size(); ++i) {
        const auto & col = columns[i];

        re::RE * expr = col.Expression; assert (expr);
        if (col.IgnoreCase) {
            expr = re::resolveCaseInsensitiveMode(expr, true);
        }
        if (col.Optional && !matchesEmptyString(expr)) {
            // the Column Rule evaluates to true, or if the column is empty.
            RE * emptyString = makeSeq({makeStart(), makeEnd()});
            expr = makeAlt({expr, emptyString});
        }

        expr = regular_expression_passes(expr);
        // if UTF8 ?
        expr = re::toUTF8(expr);



        expr = memo.transformRE(expr);
        fieldExpr[i] = expr;
    }

    PabloBuilder pb(getEntryScope());

    Var * const recordSeperators = pb.createExtract(getInputStreamVar("recordSeperators"), pb.getInteger(0));

    RE_Compiler re_compiler(getEntryScope(), recordSeperators, &cc::UTF8);

    auto basisBits = getInputStreamSet("basisBits");

    Integer * const pb_ZERO = pb.getInteger(0);

    Var * const UTFindex = pb.createExtract(getInputStreamVar("UTFindex"), pb_ZERO);
    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {UTFindex });

    Var * const fieldDataMask = pb.createExtract(getInputStreamVar("fieldData"), pb_ZERO);
    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {fieldDataMask });
    assert (fieldDataMask->getType() == getStreamTy());
    PabloAST * const textIndex = pb.createAnd(fieldDataMask, UTFindex, "textIndex");

    pb.createIntrinsicCall(pablo::Intrinsic::PrintRegister, {textIndex });

    re_compiler.setIndexing(&cc::UTF8, textIndex);

    re_compiler.addAlphabet(&cc::UTF8, basisBits);

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
            Mark match = re_compiler.compileRE(fieldExpr[i], Mark{textIndex, 1}, 1);
            assert (match.offset() == 1);
            matches[i] = match.stream();
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

}
