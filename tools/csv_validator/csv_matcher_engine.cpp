#include "csv_matcher_engine.h"
#include "csv_schema_validator.h"

#include <re/adt/adt.h>
#include <re/adt/re_utility.h>
#include <re/printer/re_printer.h>
#include <re/alphabet/alphabet.h>
#include <re/analysis/re_analysis.h>
#include <re/analysis/re_name_gather.h>
#include <re/analysis/capture-ref.h>
#include <re/analysis/collect_ccs.h>
#include <re/cc/cc_kernel.h>
#include <re/alphabet/multiplex_CCs.h>
#include <re/transforms/re_transformer.h>
#include <re/transforms/re_contextual_simplification.h>
#include <re/transforms/exclude_CC.h>
#include <re/transforms/to_utf8.h>
#include <re/transforms/remove_nullable.h>
#include <re/transforms/replaceCC.h>
#include <re/transforms/re_multiplex.h>
#include <re/transforms/name_intro.h>
#include <re/transforms/reference_transform.h>
#include <re/transforms/variable_alt_promotion.h>
#include <re/unicode/casing.h>
#include <re/unicode/boundaries.h>
#include <re/unicode/re_name_resolve.h>

#include <kernel/util/bixhash.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/scan/scanmatchgen.h>
#include <kernel/core/kernel_builder.h>

#include <re/unicode/resolve_properties.h>
#include <grep/regex_passes.h>

#include "csv_validator_toolchain.h"
#include "simple_csv_schema_parser.hpp"
#include "check_hash_table.h"
#include "../csv/csv_util.hpp"
#include "csv_error_identifier.h"
#include <re/transforms/re_memoizing_transformer.h>
#include <grep/grep_kernel.h>

using namespace cc;
using namespace csv;
using namespace re;
using namespace llvm;
using namespace grep;
using namespace kernel;

// TODO: temporary workaround. The system already implicitly "adds" start and end marks
// to bookend the query for each field but if the user explicitly adds them, the RE compiler
// treats them as line start/ends and fails to validate the field. However, by stripping them
// we cannot correctly validate multiline field values.

class StripStartEndBookEnds : public RE_Transformer {
public:
    StripStartEndBookEnds(std::string transformationName)
    : RE_Transformer(std::move(transformationName)) {

    }
    RE * transformStart(Start * s) override;
    RE * transformEnd(End * e) override;
};

RE * StripStartEndBookEnds::transformStart(Start * s) {
    return makeSeq();
}

RE * StripStartEndBookEnds::transformEnd(End * e) {
    return makeSeq();
}

