/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <cstdio>
#include <vector>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/streamutils/stream_shift.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/scan/scanmatchgen.h>
#include <re/adt/re_name.h>
#include <re/cc/cc_kernel.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <string>
#include <toolchain/toolchain.h>
#include <pablo/pablo_toolchain.h>
#include <pablo/builder.hpp>
#include <pablo/pe_ones.h>
#include <pablo/pe_zeroes.h>
#include <pablo/bixnum/bixnum.h>
#include <fcntl.h>
#include <iostream>
#include <kernel/pipeline/driver/cpudriver.h>

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace pablo;

//  These declarations are for command line processing.
static cl::OptionCategory TransformOptions("Transform Options", "Transform control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(TransformOptions));

class IncrementBytes : public PabloKernel {
public:
    IncrementBytes(KernelBuilder & kb, StreamSet * basisBits, StreamSet * incrementedBasisBits)
        : PabloKernel(kb, "IncrementBytes",
                      {Binding{"basisBits", basisBits}},
                      {Binding{"incrementedBasisBits", incrementedBasisBits}}) {}
protected:
    void generatePabloMethod() override;
};

void IncrementBytes::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);

    std::vector<PabloAST *> basisBits = getInputStreamSet("basisBits");
    std::vector<PabloAST *> incrementedBasisBits(8);

    PabloAST * one = pb.createOnes();
    
    for (unsigned i = 0; i < 8; ++i) {
        incrementedBasisBits[i] = pb.createXor(basisBits[i], one);
        one = pb.createAdvance(one, 1);
    }

    Var * incrementedVar = getOutputStreamVar("incrementedBasisBits");
    for (unsigned i = 0; i < 8; ++i) {
        pb.createAssign(pb.createExtract(incrementedVar, pb.getInteger(i)), incrementedBasisBits[i]);
    }
}

typedef void (*TransformFunctionType)(uint32_t fd);

TransformFunctionType generatePipeline(CPUDriver & pxDriver) {
    auto & b = pxDriver.getBuilder();
    auto P = pxDriver.makePipeline({Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});
    Scalar * fileDescriptor = P->getInputScalar("inputFileDescriptor");
    StreamSet * ByteStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    SHOW_BYTES(ByteStream);

    StreamSet * BasisBits = P->CreateStreamSet(8);
    P->CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    SHOW_BIXNUM(BasisBits);

    StreamSet * incrementedBasisBits = P->CreateStreamSet(8);
    P->CreateKernelCall<IncrementBytes>(BasisBits, incrementedBasisBits);
    SHOW_BIXNUM(incrementedBasisBits);

    StreamSet * IncrementedBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(incrementedBasisBits, IncrementedBytes);
    SHOW_BYTES(IncrementedBytes);

    P->CreateKernelCall<StdOutKernel>(IncrementedBytes);

    return reinterpret_cast<TransformFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&TransformOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});
    CPUDriver driver("transform");
    TransformFunctionType fn = generatePipeline(driver);
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        fn(fd);
        close(fd);
    }
    return 0;
}
