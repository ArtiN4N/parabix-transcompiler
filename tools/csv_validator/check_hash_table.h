#ifndef CHECK_HASH_TABLE_H
#define CHECK_HASH_TABLE_H

#include <kernel/core/kernel.h>
#include "csv_schema_validator.h"

namespace kernel {

class CheckKeyUniqueness : public SegmentOrientedKernel {
public:
    CheckKeyUniqueness(BuilderRef b, const csv::CSVSchema & schema, StreamSet * ByteStream, StreamSet * const HashVals, StreamSet * keyMarkers, Scalar * const schemaObject, Scalar * const fileName);
    void linkExternalMethods(BuilderRef b) override;
    llvm::StringRef getSignature() const override;
    bool hasSignature() const override { return true; }
    void generateInitializeMethod(BuilderRef b) override;
    void generateFinalizeMethod(BuilderRef b) override;
protected:

    static std::string makeSignature(const csv::CSVSchema & schema, const size_t bitsPerHashCode);

    CheckKeyUniqueness(BuilderRef b, const csv::CSVSchema & schema, std::string && signature, StreamSet * ByteStream, StreamSet * const HashVals, StreamSet * keyMarkers, Scalar * const schemaObject, Scalar * const fileName);
private:
    void generateDoSegmentMethod(BuilderRef b) override;
private:
    const csv::CSVSchema & mSchema;
    std::string            mSignature;
};


}

#endif // CHECK_HASH_TABLE_H
