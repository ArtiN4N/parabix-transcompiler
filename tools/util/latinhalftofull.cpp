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


using namespace kernel;
using namespace llvm;
using namespace codegen;

class InsertExtraAKernel : public MultiBlockKernel {
public:
    InsertExtraAKernel(KernelBuilder &b, StreamSet *inputStream, StreamSet *outputStream)
    : MultiBlockKernel(b, "InsertExtraAKernel",
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
        
        // Load input stream block
        Value * inputBlock = b.loadInputStreamBlock("inputStream", b.getInt32(0), b.getInt32(0));

        // Define the mask for 'a' characters
        Value * isA = b.CreateICmpEQ(inputBlock, b.getInt8('a'));

        // Create output stream blocks
        Value * outputBlock1 = b.CreateVectorSplat(bitBlockType->getPrimitiveSizeInBits() / 8, b.getInt8(0));
        Value * outputBlock2 = b.CreateVectorSplat(bitBlockType->getPrimitiveSizeInBits() / 8, b.getInt8(0));

        // Insert 'a' and extra 'a's in the output blocks
        b.CreateInsertElement(outputBlock1, b.getInt8('a'), b.getInt32(0));
        b.CreateInsertElement(outputBlock2, b.getInt8('a'), b.getInt32(1));
        b.CreateInsertElement(outputBlock2, b.getInt8('a'), b.getInt32(2));

        // Conditionally store the output blocks based on 'isA' mask
        Value * output = b.CreateSelect(isA, outputBlock2, outputBlock1);

        // Store output block
        b.storeOutputStreamBlock("outputStream", b.getInt32(0), b.getInt32(0), output);
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

    // Insert extra 'a' kernel transformation
    StreamSet * const outputStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<InsertExtraAKernel>(codeUnitStream, outputStream);

    // Output
    P->CreateKernelCall<StdOutKernel>(outputStream);

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
