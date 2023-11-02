#ifndef CSV_SCHEMA_VALIDATOR_H
#define CSV_SCHEMA_VALIDATOR_H

#include <llvm/ADT/StringRef.h>
#include <vector>
#include <pablo/pablo_kernel.h>

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

#warning work on compiler warnings

    class CSVSchemaValidatorKernel : public pablo::PabloKernel {
    public:
        CSVSchemaValidatorKernel(BuilderRef b, const csv::CSVSchema & schema, kernel::StreamSet * basisBits, kernel::StreamSet * UTFindex, kernel::StreamSet * fieldData, kernel::StreamSet * recordSeperators, kernel::StreamSet * allSeperators, kernel::StreamSet * invalid, kernel::StreamSet * keyMarkers = nullptr, kernel::StreamSet * keyRuns = nullptr);
        llvm::StringRef getSignature() const override;
        bool hasSignature() const override { return true; }

    protected:

        static std::string makeCSVSchemaSignature(const csv::CSVSchema & schema);

        CSVSchemaValidatorKernel(BuilderRef b, const csv::CSVSchema & schema, std::string && signature, kernel::StreamSet * basisBits, kernel::StreamSet * UTFindex, kernel::StreamSet * fieldData, kernel::StreamSet * recordSeperators, kernel::StreamSet * allSeperators, kernel::StreamSet * invalid, kernel::StreamSet * keyMarkers, kernel::StreamSet * keyRuns);
        void generatePabloMethod() override;
    private:
        static std::string makeSignature(const std::vector<re::RE *> & fields);
    private:
        const csv::CSVSchema &              mSchema;
        std::string                         mSignature;

    };

}


#endif // CSV_SCHEMA_VALIDATOR_H
