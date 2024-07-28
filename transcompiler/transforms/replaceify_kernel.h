#pragma once

#include "replace_bixData.h"
#include <pablo/pablo_kernel.h>

using namespace kernel;
using namespace llvm;
using namespace pablo;

void ReplaceByBixData(PipelineBuilder & P,
                  replace_bixData & BixData,    
                  StreamSet * Basis, StreamSet * Output
                  );