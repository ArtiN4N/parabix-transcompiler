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

        // Create constants for character mappings
        // Define your Greek to Latin character mappings here
        std::unordered_map<wchar_t, std::wstring> translitMap = {
            {L'Α', L"A"}, {L'Β', L"B"}, {L'Γ', L"G"}, {L'Δ', L"D"}, {L'Ε', L"E"},
            {L'Ζ', L"Z"}, {L'Η', L"H"}, {L'Θ', L"TH"}, {L'Ι', L"I"}, {L'Κ', L"K"},
            {L'Λ', L"L"}, {L'Μ', L"M"}, {L'Ν', L"N"}, {L'Ξ', L"X"}, {L'Ο', L"O"},
            {L'Π', L"P"}, {L'Ρ', L"R"}, {L'Σ', L"S"}, {L'Τ', L"T"}, {L'Υ', L"Y"},
            {L'Φ', L"PH"}, {L'Χ', L"CH"}, {L'Ψ', L"PS"}, {L'Ω', L"O"},
            {L'α', L"a"}, {L'β', L"b"}, {L'γ', L"g"}, {L'δ', L"d"}, {L'ε', L"e"},
            {L'ζ', L"z"}, {L'η', L"h"}, {L'θ', L"th"}, {L'ι', L"i"}, {L'κ', L"k"},
            {L'λ', L"l"}, {L'μ', L"m"}, {L'ν', L"n"}, {L'ξ', L"x"}, {L'ο', L"o"},
            {L'π', L"p"}, {L'ρ', L"r"}, {L'σ', L"s"}, {L'τ', L"t"}, {L'υ', L"y"},
            {L'φ', L"ph"}, {L'χ', L"ch"}, {L'ψ', L"ps"}, {L'ω', L"o"}
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

            // Perform transliteration
            std::wstring result;
            for (wchar_t ch : translitMap) {
                auto it = translitMap.find(ch);
                if (it != translitMap.end()) {
                    result += it->second;
                } else {
                    result += ch; // Preserve characters that are not in the map
                }
            }

            // Store output block
            b.storeOutputStreamBlock("outputStream", b.getInt32(0), offset, result);
        }

        Value * nextBlockIdx = b.CreateAdd(blockIdx, b.getInt32(1));
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
    cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required);
    codegen::ParseCommandLineOptions(argc, argv);

    CPUDriver pxDriver("greektolatin");
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        GreekToLatinFunctionType func = nullptr;
        func = generatePipeline(pxDriver);
        func(fd);
        close(fd);
    }
    return 0;
}
