#include <kernel/core/kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>  
#include <kernel/core/kernel_builder.h>
#include <kernel/core/streamset.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/StringRef.h>
#include <unicode/algo/decomposition.h>
#include <unicode/core/unicode_set.h>
#include <unicode/data/PropertyAliases.h>
#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>
#include <toolchain/toolchain.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace codegen;

class HalfwidthToFullwidthKernel : public MultiBlockKernel {
public:
    HalfwidthToFullwidthKernel(KernelBuilder &b, StreamSet *inputStream, StreamSet *outputStream)
    : MultiBlockKernel(b, "HalfwidthToFullwidthKernel",
        {Binding{"inputStream", inputStream}}, // input bindings
        {Binding{"outputStream", outputStream}}, // output bindings
        {}, // internal scalar bindings
        {}, // initializer bindings
        {}) // kernel state bindings
    {}

protected:
    void generateMultiBlockLogic(KernelBuilder &b, llvm::Value * const numOfBlocks) override {
        // bitBlockType represents the SIMD width, typically 128 or 256 bits
        Type * const bitBlockType = b.getBitBlockType();
        for (unsigned i = 0; i < b.getBitBlockWidth(); i += bitBlockType->getPrimitiveSizeInBits()) {
            std::cout << "loop: " << i << std::endl;
            Value * inputBlock = b.loadInputStreamBlock("inputStream", b.getInt32(0), b.getInt32(i));

            // Create a new empty block for the output
            Value * outputBlock1 = b.CreateVectorSplat(bitBlockType->getPrimitiveSizeInBits() / 8, b.getInt8(0xEF));
            Value * outputBlock2 = b.CreateVectorSplat(bitBlockType->getPrimitiveSizeInBits() / 8, b.getInt8(0xBC));
            Value * outputBlock3 = b.CreateVectorSplat(bitBlockType->getPrimitiveSizeInBits() / 8, b.getInt8(0x81));
            
            // Transform halfwidth characters to fullwidth characters
            for (unsigned j = 0; j < 64; ++j) {
                std::cout << "loop 2: " << j << std::endl;
                Value * inputChar = b.CreateExtractElement(inputBlock, b.getInt32(j));
                Value * isHalfwidth = b.CreateICmpUGE(inputChar, b.getInt8(0x21));
                isHalfwidth = b.CreateAnd(isHalfwidth, b.CreateICmpULE(inputChar, b.getInt8(0x7E)));
                
                if (isHalfwidth) {
                    Value * fullwidthChar = b.CreateAdd(inputChar, b.getInt32(0xFFBF));
                    b.CreateInsertElement(outputBlock1, fullwidthChar, b.getInt32(j * 3));
                    b.CreateInsertElement(outputBlock2, b.getInt32(0xBC), b.getInt32(j * 3 + 1));
                    b.CreateInsertElement(outputBlock3, b.getInt32(0x81), b.getInt32(j * 3 + 2));
                }
            }

            // Store output block
            b.storeOutputStreamBlock("outputStream", b.getInt32(0), b.getInt32(i), outputBlock1);
            b.storeOutputStreamBlock("outputStream", b.getInt32(1), b.getInt32(i), outputBlock2);
            b.storeOutputStreamBlock("outputStream", b.getInt32(2), b.getInt32(i), outputBlock3);
        }
    }
};

typedef void (*TransliteratorFunctionType)(uint32_t fd);

TransliteratorFunctionType transliterator_gen(CPUDriver & driver) {
    auto & b = driver.getBuilder();
    auto P = driver.makePipeline({Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});

    Scalar * fileDescriptor = P->getInputScalar("inputFileDescriptor");

    // Source data
    StreamSet * const codeUnitStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, codeUnitStream);
    SHOW_BYTES(codeUnitStream);

    // Halfwidth to Fullwidth transformation
    StreamSet * const fullwidthStream = P->CreateStreamSet(3, 8);
    P->CreateKernelCall<HalfwidthToFullwidthKernel>(codeUnitStream, fullwidthStream);

    // Output
    P->CreateKernelCall<StdOutKernel>(fullwidthStream);
    SHOW_BYTES(fullwidthStream);

    return reinterpret_cast<TransliteratorFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required);
    codegen::ParseCommandLineOptions(argc, argv);

    CPUDriver pxDriver("transliterator");
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        TransliteratorFunctionType func = nullptr;
        func = transliterator_gen(pxDriver);
        func(fd);
        close(fd);
    }
    return 0;
}