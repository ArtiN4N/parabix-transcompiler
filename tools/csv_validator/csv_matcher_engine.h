#ifndef CSV_MATCHER_ENGINE_H
#define CSV_MATCHER_ENGINE_H

#include <pablo/pablo_kernel.h>  // for PabloKernel
#include <grep/grep_engine.h>
#include <re/adt/re_re.h>
#include "csv_schema_validator.h"
#include "csv_validator_util.h"

#include <kernel/pipeline/driver/cpudriver.h>

typedef void (*CSVValidatorFunctionType)(uint32_t fd, const csv::CSVSchema & schema, const char * fileName);

class CSVMatcherEngine {
    enum class FileStatus {Pending, GrepComplete, PrintComplete};
    friend class InternalSearchEngine;
    friend class InternalMultiSearchEngine;
public:

    using ProgBuilderRef = const std::unique_ptr<kernel::ProgramBuilder> &;

    enum class EngineKind {QuietMode, MatchOnly, CountOnly, EmitMatches};

    CSVMatcherEngine() = default;

    CSVValidatorFunctionType compile(CPUDriver & pxDriver, csv::CSVSchema & schema);

protected:

    void initRE(csv::CSVSchema & schema);


protected:
    // Functional components that may be required for grep searches,
    // depending on search pattern, mode flags, external parameters and
    // implementation strategy.
    typedef uint32_t component_t;
    enum class Component : component_t {
        NoComponents = 0,
        S2P = 1,
        UTF8index = 2,
        MoveMatchesToEOL = 4,
        MatchSpans = 8,
        U21 = 64
    };
    bool hasComponent(Component compon_set, Component c);
    void setComponent(Component & compon_set, Component c);

    // Transpose to basis bit streams, if required otherwise return the source byte stream.
    kernel::StreamSet * getBasis(ProgBuilderRef P, kernel::StreamSet * ByteStream);

    // Initial grep set-up.
    // Implement any required checking/processing of null characters, determine the
    // line break stream and the U8 index stream (if required).
    void grepPrologue(ProgBuilderRef P, kernel::StreamSet * SourceStream);
    // Prepare external property and GCB streams, if required.
    void prepareExternalStreams(ProgBuilderRef P, kernel::StreamSet * SourceStream);

    void addExternalStreams(ProgBuilderRef P, const cc::Alphabet * a, const csv::CSVSchema &schema, csv::CSVSchemaValidatorOptions & options, kernel::StreamSet * indexMask = nullptr);


protected:

    bool UnicodeIndexing = false;
    Component mExternalComponents;
    Component mInternalComponents;
    const cc::Alphabet * mIndexAlphabet;
    const cc::Alphabet * mLengthAlphabet;
    kernel::ExternalStreamTable mExternalTable;
    kernel::StreamSet * mLineBreakStream;
    kernel::StreamSet * mU8index;
};

#endif // CSV_MATCHER_ENGINE_H
