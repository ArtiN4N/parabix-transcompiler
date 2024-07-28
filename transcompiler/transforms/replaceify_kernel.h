#pragma once

#include "replace_bixData.h"
#include <pablo/pablo_kernel.h>

using namespace kernel;
using namespace llvm;
using namespace pablo;

class Replaceify : public pablo::PabloKernel {
public:
    Replaceify(KernelBuilder & b, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output);
protected:
    void generatePabloMethod() override;
    replace_bixData & mBixData;
};