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

class LowercaseKernel : public MultiBlockKernel {
public:
    LowercaseKernel(KernelBuilder &b, StreamSet *inputStream, StreamSet *outputStream)
    : MultiBlockKernel(b, "LowercaseKernel",
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
        unsigned bitBlockWidth = bitBlockType->getPrimitiveSizeInBits();
        unsigned numElements = bitBlockWidth / 8; // Number of 8-bit elements in a bit block

        // Load input and apply the lowercase transformation
        for (unsigned i = 0; i < b.getBitBlockWidth(); i += bitBlockWidth) {
            // Load input block
            Value *inputBlock = b.loadInputStreamBlock("inputStream", b.getInt32(0), b.getInt32(i));

            // Initialize a vector to hold the transformed characters
            Value *lowercaseBlock = UndefValue::get(bitBlockType);

            // Loop through each byte in the block
            for (unsigned j = 0; j < numElements; ++j) {
                // Extract the character from the input block
                Value *charValue = b.CreateExtractElement(inputBlock, b.getInt32(j));
                charValue = b.CreateTrunc(charValue, b.getInt8Ty());  // Ensure charValue is treated as i8

                llvm::errs() << "Original char: " << *charValue << "\n"; // Debug output

                // Check if the character is uppercase (between 'A' and 'Z')
                Value *isUppercase = b.CreateAnd(
                    b.CreateICmpUGE(charValue, b.getInt8('A')),
                    b.CreateICmpULE(charValue, b.getInt8('Z'))
                );
                llvm::errs() << "Is uppercase: " << *isUppercase << "\n"; // Debug output

                // Calculate the lowercase character if it is uppercase
                Value *lowerChar = b.CreateSelect(isUppercase, b.CreateAdd(charValue, b.getInt8(32)), charValue);

                llvm::errs() << "Lowercase char: " << *lowerChar << "\n"; // Debug output

                // Insert the transformed character into the lowercase block
                lowercaseBlock = b.CreateInsertElement(lowercaseBlock, lowerChar, b.getInt32(j));
            }

            // Store the output block
            b.storeOutputStreamBlock("outputStream", b.getInt32(0), b.getInt32(i), lowercaseBlock);
        }
    }
};

typedef void (*TransliteratorFunctionType)(uint32_t fd);

TransliteratorFunctionType transliterator_gen(CPUDriver &driver) {
    auto &b = driver.getBuilder();
    auto P = driver.makePipeline({Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});

    Scalar *fileDescriptor = P->getInputScalar("inputFileDescriptor");

    // Source data
    StreamSet *const codeUnitStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, codeUnitStream);
    SHOW_BYTES(codeUnitStream);

    // Lowercase transformation
    StreamSet *const lowerStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<LowercaseKernel>(codeUnitStream, lowerStream);
    SHOW_BYTES(lowerStream);

    // Output
    P->CreateKernelCall<StdOutKernel>(lowerStream);
    SHOW_BYTES(lowerStream);

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
