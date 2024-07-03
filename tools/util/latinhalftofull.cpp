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

class UppercaseKernel : public MultiBlockKernel {
public:
    UppercaseKernel(KernelBuilder &b, StreamSet *inputStream, StreamSet *outputStream)
    : MultiBlockKernel(b, "UppercaseKernel",
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
        Type * const bitBlockType = b.getBitBlockType(); // 

        // Load input and apply the uppercase transformation
        for (unsigned i = 0; i < b.getBitBlockWidth(); i += bitBlockType->getPrimitiveSizeInBits()) {
            // Load input block
            Value * inputBlock = b.loadInputStreamBlock("inputStream", b.getInt32(0), b.getInt32(i));

            // Mask to identify lowercase letters (0x20)
            Value * lowercaseMask = b.CreateVectorSplat(bitBlockType->getPrimitiveSizeInBits() / 8, b.getInt8(0x20));
            llvm::errs() << "lowerCaseMask: " << *lowercaseMask << "\n"; // for testing
            llvm::errs() << "bitBlockType: " << *bitBlockType << "\n"; // for testing
            llvm::errs() << "getPrimitiveSizeInBits: " << bitBlockType->getPrimitiveSizeInBits() << "\n"; // for testing
            llvm::errs() << "getInt8(0x20): " << *b.getInt8(0x20) << "\n"; // for testing

            Value * uppercaseMask = b.CreateVectorSplat(bitBlockType->getPrimitiveSizeInBits() / 8, b.getInt8(0xDF));

            // Check if characters are lowercase
            Value * isLowercase = b.CreateICmpEQ(b.CreateAnd(inputBlock, lowercaseMask), lowercaseMask);

            // Calculate uppercase values
            Value * uppercaseBlock = b.CreateOr(
                b.CreateAnd(inputBlock, uppercaseMask), 
                b.CreateAnd(isLowercase, lowercaseMask)
            );

            // Store output block
            b.storeOutputStreamBlock("outputStream", b.getInt32(0), b.getInt32(i), uppercaseBlock);
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


    // Uppercase transformation
    StreamSet * const upperStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<UppercaseKernel>(codeUnitStream, upperStream);

    // Output
    P->CreateKernelCall<StdOutKernel>(upperStream);
    SHOW_BYTES(upperStream);

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
