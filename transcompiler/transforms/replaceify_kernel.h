#pragma once

#include <vector>
#include <fcntl.h>
#include <string>
#include <iostream>

#include "replace_bixData.h"
#include <pablo/pablo_kernel.h>

namespace kernel {

class Replaceify : public pablo::PabloKernel {
public:
    Replaceify(KernelBuilder & b, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output);
protected:
    void generatePabloMethod() override;
    replace_bixData & mBixData;
};

}