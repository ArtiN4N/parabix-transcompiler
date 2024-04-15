#ifndef CSV_ERROR_IDENTIFIER_H
#define CSV_ERROR_IDENTIFIER_H

#include <kernel/core/kernel.h>

namespace kernel {

class CSVErrorIdentifier : public MultiBlockKernel {
public:
    CSVErrorIdentifier(BuilderRef b, StreamSet * const errorStream, StreamSet * const allSeparators, StreamSet * const ByteStream, Scalar * const schemaObject, Scalar * const fileName);
    void linkExternalMethods(BuilderRef b) override;
private:
    void generateMultiBlockLogic(BuilderRef iBuilder, llvm::Value * const numOfStrides) override;
};

}

#endif // CSV_ERROR_IDENTIFIER_H
