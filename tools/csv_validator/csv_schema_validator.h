#ifndef CSV_SCHEMA_VALIDATOR_H
#define CSV_SCHEMA_VALIDATOR_H

#include <llvm/ADT/StringRef.h>
#include <vector>
#include <pablo/pablo_kernel.h>
#include <re/alphabet/alphabet.h>
#include <re/alphabet/multiplex_CCs.h>

namespace re { class RE; }

namespace csv {

    struct CSVSchemaColumnRule {
        std::string Name;
        bool Optional = false; // field data can be empty regardless of rule
        bool MatchIsFalse = false; // negate field match when validating
        bool IgnoreCase = false; // the case of a column value is not important
        bool Warning = false; // violations are warnings not errors
        re::RE * Expression = nullptr;
    };

    struct CSVSchemaCompositeKey {
        std::vector<size_t> Fields;
    };

    struct CSVSchema {
        size_t TotalColumns = 0;
        char Separator = ',';
        bool Quoted = false; // whether or not all columns are quoted
        bool PermitEmpty = false; // whether CSV file can be empty
        bool NoHeader = false; // whether first line of CSV is column header names
        bool IgnoreColumnNameCase = false; // ignore mismatches in case for column header names
        std::vector<CSVSchemaColumnRule> Column;
        std::vector<CSVSchemaCompositeKey> CompositeKey;
    };

    class CSVSchemaValidatorOptions {
        friend class CSVSchemaValidatorKernel;
    public:
        using Alphabets = std::vector<std::pair<const cc::Alphabet *, kernel::StreamSet *>>;
        CSVSchemaValidatorOptions(const cc::Alphabet * codeUnitAlphabet = &cc::UTF8);
//        void setBarrier(kernel::StreamSet * barrierStream);
        void setIndexing(kernel::StreamSet * indexStream);

        void addExternal(std::string name,
                         kernel::StreamSet * strm,
                         unsigned offset = 0,
                         std::pair<int, int> lengthRange = std::make_pair<int,int>(1, 1));
        void addAlphabet(const cc::Alphabet * a, kernel::StreamSet * basis);

        void setKeyRunStream(kernel::StreamSet * keyRun) { mKeyRuns = keyRun; }

        void setKeyMarkerStream(kernel::StreamSet * keyMarkers) { mKeyMarkers = keyMarkers; }

//    protected:
//        kernel::Bindings streamSetInputBindings();
//        kernel::Bindings streamSetOutputBindings();
//        kernel::Bindings scalarInputBindings();
//        kernel::Bindings scalarOutputBindings();
//        std::string makeSignature();

    private:

        const cc::Alphabet *                mCodeUnitAlphabet = nullptr;
        kernel::StreamSet *                 mIndexStream = nullptr;
        kernel::StreamSet *                 mCombiningStream = nullptr;

        kernel::StreamSet *                 mKeyMarkers = nullptr;
        kernel::StreamSet *                 mKeyRuns = nullptr;

        kernel::Bindings                    mExternalBindings;
        std::vector<unsigned>               mExternalOffsets;
        std::vector<std::pair<int, int>>    mExternalLengths;
        Alphabets                           mAlphabets;
    };


    class CSVSchemaValidatorKernel : public pablo::PabloKernel {
    public:
        CSVSchemaValidatorKernel(BuilderRef b, const csv::CSVSchema & schema, kernel::StreamSet * basisBits, kernel::StreamSet * fieldData, kernel::StreamSet * recordSeperators, kernel::StreamSet * allSeperators, kernel::StreamSet * invalid, CSVSchemaValidatorOptions && options);
        llvm::StringRef getSignature() const override;
        bool hasSignature() const override { return true; }

    protected:

        static std::string makeCSVSchemaSignature(const csv::CSVSchema & schema);

        CSVSchemaValidatorKernel(BuilderRef b, const csv::CSVSchema & schema, std::string && signature, kernel::StreamSet * basisBits, kernel::StreamSet * fieldData, kernel::StreamSet * recordSeperators, kernel::StreamSet * allSeperators, kernel::StreamSet * invalid, CSVSchemaValidatorOptions && options);
        void generatePabloMethod() override;
    private:
        static std::string makeSignature(const std::vector<re::RE *> & fields);
    private:
        const csv::CSVSchema &              mSchema;
        CSVSchemaValidatorOptions           mOptions;
        std::string                         mSignature;

    };

}


#endif // CSV_SCHEMA_VALIDATOR_H