void CSVMatcherEngine::initRE(csv::CSVSchema & schema) {
//    if ((mEngineKind != EngineKind::EmitMatches) || mInvertMatches) {
//        mColoring = false;
//    }

    UnicodeIndexing = false;

    // Determine the unit of length for the RE.  If the RE involves
    // fixed length UTF-8 sequences only, then UTF-8 can be used
    // for most efficient processing.   Otherwise we must use full
    // Unicode length calculations.
    mLengthAlphabet = &cc::UTF8;

    StreamIndexCode u8 = mExternalTable.declareStreamIndex("u8");
    StreamIndexCode Unicode = mExternalTable.declareStreamIndex("U", u8, "u8index");

    CapturePostfixMap cm;
    ReferenceInfo info;
    UCD::PropertyExternalizer PE;

    RE_MemoizingTransformer MT("memoizer");
    StripStartEndBookEnds ST("removeStartEnds");

    RE * emptyString = nullptr;

    const auto optLevel = codegen::OptLevel;
    codegen::OptLevel = CodeGenOpt::Level::Aggressive;

    for (unsigned i = 0; i < schema.Column.size(); ++i) {
        auto & column = schema.Column[i];

        RE * expr = ST.transformRE(column.Expression, NameTransformationMode::TransformDefinition);

        if (column.Optional && !matchesEmptyString(expr)) {
            // the Column Rule evaluates to true, or if the column is empty.
            if (emptyString == nullptr) {
                emptyString = makeSeq(); // { makeStart(), makeEnd() }
            }
            expr = makeAlt({expr, emptyString});
        }

        expr = resolveModesAndExternalSymbols(expr, column.IgnoreCase);

        updateReferenceInfo(expr, cm, info);

        expr = regular_expression_passes(expr);

        expr = PE.transformRE(expr);

        column.Expression = MT.transformRE(expr);
    }

    codegen::OptLevel = optLevel;

    mLengthAlphabet = &cc::Unicode;

    if (!info.twixtREs.empty()) {
        UnicodeIndexing = true;
        auto indexCode = mExternalTable.getStreamIndex(cc::Unicode.getCode());
//        setComponent(mExternalComponents, Component::S2P);
        FixedReferenceTransformer FRT(info);

        for (unsigned i = 0; i < schema.Column.size(); ++i) {
            auto & column = schema.Column[i];
            column.Expression = MT.transformRE(FRT.transformRE(column.Expression));
        }

        for (auto m : FRT.mNameMap) {
            Reference * ref = cast<Reference>(m.second);
            UCD::property_t p = ref->getReferencedProperty();
            if (p == UCD::identity) {
                auto u8_u21 = new U21_External();
                mExternalTable.declareExternal(u8, "u21", u8_u21);
                mExternalTable.declareExternal(Unicode, "basis", new FilterByMaskExternal(u8, {"u8index", "u21"}, u8_u21));
            } else {
                std::string extName = UCD::getPropertyFullName(p) + "_basis";
                mExternalTable.declareExternal(indexCode, extName, new PropertyBasisExternal(p));
            }
            auto captureLen = getLengthRange(ref->getCapture(), &cc::Unicode).first;
            if (captureLen != 1) {
                llvm::report_fatal_error("Capture length > 1 is a future extension");
            }
            std::string instanceName = ref->getInstanceName();
            auto mapping = info.twixtREs.find(instanceName);
            auto twixtLen = getLengthRange(mapping->second, &cc::Unicode).first;
            auto dist = captureLen + twixtLen;
            mExternalTable.declareExternal(indexCode, m.first, new PropertyDistanceExternal(p, dist));
        }
    }

    mExternalTable.declareExternal(u8, "LineStarts", new LineStartsExternal());

    mIndexAlphabet = &cc::Unicode;
    setComponent(mExternalComponents, Component::S2P);
    setComponent(mExternalComponents, Component::UTF8index);

    // TODO: multiplexing seems to break matching algorithm?

#if 0

        SmallVector<RE *, 256> tmp;
        for (unsigned i = 0; i < schema.Column.size(); ++i) {
            auto & column = schema.Column[i];
            tmp.push_back(column.Expression);
        }

        const auto UnicodeSets = collectCCs(makeSeq(tmp.begin(), tmp.end()), *mIndexAlphabet);
        if (!UnicodeSets.empty()) {
            auto mpx = makeMultiplexedAlphabet("mpx", UnicodeSets);
            mExternalTable.declareExternal(Unicode, mpx->getName() + "_basis", new MultiplexedExternal(mpx));
            for (unsigned i = 0; i < schema.Column.size(); ++i) {
                auto & column = schema.Column[i];
                RE * re = transformCCs(mpx, column.Expression, NameTransformationMode::None);
                column.Expression = MT.transformRE(re);
            }
        }

#endif

    auto indexCode = mExternalTable.getStreamIndex(mIndexAlphabet->getCode());
//    if (hasGraphemeClusterBoundary(re)) {
//        auto GCB_basis = new PropertyBasisExternal(UCD::GCB);
//        mExternalTable.declareExternal(u8, "UCD:" + getPropertyFullName(UCD::GCB) + "_basis", GCB_basis);
//        RE * epict_pe = UCD::linkAndResolve(makePropertyExpression("Extended_Pictographic"));
//        Name * epict = cast<Name>(UCD::externalizeProperties(epict_pe));
//        mExternalTable.declareExternal(u8, epict->getFullName(), new PropertyExternal(epict));
//        auto u8_GCB = new GraphemeClusterBreak(this, &cc::UTF8);
//        mExternalTable.declareExternal(u8, "\\b{g}", u8_GCB);
//        if (indexCode == Unicode) {
//            mExternalTable.declareExternal(Unicode, "\\b{g}", new FilterByMaskExternal(u8, {"u8index","\\b{g}"}, u8_GCB));
//        }
//    }

    for (auto m : PE.mNameMap) {
        if (PropertyExpression * pe = dyn_cast<PropertyExpression>(m.second)) {
            if (pe->getKind() == PropertyExpression::Kind::Codepoint) {
                mExternalTable.declareExternal(indexCode, m.first, new PropertyExternal(makeName(m.first, m.second)));
            } else { //PropertyExpression::Kind::Boundary
                UCD::property_t prop = static_cast<UCD::property_t>(pe->getPropertyCode());
                if (prop != UCD::g) {  // Boundary expressions, except GCB.
                    auto prop_basis = new PropertyBasisExternal(prop);
                    mExternalTable.declareExternal(indexCode, getPropertyFullName(prop) + "_basis", prop_basis);
                    auto boundary = new PropertyBoundaryExternal(prop);
                    mExternalTable.declareExternal(indexCode, m.first, boundary);
                }
           }
        } else {
            llvm::report_fatal_error("Expected property expression");
        }
    }

    VariableLengthCCNamer CCnamer;

    bool addWordBoundaryExternal = false;

    for (unsigned i = 0; i < schema.Column.size(); ++i) {
        auto & column = schema.Column[i];
        column.Expression = CCnamer.transformRE(column.Expression);
        if (!addWordBoundaryExternal && hasWordBoundary(column.Expression)) {
            addWordBoundaryExternal = true;
        }
    }

    for (auto m : CCnamer.mNameMap) {
        mExternalTable.declareExternal(indexCode, m.first, new CC_External(cast<CC>(m.second)));
    }

    if (addWordBoundaryExternal) {
        mExternalTable.declareExternal(indexCode, "\\b", new WordBoundaryExternal());
    }

}

