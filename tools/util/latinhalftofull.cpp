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

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace codegen;

class HalfwidthFullwidthKernel : public MultiBlockKernel {
public:
    HalfwidthFullwidthKernel(KernelBuilder &b, StreamSet *inputStream, StreamSet *outputStream)
    : MultiBlockKernel(b, "HalfwidthFullwidthKernel",
        {Binding{"inputStream", inputStream}}, // input bindings
        {Binding{"outputStream", outputStream}}, // output bindings
        {}, // internal scalar bindings
        {}, // initializer bindings
        {}) // kernel state bindings)
    {}

protected:
    void generateMultiBlockLogic(KernelBuilder &b, llvm::Value * const numOfBlocks) override {
        // bitBlockType is obtained from the KernelBuilder.
        // This type represents the SIMD width, typically 128 or 256 bits
        Type * const bitBlockType = b.getBitBlockType();

        // Load input block
        Value * inputBlock = b.loadInputStreamBlock("inputStream", b.getInt32(0), numOfBlocks);

        // Define constants for fullwidth transformation
        Value * baseOffset = b.getInt32(0xEFBC80); // Starting fullwidth offset for halfwidth characters

        // Create masks and transformations
        Value * isLatinRange = b.CreateAnd(
            b.CreateICmpUGE(inputBlock, b.getInt8(0x21)),
            b.CreateICmpULE(inputBlock, b.getInt8(0x7E))
        );

        Value * isHalfwidth = b.CreateAnd(inputBlock, isLatinRange);
        Value * fullwidthOffset = b.CreateAdd(baseOffset, b.CreateZExt(isHalfwidth, bitBlockType));

        // Store output block
        b.storeOutputStreamBlock("outputStream", b.getInt32(0), numOfBlocks, fullwidthOffset);
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
    StreamSet * const fullwidthStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<HalfwidthFullwidthKernel>(codeUnitStream, fullwidthStream);
    SHOW_BYTES(fullwidthStream);

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