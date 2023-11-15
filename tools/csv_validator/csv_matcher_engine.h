#ifndef CSV_MATCHER_ENGINE_H
#define CSV_MATCHER_ENGINE_H

#include <pablo/pablo_kernel.h>  // for PabloKernel
#include <grep/grep_engine.h>
#include <re/adt/re_re.h>
#include "csv_schema_validator.h"
#include "csv_validator_util.h"

#include <kernel/pipeline/driver/cpudriver.h>

typedef void (*CSVValidatorFunctionType)(uint32_t fd, const char * fileName);

class CSVMatcherEngine {
    enum class FileStatus {Pending, GrepComplete, PrintComplete};
    friend class InternalSearchEngine;
    friend class InternalMultiSearchEngine;
public:

    using ProgBuilderRef = const std::unique_ptr<kernel::ProgramBuilder> &;

    enum class EngineKind {QuietMode, MatchOnly, CountOnly, EmitMatches};

    CSVMatcherEngine() = default;

    CSVValidatorFunctionType compile(CPUDriver & pxDriver, const std::string & inputSchema);

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
    bool matchesToEOLrequired();

    // Transpose to basis bit streams, if required otherwise return the source byte stream.
    kernel::StreamSet * getBasis(ProgBuilderRef P, kernel::StreamSet * ByteStream);

    // Initial grep set-up.
    // Implement any required checking/processing of null characters, determine the
    // line break stream and the U8 index stream (if required).
    void grepPrologue(ProgBuilderRef P, kernel::StreamSet * SourceStream);
    // Prepare external property and GCB streams, if required.
    void prepareExternalStreams(ProgBuilderRef P, kernel::StreamSet * SourceStream);
    kernel::StreamSet * getMatchSpan(ProgBuilderRef P, re::RE * r, kernel::StreamSet * MatchResults);
    kernel::StreamSet * initialMatches(ProgBuilderRef P, kernel::StreamSet * ByteStream);
    kernel::StreamSet * matchedLines(ProgBuilderRef P, kernel::StreamSet * ByteStream);
    kernel::StreamSet * grepPipeline(ProgBuilderRef P, kernel::StreamSet * ByteStream);
//    virtual uint64_t doGrep(const std::vector<std::string> & fileNames, std::ostringstream & strm);
//    int32_t openFile(const std::string & fileName, std::ostringstream & msgstrm);
//    void applyColorization(const std::unique_ptr<kernel::ProgramBuilder> & E,
//                                              kernel::StreamSet * SourceCoords,
//                                              kernel::StreamSet * MatchSpans,
//                                              kernel::StreamSet * Basis);
//    std::string linePrefix(std::string fileName);

    void addExternalStreams(ProgBuilderRef P, const cc::Alphabet * a, const csv::CSVSchema &schema, csv::CSVSchemaValidatorOptions & options, kernel::StreamSet * indexMask = nullptr);


protected:

    EngineKind mEngineKind;
    bool mSuppressFileMessages;
    argv::BinaryFilesMode mBinaryFilesMode;
//    bool mPreferMMap;
//    bool mColoring;
//    bool mShowFileNames;
    std::string mStdinLabel;
//    bool mShowLineNumbers;
//    unsigned mBeforeContext;
//    unsigned mAfterContext;
    bool mInitialTab;
    bool mCaseInsensitive;
    bool mInvertMatches;
    int mMaxCount;
    bool mGrepStdIn;
    NullCharMode mNullMode;
//    BaseDriver & mGrepDriver;
    void * mMainMethod;
    void * mBatchMethod;

//    std::atomic<unsigned> mNextFileToGrep;
//    std::atomic<unsigned> mNextFileToPrint;
//    std::vector<boost::filesystem::path> mInputPaths;
//    std::vector<std::vector<std::string>> mFileGroups;
//    std::vector<std::ostringstream> mResultStrs;
//    std::vector<FileStatus> mFileStatus;
//    bool grepMatchFound;
    grep::GrepRecordBreakKind mGrepRecordBreak;
    bool UnicodeIndexing = false;
//    re:: RE * mRE;
    re::ReferenceInfo mRefInfo;
//    std::string mFileSuffix;
    Component mExternalComponents;
    Component mInternalComponents;
    const cc::Alphabet * mIndexAlphabet;
    const cc::Alphabet * mLengthAlphabet;
    kernel::ExternalStreamTable mExternalTable;
    kernel::StreamSet * mLineBreakStream;
    kernel::StreamSet * mU8index;
    kernel::StreamSet * mU21;
    std::vector<std::string> mSpanNames;
    re::UTF8_Transformer mUTF8_Transformer;
//    pthread_t mEngineThread;
//    kernel::ParabixIllustrator * mIllustrator;
};

#endif // CSV_MATCHER_ENGINE_H