CSVValidatorFunctionType CSVMatcherEngine::compile(CPUDriver & pxDriver, const std::string & inputSchema) {

    auto & b = pxDriver.getBuilder();

    Type * const int32Ty = b->getInt32Ty();

    auto P = pxDriver.makePipeline({Binding{int32Ty, "fd"}, Binding{b->getInt8PtrTy(), "fileName"}});

    P->setUniqueName("csv_validator");

    Scalar * const fileDescriptor = P->getInputScalar("fd");

    StreamSet * const ByteStream = P->CreateStreamSet(1, 8);

    auto schemaFile = csv::CSVSchemaParser::load(inputSchema);

    setComponent(mExternalComponents, Component::S2P);

    initRE(schemaFile);

    P->CreateKernelCall<MMapSourceKernel>(fileDescriptor, ByteStream);

    auto BasisBits = getBasis(P, ByteStream);

    mLineBreakStream = nullptr;
    mU8index = nullptr;
    mNullMode = NullCharMode::Data;

    mU8index = P->CreateStreamSet(1, 1);
    P->CreateKernelCall<UTF8_index>(ByteStream, mU8index);

    StreamSet * csvCCs = P->CreateStreamSet(5);
    P->CreateKernelCall<CSVlexer>(BasisBits, csvCCs);

    StreamSet * fieldData = P->CreateStreamSet(2);
    StreamSet * allSeparators = P->CreateStreamSet(1);
    mLineBreakStream = allSeparators;

    P->CreateKernelCall<CSVDataParser>(csvCCs, fieldData, allSeparators);

    auto u8 = mExternalTable.getStreamIndex(cc::UTF8.getCode());
    mExternalTable.declareExternal(u8, "u8index", new PreDefined(mU8index));
    mExternalTable.declareExternal(u8, "$", new PreDefined(allSeparators));

    StreamSet * errors = P->CreateStreamSet(schemaFile.AnyWarnings ? 2 : 1);

    prepareExternalStreams(P, BasisBits);

    csv::CSVSchemaValidatorOptions options;

    options.setIndexing(mU8index);

    addExternalStreams(P, mIndexAlphabet, schemaFile, options, mU8index);

    if (!schemaFile.AnyUniqueKeys) {

        P->CreateKernelFamilyCall<CSVSchemaValidatorKernel>(schemaFile, BasisBits, fieldData, allSeparators, errors, std::move(options));

    } else {

        StreamSet * const keyMarkers = P->CreateStreamSet(1);

        options.setKeyMarkerStream(keyMarkers);

        StreamSet * const keyRuns = P->CreateStreamSet(1);

        options.setKeyRunStream(keyRuns);

        // If we use a bixhash like technique, we could possibly chunk the field data into N-byte phases and use
        // a loop to combine the data. But how can we prevent the data from one record from being combined with
        // another? We could scan through and iterate over each record individually?

        P->CreateKernelFamilyCall<CSVSchemaValidatorKernel>(schemaFile, BasisBits, fieldData, allSeparators, errors, std::move(options));

        StreamSet * hashes = P->CreateStreamSet(NumOfHashBits);

        StreamSet * selector_span = P->CreateStreamSet(1);

        P->CreateKernelFamilyCall<BixSubHash>(BasisBits, keyRuns, hashes, selector_span, NumOfSteps, HashSeed);

        const auto sizeTyWidth = b->getSizeTy()->getBitWidth();

        StreamSet * const markerSeq = P->CreateStreamSet(1, sizeTyWidth);

        P->CreateKernelCall<ExtractCoordinateSequence>(keyMarkers, markerSeq);

        StreamSet * const hash_bit_selector = P->CreateStreamSet(1);

        P->CreateKernelCall<IdentifyLastSelector>(selector_span, hash_bit_selector);

        StreamSet * const compressed = P->CreateStreamSet(NumOfHashBits);

        P->CreateKernelCall<FieldCompressKernel>(Select(hash_bit_selector, {0}), SelectOperationList{Select(hashes, streamutils::Range(0, NumOfHashBits))}, compressed, sizeTyWidth);

        StreamSet * const outputs = P->CreateStreamSet(NumOfHashBits);

        P->CreateKernelCall<StreamCompressKernel>(hash_bit_selector, compressed, outputs, sizeTyWidth);

        StreamSet * hashVals;

        if (NumOfHashBits <= 8) {
            hashVals = P->CreateStreamSet(1, 8);
            P->CreateKernelCall<P2SKernel>(outputs, hashVals);
        } else if (NumOfHashBits <= 16) {
            hashVals = P->CreateStreamSet(1, 16);
            P->CreateKernelCall<P2S16Kernel>(outputs, hashVals);
        } else {
            hashVals = P->CreateStreamSet(1, 32);
            P->CreateKernelCall<P2S32Kernel>(outputs, hashVals);
        }

        P->CreateKernelFamilyCall<CheckKeyUniqueness>(schemaFile, ByteStream, hashVals, markerSeq);

    }

    Scalar * const fileName = P->getInputScalar("fileName");

    // TODO: using the scan match here like this won't really let us determine what field is wrong in the case an error occurs
    // since the only information we get is whether a match fails.

    // TODO: What if we had pablo functions that automatically converts bit markers into an integer sequence and vv?

    Scalar * const fieldsPerRecord = P->CreateConstant(b->getSize(schemaFile.Column.size()));

    P->CreateKernelCall<CSVErrorIdentifier>(errors, allSeparators, ByteStream, fileName, fieldsPerRecord);

    return reinterpret_cast<CSVValidatorFunctionType>(P->compile());
}

