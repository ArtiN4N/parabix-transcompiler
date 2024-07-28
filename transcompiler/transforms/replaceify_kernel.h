#pragma once

#include "replace_bixData.h"
#include <pablo/pablo_kernel.h>

#include <kernel/core/kernel.h>
#include <llvm/IR/Value.h>
#include <string>
#include <kernel/pipeline/driver/driver.h>
#include <kernel/pipeline/pipeline_builder.h>


using namespace kernel;
using namespace llvm;
using namespace pablo;

void ReplaceByBixData(PipelineBuilder & P, replace_bixData & BixData, StreamSet * Basis, StreamSet * Output);