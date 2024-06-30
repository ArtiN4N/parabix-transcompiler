#include <kernel/core/kernel.h>
#include <kernel/io/stdout.h>
#include <kernel/builder.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/io/source.h>
#include <llvm/Support/CommandLine.h>

using namespace kernel;

class UppercaseKernel : public MultiBlockKernel {
public:
    UppercaseKernel(BuilderRef b)
    : MultiBlockKernel(b, "UppercaseKernel",
        {Binding{"inputStream", 8}},
        {Binding{"outputStream", 8}}) {}

protected:
    void generateMultiBlockLogic(BuilderRef b, llvm::Value * const numOfBlocks) override {
        // Load input and apply the uppercase transformation
        Value * input = b->getInputStreamBlockPtr("inputStream", b->getInt32(0));
        Value * output = b->getOutputStreamBlockPtr("outputStream", b->getInt32(0));

        // Uppercase transformation logic
        for (unsigned i = 0; i < b->getBitBlockWidth(); ++i) {
            Value * charValue = b->simd_load(b->getBitBlockType(), input, b->getInt32(i));
            Value * isLowercase = b->simd_icmp_eq(b->simd_and(charValue, b->simd_fill(0x20)), b->simd_fill(0x20));
            Value * upperValue = b->simd_sub(charValue, b->simd_and(isLowercase, b->simd_fill(0x20)));
            b->simd_store(upperValue, output, b->getInt32(i));
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

    // Uppercase transformation
    StreamSet * const upperStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<UppercaseKernel>(codeUnitStream, upperStream);

    // Output
    P->CreateKernelCall<StdOutKernel>(upperStream);

    return reinterpret_cast<TransliteratorFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&TransliteratorOptions, codegen::codegen_flags()});
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
