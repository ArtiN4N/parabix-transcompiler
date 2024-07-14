#include <cstdio>
#include <vector>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <pablo/codegenstate.h>
#include <pablo/pe_zeroes.h>        // for Zeroes
#include <pablo/bixnum/bixnum.h>
#include <grep/grep_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/run_index.h>
#include <kernel/streamutils/string_insert.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <re/adt/re_name.h>
#include <re/cc/cc_kernel.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <string>
#include <toolchain/toolchain.h>
#include <pablo/pablo_toolchain.h>
#include <fcntl.h>
#include <iostream>
#include <kernel/pipeline/driver/cpudriver.h>
#include <unicode/algo/decomposition.h>
#include <unicode/core/unicode_set.h>
#include <unicode/data/PropertyAliases.h>
#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>
#include <codecvt>
#include <re/toolchain/toolchain.h>

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace pablo;

// These declarations are for command line processing.
// See the LLVM CommandLine 2.0 Library Manual https://llvm.org/docs/CommandLine.html
// Command line options category
static cl::OptionCategory GreekToLatinOptions("greektolatin Options", "greektolatin control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(GreekToLatinOptions));

class GreekToLatinKernel : public MultiBlockKernel {
public:
    GreekToLatinKernel(KernelBuilder &b, StreamSet *inputStream, StreamSet *outputStream)
    : MultiBlockKernel(b, "GreekToLatinKernel",
        {Binding{"inputStream", inputStream}}, // input bindings
        {Binding{"outputStream", outputStream}}, // output bindings
        {}, // internal scalar bindings
        {}, // initializer bindings
        {}) // kernel state bindings
    {}

protected:
    void generateMultiBlockLogic(KernelBuilder &b, llvm::Value * const numOfBlocks) override {
        Type * const bitBlockType = b.getBitBlockType();
        const unsigned fieldWidth = bitBlockType->getPrimitiveSizeInBits() / 8;
        Value * const bitBlockWidth = b.getInt32(b.getBitBlockWidth());

        // Define codepoint mappings for Greek to Latin transliteration
        std::unordered_map<uint32_t, std::string> translitMap = {
            {0x0391, "A"}, {0x0392, "B"}, {0x0393, "G"}, {0x0394, "D"}, {0x0395, "E"},
            {0x0396, "Z"}, {0x0397, "H"}, {0x0398, "TH"}, {0x0399, "I"}, {0x039A, "K"},
            {0x039B, "L"}, {0x039C, "M"}, {0x039D, "N"}, {0x039E, "X"}, {0x039F, "O"},
            {0x03A0, "P"}, {0x03A1, "R"}, {0x03A3, "S"}, {0x03A4, "T"}, {0x03A5, "Y"},
            {0x03A6, "PH"}, {0x03A7, "CH"}, {0x03A8, "PS"}, {0x03A9, "O"},
            {0x03B1, "a"}, {0x03B2, "b"}, {0x03B3, "g"}, {0x03B4, "d"}, {0x03B5, "e"},
            {0x03B6, "z"}, {0x03B7, "h"}, {0x03B8, "th"}, {0x03B9, "i"}, {0x03BA, "k"},
            {0x03BB, "l"}, {0x03BC, "m"}, {0x03BD, "n"}, {0x03BE, "x"}, {0x03BF, "o"},
            {0x03C0, "p"}, {0x03C1, "r"}, {0x03C3, "s"}, {0x03C4, "t"}, {0x03C5, "y"},
            {0x03C6, "ph"}, {0x03C7, "ch"}, {0x03C8, "ps"}, {0x03C9, "o"}
        };

        BasicBlock * entryBlock = b.GetInsertBlock();
        Function * func = entryBlock->getParent();
        BasicBlock * loopCond = BasicBlock::Create(b.getContext(), "loopCond", func);
        BasicBlock * loopBody = BasicBlock::Create(b.getContext(), "loopBody", func);
        BasicBlock * afterLoop = BasicBlock::Create(b.getContext(), "afterLoop", func);

        b.CreateBr(loopCond);

        b.SetInsertPoint(loopCond);
        PHINode * blockIdx = b.CreatePHI(b.getInt32Ty(), 2);
        blockIdx->addIncoming(b.getInt32(0), entryBlock);
        Value * currentBlock = b.CreateMul(blockIdx, bitBlockWidth);
        Value * loopCondVal = b.CreateICmpULT(blockIdx, numOfBlocks);
        b.CreateCondBr(loopCondVal, loopBody, afterLoop);

        b.SetInsertPoint(loopBody);

        for (unsigned i = 0; i < b.getBitBlockWidth(); i += fieldWidth) {
            Value * offset = b.CreateAdd(currentBlock, b.getInt32(i));

            // Load input block
            Value * inputBlock = b.loadInputStreamBlock("inputStream", b.getInt32(0), offset);

            // Create an empty vector to hold the result characters
            std::vector<Value*> result;

            // Loop through each character in the input block
            for (unsigned j = 0; j < fieldWidth; ++j) {
                Value *charVal = b.CreateExtractElement(inputBlock, b.getInt32(j));
                Value *charCode = b.CreateZExtOrBitCast(charVal, b.getInt32Ty());

                auto it = translitMap.find(static_cast<uint32_t>(cast<ConstantInt>(charCode)->getZExtValue()));
                if (it != translitMap.end()) {
                    for (auto latinChar : it->second) {
                        result.push_back(b.getInt8(static_cast<uint8_t>(latinChar)));
                    }
                } else {
                    result.push_back(charVal); // Preserve characters that are not in the map
                }
            }

            // Convert the result vector into an LLVM vector
            Value *resultBlock = UndefValue::get(VectorType::get(b.getInt8Ty(), result.size()));
            for (unsigned k = 0; k < result.size(); ++k) {
                resultBlock = b.CreateInsertElement(resultBlock, result[k], b.getInt32(k));
            }

            // Store the output block
            b.storeOutputStreamBlock("outputStream", b.getInt32(0), offset, resultBlock);
        }

        Value *nextBlockIdx = b.CreateAdd(blockIdx, b.getInt32(1));
        blockIdx->addIncoming(nextBlockIdx, loopBody);
        b.CreateBr(loopCond);

        b.SetInsertPoint(afterLoop);
    }
};

typedef void (*GreekToLatinFunctionType)(uint32_t fd);

GreekToLatinFunctionType generatePipeline(CPUDriver & driver) {
    auto & b = driver.getBuilder();
    auto P = driver.makePipeline({Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});

    Scalar * fileDescriptor = P->getInputScalar("inputFileDescriptor");

    // Source data
    StreamSet * const codeUnitStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, codeUnitStream);
    SHOW_BYTES(codeUnitStream);

    // Greek to Latin transformation
    StreamSet * const translitStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<GreekToLatinKernel>(codeUnitStream, translitStream);

    // Output
    P->CreateKernelCall<StdOutKernel>(translitStream);
    SHOW_BYTES(translitStream);

    return reinterpret_cast<GreekToLatinFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    // Parse command line options
    cl::ParseCommandLineOptions(argc, argv, "greektolatin\n");

    // Check if inputFile is provided
    if (inputFile.empty()) {
        errs() << "Error: Must specify an input file.\n";
        return 1;
    }

    // A CPU driver is capable of compiling and running Parabix programs on the CPU.
    CPUDriver pxDriver("greektolatin");

    // Build and compile the Parabix pipeline by calling the Pipeline function above.
    GreekToLatinFunctionType fn = generatePipeline(pxDriver);

    // The compiled function "fn" can now be used. It takes a file
    // descriptor as an input, which is specified by the filename given by
    // the inputFile command line option.
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
        return 1;
    } else {
        // Run the pipeline.
        fn(fd);
        close(fd);
    }

    return 0;
}