StreamSet * CSVMatcherEngine::getBasis(ProgBuilderRef P, StreamSet * ByteStream) {
    StreamSet * Source = ByteStream;
    auto u8 = mExternalTable.getStreamIndex(mIndexAlphabet->getCode());
    assert (hasComponent(mExternalComponents, Component::S2P));
//    if (hasComponent(mExternalComponents, Component::S2P)) {
        StreamSet * BasisBits = P->CreateStreamSet(ByteStream->getFieldWidth(), 1);
        Selected_S2P(P, ByteStream, BasisBits);
        Source = BasisBits;
        mExternalTable.declareExternal(u8, "basis", new PreDefined(BasisBits));
//    } else {
//        mExternalTable.declareExternal(u8, "basis", new PreDefined(ByteStream));
//    }
    return Source;
}

void CSVMatcherEngine::grepPrologue(ProgBuilderRef P, StreamSet * SourceStream) {

}

void CSVMatcherEngine::prepareExternalStreams(ProgBuilderRef P, StreamSet * SourceStream) {
    mExternalTable.resolveExternals(P);
}

void CSVMatcherEngine::addExternalStreams(ProgBuilderRef P, const cc::Alphabet * a, const csv::CSVSchema & schema, csv::CSVSchemaValidatorOptions & options, kernel::StreamSet * indexMask) {
    auto indexing = mExternalTable.getStreamIndex(a->getCode());
    re::Alphabet_Set alphas;
    std::set<re::Name *> externals;
    for (const auto & field : schema.Column) {
        re::collectAlphabets(field.Expression, alphas);
        re::gatherNames(field.Expression, externals);
    }

    // We may end up with multiple instances of a Name, but we should
    // only add the external once.
    std::set<std::string> extNames;
    for (const auto & e : externals) {
        auto name = e->getFullName();
      //  errs() << "nm: " << name << "\n";
        if ((extNames.count(name) == 0) && mExternalTable.isDeclared(indexing, name)) {
            extNames.insert(name);
            const auto & ext = mExternalTable.lookup(indexing, name);
            StreamSet * extStream = mExternalTable.getStreamSet(P, indexing, name);
            const auto offset = ext->getOffset();
            std::pair<int, int> lengthRange = ext->getLengthRange();
            options.addExternal(name, extStream, offset, lengthRange);
        } else {
            // We have a name that has not been set up as an external.
            // Its definition will need to be processed.
            re::RE * defn = e->getDefinition();
            if (defn) re::collectAlphabets(defn, alphas);
        }
    }
    for (auto & a : alphas) {
        if (const MultiplexedAlphabet * mpx = dyn_cast<MultiplexedAlphabet>(a)) {
            std::string basisName = a->getName() + "_basis";
          //  errs() << "mpx: " << basisName << "\n";
            StreamSet * alphabetBasis = mExternalTable.getStreamSet(P, indexing, basisName);
            options.addAlphabet(mpx, alphabetBasis);
        } else {
            StreamSet * alphabetBasis = mExternalTable.getStreamSet(P, indexing, "basis");
         //   errs() << "alp: " << alphabetBasis << "\n";
            options.addAlphabet(a, alphabetBasis);
        }
    }
}

bool CSVMatcherEngine::hasComponent(Component compon_set, Component c) {
    return (static_cast<component_t>(compon_set) & static_cast<component_t>(c)) != 0;
}

void CSVMatcherEngine::setComponent(Component & compon_set, Component c) {
    compon_set = static_cast<Component>(static_cast<component_t>(compon_set) | static_cast<component_t>(c));
}

//
// Moving matches to EOL.   Mathches need to be aligned at EOL if for
// scanning or counting processes (with a max count != 1).   If the REs
// are not all anchored, then we need to move the matches to EOL.
bool CSVMatcherEngine::matchesToEOLrequired () {
    return (mGrepRecordBreak == GrepRecordBreakKind::Unicode);
}
