// #include <cstdio>
// #include <vector>
// #include <llvm/Support/CommandLine.h>
// #include <llvm/Support/ErrorHandling.h>
// #include <llvm/Support/raw_ostream.h>
// #include <llvm/IR/Module.h>
// #include <re/adt/re_name.h>
// #include <re/adt/re_re.h>
// #include <pablo/codegenstate.h>
// #include <pablo/pe_zeroes.h>        // for Zeroes
// #include <pablo/bixnum/bixnum.h>
// #include <grep/grep_kernel.h>
// #include <kernel/core/kernel_builder.h>
// #include <kernel/pipeline/pipeline_builder.h>
// #include <kernel/streamutils/deletion.h>
// #include <kernel/streamutils/pdep_kernel.h>
// #include <kernel/streamutils/run_index.h>
// #include <kernel/streamutils/string_insert.h>
// #include <kernel/basis/s2p_kernel.h>
// #include <kernel/basis/p2s_kernel.h>
// #include <kernel/io/source_kernel.h>
// #include <kernel/io/stdout_kernel.h>
// #include <kernel/unicode/charclasses.h>
// #include <kernel/unicode/utf8gen.h>
// #include <kernel/unicode/utf8_decoder.h>
// #include <kernel/unicode/UCD_property_kernel.h>
// #include <re/adt/re_name.h>
// #include <re/cc/cc_kernel.h>
// #include <re/cc/cc_compiler.h>
// #include <re/cc/cc_compiler_target.h>
// #include <string>
// #include <toolchain/toolchain.h>
// #include <pablo/pablo_toolchain.h>
// #include <fcntl.h>
// #include <iostream>
// #include <kernel/pipeline/driver/cpudriver.h>
// #include <unicode/algo/decomposition.h>
// #include <unicode/core/unicode_set.h>
// #include <unicode/data/PropertyAliases.h>
// #include <unicode/data/PropertyObjects.h>
// #include <unicode/data/PropertyObjectTable.h>
// #include <unicode/utf/utf_compiler.h>
// #include <unicode/utf/transchar.h>
// #include <codecvt>
// #include <re/toolchain/toolchain.h>

// #define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
// #define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
// #define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

// using namespace kernel;
// using namespace llvm;
// using namespace pablo;

// static cl::OptionCategory GreekToLatinOptions("greektolatin Options", "greektolatin control options.");
// static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(GreekToLatinOptions));

// class GreekToLatinKernel : public MultiBlockKernel {
// public:
//     GreekToLatinKernel(KernelBuilder &b, StreamSet *inputStream, StreamSet *outputStream)
//     : MultiBlockKernel(b, "GreekToLatinKernel",
//         {Binding{"inputStream", inputStream}}, // input bindings
//         {Binding{"outputStream", outputStream}}, // output bindings
//         {}, // internal scalar bindings
//         {}, // initializer bindings
//         {}) // kernel state bindings
//     {}

// protected:
//     void generateMultiBlockLogic(KernelBuilder &b, llvm::Value * const numOfBlocks) override {
//         Type * const bitBlockType = b.getBitBlockType();
//         const unsigned fieldWidth = bitBlockType->getPrimitiveSizeInBits() / 8;
//         Value * const bitBlockWidth = b.getInt32(b.getBitBlockWidth());

//         // Define codepoint mappings for Greek to Latin transliteration
//         std::unordered_map<uint32_t, std::string> translitMap = {
//             {0x0391, "A"}, {0x0392, "B"}, {0x0393, "G"}, {0x0394, "D"}, {0x0395, "E"},
//             {0x0396, "Z"}, {0x0397, "H"}, {0x0398, "TH"}, {0x0399, "I"}, {0x039A, "K"},
//             {0x039B, "L"}, {0x039C, "M"}, {0x039D, "N"}, {0x039E, "X"}, {0x039F, "O"},
//             {0x03A0, "P"}, {0x03A1, "R"}, {0x03A3, "S"}, {0x03A4, "T"}, {0x03A5, "Y"},
//             {0x03A6, "PH"}, {0x03A7, "CH"}, {0x03A8, "PS"}, {0x03A9, "O"},
//             {0x03B1, "a"}, {0x03B2, "b"}, {0x03B3, "g"}, {0x03B4, "d"}, {0x03B5, "e"},
//             {0x03B6, "z"}, {0x03B7, "h"}, {0x03B8, "th"}, {0x03B9, "i"}, {0x03BA, "k"},
//             {0x03BB, "l"}, {0x03BC, "m"}, {0x03BD, "n"}, {0x03BE, "x"}, {0x03BF, "o"},
//             {0x03C0, "p"}, {0x03C1, "r"}, {0x03C3, "s"}, {0x03C4, "t"}, {0x03C5, "y"},
//             {0x03C6, "ph"}, {0x03C7, "ch"}, {0x03C8, "ps"}, {0x03C9, "o"}
//         };

//         BasicBlock * entryBlock = b.GetInsertBlock();
//         Function * func = entryBlock->getParent();
//         BasicBlock * loopCond = BasicBlock::Create(b.getContext(), "loopCond", func);
//         BasicBlock * loopBody = BasicBlock::Create(b.getContext(), "loopBody", func);
//         BasicBlock * afterLoop = BasicBlock::Create(b.getContext(), "afterLoop", func);

//         b.CreateBr(loopCond);

//         b.SetInsertPoint(loopCond);
//         PHINode * blockIdx = b.CreatePHI(b.getInt32Ty(), 2);
//         blockIdx->addIncoming(b.getInt32(0), entryBlock);
//         Value * currentBlock = b.CreateMul(blockIdx, bitBlockWidth);
//         Value * loopCondVal = b.CreateICmpULT(blockIdx, numOfBlocks);
//         b.CreateCondBr(loopCondVal, loopBody, afterLoop);

//         b.SetInsertPoint(loopBody);

//         for (unsigned i = 0; i < b.getBitBlockWidth(); i += fieldWidth) {
//             Value * offset = b.CreateAdd(currentBlock, b.getInt32(i));

//             // Load input block
//             Value * inputBlock = b.loadInputStreamBlock("inputStream", b.getInt32(0), offset);

//             // Create an empty vector to hold the result characters
//             std::vector<Value*> result;

//             // Loop through each character in the input block
//             for (unsigned j = 0; j < fieldWidth; ++j) {
//                 Value *charVal = b.CreateExtractElement(inputBlock, b.getInt32(j));
//                 Value *charCode = b.CreateZExtOrBitCast(charVal, b.getInt32Ty());

//                 auto it = translitMap.find(static_cast<uint32_t>(cast<ConstantInt>(charCode)->getZExtValue()));
//                 if (it != translitMap.end()) {
//                     for (auto latinChar : it->second) {
//                         result.push_back(b.getInt8(static_cast<uint8_t>(latinChar)));
//                     }
//                 } else {
//                     result.push_back(charVal); // Preserve characters that are not in the map
//                 }
//             }

//             // Convert the result vector into an LLVM array
//             ArrayType *resultArrayType = ArrayType::get(b.getInt8Ty(), result.size());
//             Value *resultBlock = UndefValue::get(resultArrayType);
//             for (unsigned k = 0; k < result.size(); ++k) {
//                 resultBlock = b.CreateInsertValue(resultBlock, result[k], k);
//             }

//             // Store the output block
//             b.storeOutputStreamBlock("outputStream", b.getInt32(0), offset, resultBlock);
//         }

//         Value *nextBlockIdx = b.CreateAdd(blockIdx, b.getInt32(1));
//         blockIdx->addIncoming(nextBlockIdx, loopBody);
//         b.CreateBr(loopCond);

//         b.SetInsertPoint(afterLoop);
//     }
// };

// typedef void (*GreekToLatinFunctionType)(uint32_t fd);

// GreekToLatinFunctionType generatePipeline(CPUDriver & driver) {
//     auto & b = driver.getBuilder();
//     auto P = driver.makePipeline({Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});

//     Scalar * fileDescriptor = P->getInputScalar("inputFileDescriptor");

//     // Source data
//     StreamSet * const codeUnitStream = P->CreateStreamSet(1, 8);
//     P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, codeUnitStream);

//     // Greek to Latin transformation
//     StreamSet * const translitStream = P->CreateStreamSet(1, 8);
//     P->CreateKernelCall<GreekToLatinKernel>(codeUnitStream, translitStream);

//     // Output
//     P->CreateKernelCall<StdOutKernel>(translitStream);

//     return reinterpret_cast<GreekToLatinFunctionType>(P->compile());
// }

// int main(int argc, char *argv[]) {
//     // Parse command line options
//     cl::ParseCommandLineOptions(argc, argv, "greektolatin\n");

//     // Check if inputFile is provided
//     if (inputFile.empty()) {
//         errs() << "Error: Must specify an input file.\n";
//         return 1;
//     }

//     // A CPU driver is capable of compiling and running Parabix programs on the CPU.
//     CPUDriver pxDriver("greektolatin");

//     // Build and compile the Parabix pipeline by calling the Pipeline function above.
//     GreekToLatinFunctionType fn = generatePipeline(pxDriver);

//     // The compiled function "fn" can now be used. It takes a file
//     // descriptor as an input, which is specified by the filename given by
//     // the inputFile command line option.
//     const int fd = open(inputFile.c_str(), O_RDONLY);
//     if (LLVM_UNLIKELY(fd == -1)) {
//         errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
//         return 1;
//     } else {
//         // Run the pipeline.
//         fn(fd);
//         close(fd);
//     }

//     return 0;
// }

// Command line options
// static cl::OptionCategory GreekToLatinOptions("greektolatin Options", "greektolatin control options.");
// static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(GreekToLatinOptions));

// class GreekToLatinKernel : public MultiBlockKernel {
// public:
//     GreekToLatinKernel(KernelBuilder &b, StreamSet *inputStream, StreamSet *outputStream)
//     : MultiBlockKernel(b, "GreekToLatinKernel",
//         {Binding{"inputStream", inputStream}}, // input bindings
//         {Binding{"outputStream", outputStream}}, // output bindings
//         {}, // internal scalar bindings
//         {}, // initializer bindings
//         {}) // kernel state bindings
//     {}

// protected:
//     void generateMultiBlockLogic(KernelBuilder &b, llvm::Value * const numOfBlocks) override {
//         Type * const bitBlockType = b.getBitBlockType();
//         Value * const bitBlockWidth = b.getInt32(b.getBitBlockWidth());

//         BasicBlock * entryBlock = b.GetInsertBlock();
//         Function * func = entryBlock->getParent();
//         BasicBlock * loopCond = BasicBlock::Create(b.getContext(), "loopCond", func);
//         BasicBlock * loopBody = BasicBlock::Create(b.getContext(), "loopBody", func);
//         BasicBlock * afterLoop = BasicBlock::Create(b.getContext(), "afterLoop", func);

//         b.CreateBr(loopCond);

//         b.SetInsertPoint(loopCond);
//         PHINode * blockIdx = b.CreatePHI(b.getInt32Ty(), 2);
//         blockIdx->addIncoming(b.getInt32(0), entryBlock);
//         Value * currentBlock = b.CreateMul(blockIdx, bitBlockWidth);
//         Value * loopCondVal = b.CreateICmpULT(blockIdx, numOfBlocks);
//         b.CreateCondBr(loopCondVal, loopBody, afterLoop);

//         b.SetInsertPoint(loopBody);

//         for (unsigned i = 0; i < b.getBitBlockWidth(); ++i) {
//             Value * offset = b.CreateAdd(currentBlock, b.getInt32(i));

//             // Load input block
//             Value * inputBlock = b.loadInputStreamBlock("inputStream", b.getInt32(0), offset);

//             // Simply copy input to output
//             b.storeOutputStreamBlock("outputStream", b.getInt32(0), offset, inputBlock);
//         }

//         Value *nextBlockIdx = b.CreateAdd(blockIdx, b.getInt32(1));
//         blockIdx->addIncoming(nextBlockIdx, loopBody);
//         b.CreateBr(loopCond);

//         b.SetInsertPoint(afterLoop);
//     }
// };

// typedef void (*GreekToLatinFunctionType)(uint32_t fd);

// GreekToLatinFunctionType generatePipeline(CPUDriver & driver) {
//     auto & b = driver.getBuilder();
//     auto P = driver.makePipeline({Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});

//     Scalar * fileDescriptor = P->getInputScalar("inputFileDescriptor");

//     // Source data
//     StreamSet * const codeUnitStream = P->CreateStreamSet(1, 8);
//     P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, codeUnitStream);

//     // Greek to Latin transformation (identity transformation for testing)
//     StreamSet * const translitStream = P->CreateStreamSet(1, 8);
//     P->CreateKernelCall<GreekToLatinKernel>(codeUnitStream, translitStream);

//     // Output
//     P->CreateKernelCall<StdOutKernel>(translitStream);

//     return reinterpret_cast<GreekToLatinFunctionType>(P->compile());
// }

// int main(int argc, char *argv[]) {
//     // Parse command line options
//     cl::ParseCommandLineOptions(argc, argv, "greektolatin\n");

//     // Check if inputFile is provided
//     if (inputFile.empty()) {
//         errs() << "Error: Must specify an input file.\n";
//         return 1;
//     }

//     // A CPU driver is capable of compiling and running Parabix programs on the CPU.
//     CPUDriver pxDriver("greektolatin");

//     // Build and compile the Parabix pipeline by calling the Pipeline function above.
//     GreekToLatinFunctionType fn = generatePipeline(pxDriver);

//     // The compiled function "fn" can now be used. It takes a file
//     // descriptor as an input, which is specified by the filename given by
//     // the inputFile command line option.
//     const int fd = open(inputFile.c_str(), O_RDONLY);
//     if (LLVM_UNLIKELY(fd == -1)) {
//         errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
//         return 1;
//     } else {
//         // Run the pipeline.
//         fn(fd);
//         close(fd);
//     }

//     return 0;
// }

#include <cstdio>
#include <vector>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <pablo/codegenstate.h>
#include <pablo/pe_zeroes.h>
#include <pablo/bixnum/bixnum.h>
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
#include <unicode/algo/decomposition.h>
#include <unicode/core/unicode_set.h>
#include <unicode/data/PropertyAliases.h>
#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>
#include <codecvt>
#include <re/toolchain/toolchain.h>
#include <string>
#include <fcntl.h>
#include <iostream>
#include <kernel/pipeline/driver/cpudriver.h>

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace pablo;

static cl::OptionCategory GreekToLatin_Options("Greek to Latin Options", "Greek to Latin Transliteration Options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(GreekToLatin_Options));
static cl::opt<std::string> outputFile("o", cl::desc("Specify output filename"), cl::value_desc("filename"), cl::init("-"), cl::cat(GreekToLatin_Options));

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

class GreekToLatin_BixData {
public:
    GreekToLatin_BixData();
    unicode::BitTranslationSets GreekToLatin_1st_BitXorCCs();
    unicode::BitTranslationSets GreekToLatin_2nd_BitCCs();
    unicode::BitTranslationSets GreekToLatin_3rd_BitCCs();
    unicode::BitTranslationSets GreekToLatin_4th_BitCCs();
private:
    std::unordered_map<UCD::codepoint_t, unsigned> mGreekToLatin_length;
    unicode::TranslationMap mGreekToLatin_CharMap[4];
};

GreekToLatin_BixData::GreekToLatin_BixData() {
    // Initialize the Greek to Latin mapping
    mGreekToLatin_CharMap[0].emplace(0x0391, 0x0041); // Greek Capital Letter Alpha to Latin Capital Letter A
    mGreekToLatin_CharMap[0].emplace(0x03B1, 0x0061); // Greek Small Letter Alpha to Latin Small Letter a
    mGreekToLatin_CharMap[0].emplace(0x0392, 0x0042); // Greek Capital Letter Beta to Latin Capital Letter B
    mGreekToLatin_CharMap[0].emplace(0x03B2, 0x0062); // Greek Small Letter Beta to Latin Small Letter b
    mGreekToLatin_CharMap[0].emplace(0x0393, 0x0043); // Greek Capital Letter Gamma to Latin Capital Letter C
    mGreekToLatin_CharMap[0].emplace(0x03B3, 0x0063); // Greek Small Letter Gamma to Latin Small Letter c
    mGreekToLatin_CharMap[0].emplace(0x0394, 0x0044); // Greek Capital Letter Delta to Latin Capital Letter D
    mGreekToLatin_CharMap[0].emplace(0x03B4, 0x0064); // Greek Small Letter Delta to Latin Small Letter d
    mGreekToLatin_CharMap[0].emplace(0x0395, 0x0045); // Greek Capital Letter Epsilon to Latin Capital Letter E
    mGreekToLatin_CharMap[0].emplace(0x03B5, 0x0065); // Greek Small Letter Epsilon to Latin Small Letter e
    mGreekToLatin_CharMap[0].emplace(0x0396, 0x0046); // Greek Capital Letter Zeta to Latin Capital Letter F
    mGreekToLatin_CharMap[0].emplace(0x03B6, 0x0066); // Greek Small Letter Zeta to Latin Small Letter f
    mGreekToLatin_CharMap[0].emplace(0x0397, 0x0047); // Greek Capital Letter Eta to Latin Capital Letter G
    mGreekToLatin_CharMap[0].emplace(0x03B7, 0x0067); // Greek Small Letter Eta to Latin Small Letter g
    mGreekToLatin_CharMap[0].emplace(0x0398, 0x0048); // Greek Capital Letter Theta to Latin Capital Letter H
    mGreekToLatin_CharMap[0].emplace(0x03B8, 0x0068); // Greek Small Letter Theta to Latin Small Letter h
    mGreekToLatin_CharMap[0].emplace(0x0399, 0x0049); // Greek Capital Letter Iota to Latin Capital Letter I
    mGreekToLatin_CharMap[0].emplace(0x03B9, 0x0069); // Greek Small Letter Iota to Latin Small Letter i
    mGreekToLatin_CharMap[0].emplace(0x039A, 0x004A); // Greek Capital Letter Kappa to Latin Capital Letter J
    mGreekToLatin_CharMap[0].emplace(0x03BA, 0x006A); // Greek Small Letter Kappa to Latin Small Letter j
    mGreekToLatin_CharMap[0].emplace(0x039B, 0x004B); // Greek Capital Letter Lambda to Latin Capital Letter K
    mGreekToLatin_CharMap[0].emplace(0x03BB, 0x006B); // Greek Small Letter Lambda to Latin Small Letter k
    mGreekToLatin_CharMap[0].emplace(0x039C, 0x004C); // Greek Capital Letter Mu to Latin Capital Letter L
    mGreekToLatin_CharMap[0].emplace(0x03BC, 0x006C); // Greek Small Letter Mu to Latin Small Letter l
    mGreekToLatin_CharMap[0].emplace(0x039D, 0x004D); // Greek Capital Letter Nu to Latin Capital Letter M
    mGreekToLatin_CharMap[0].emplace(0x03BD, 0x006D); // Greek Small Letter Nu to Latin Small Letter m
    mGreekToLatin_CharMap[0].emplace(0x039E, 0x004E); // Greek Capital Letter Xi to Latin Capital Letter N
    mGreekToLatin_CharMap[0].emplace(0x03BE, 0x006E); // Greek Small Letter Xi to Latin Small Letter n
    mGreekToLatin_CharMap[0].emplace(0x039F, 0x004F); // Greek Capital Letter Omicron to Latin Capital Letter O
    mGreekToLatin_CharMap[0].emplace(0x03BF, 0x006F); // Greek Small Letter Omicron to Latin Small Letter o
    mGreekToLatin_CharMap[0].emplace(0x03A0, 0x0050); // Greek Capital Letter Pi to Latin Capital Letter P
    mGreekToLatin_CharMap[0].emplace(0x03C0, 0x0070); // Greek Small Letter Pi to Latin Small Letter p
    mGreekToLatin_CharMap[0].emplace(0x03A1, 0x0051); // Greek Capital Letter Rho to Latin Capital Letter Q
    mGreekToLatin_CharMap[0].emplace(0x03C1, 0x0071); // Greek Small Letter Rho to Latin Small Letter q
    mGreekToLatin_CharMap[0].emplace(0x03A3, 0x0052); // Greek Capital Letter Sigma to Latin Capital Letter R
    mGreekToLatin_CharMap[0].emplace(0x03C3, 0x0072); // Greek Small Letter Sigma to Latin Small Letter r
    mGreekToLatin_CharMap[0].emplace(0x03A4, 0x0053); // Greek Capital Letter Tau to Latin Capital Letter S
    mGreekToLatin_CharMap[0].emplace(0x03C4, 0x0073); // Greek Small Letter Tau to Latin Small Letter s
    mGreekToLatin_CharMap[0].emplace(0x03A5, 0x0054); // Greek Capital Letter Upsilon to Latin Capital Letter T
    mGreekToLatin_CharMap[0].emplace(0x03C5, 0x0074); // Greek Small Letter Upsilon to Latin Small Letter t
    mGreekToLatin_CharMap[0].emplace(0x03A6, 0x0055); // Greek Capital Letter Phi to Latin Capital Letter U
    mGreekToLatin_CharMap[0].emplace(0x03C6, 0x0075); // Greek Small Letter Phi to Latin Small Letter u
    mGreekToLatin_CharMap[0].emplace(0x03A7, 0x0056); // Greek Capital Letter Chi to Latin Capital Letter V
    mGreekToLatin_CharMap[0].emplace(0x03C7, 0x0076); // Greek Small Letter Chi to Latin Small Letter v
    mGreekToLatin_CharMap[0].emplace(0x03A8, 0x0057); // Greek Capital Letter Psi to Latin Capital Letter W
    mGreekToLatin_CharMap[0].emplace(0x03C8, 0x0077); // Greek Small Letter Psi to Latin Small Letter w
    mGreekToLatin_CharMap[0].emplace(0x03A9, 0x0058); // Greek Capital Letter Omega to Latin Capital Letter X
    mGreekToLatin_CharMap[0].emplace(0x03C9, 0x0078); // Greek Small Letter Omega to Latin Small Letter x
    
    // Populate lengths
    for (const auto &mapping : mGreekToLatin_CharMap[0]) {
        mGreekToLatin_length.emplace(mapping.first, 1);
    }
}

unicode::BitTranslationSets GreekToLatin_BixData::GreekToLatin_1st_BitXorCCs() {
    return unicode::ComputeBitTranslationSets(mGreekToLatin_CharMap[0]);
}

unicode::BitTranslationSets GreekToLatin_BixData::GreekToLatin_2nd_BitCCs() {
    return unicode::ComputeBitTranslationSets(mGreekToLatin_CharMap[1], unicode::XlateMode::LiteralBit);
}

unicode::BitTranslationSets GreekToLatin_BixData::GreekToLatin_3rd_BitCCs() {
    return unicode::ComputeBitTranslationSets(mGreekToLatin_CharMap[2], unicode::XlateMode::LiteralBit);
}

unicode::BitTranslationSets GreekToLatin_BixData::GreekToLatin_4th_BitCCs() {
    return unicode::ComputeBitTranslationSets(mGreekToLatin_CharMap[3], unicode::XlateMode::LiteralBit);
}

class GreekToLatin_Translation : public pablo::PabloKernel {
public:
    GreekToLatin_Translation(KernelBuilder &b, GreekToLatin_BixData &BixData,
                             StreamSet *Basis, StreamSet *Output);
protected:
    void generatePabloMethod() override;
    GreekToLatin_BixData &mBixData;
};

GreekToLatin_Translation::GreekToLatin_Translation(KernelBuilder &b, GreekToLatin_BixData &BixData,
                                                   StreamSet *Basis, StreamSet *Output)
    : PabloKernel(b, "GreekToLatin_Translation" + std::to_string(Basis->getNumElements()) + "x1",
                  // inputs
                  {Binding{"basis", Basis}},
                  // output
                  {Binding{"Output", Output}}), mBixData(BixData) {
}

void GreekToLatin_Translation::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    UTF::UTF_Compiler unicodeCompiler(getInput(0), pb);
    unicode::BitTranslationSets G2L1 = mBixData.GreekToLatin_1st_BitXorCCs();
    unicode::BitTranslationSets G2L2 = mBixData.GreekToLatin_2nd_BitCCs();
    unicode::BitTranslationSets G2L3 = mBixData.GreekToLatin_3rd_BitCCs();
    unicode::BitTranslationSets G2L4 = mBixData.GreekToLatin_4th_BitCCs();
    std::vector<Var *> G2L1_Vars;
    std::vector<Var *> G2L2_Vars;
    std::vector<Var *> G2L3_Vars;
    std::vector<Var *> G2L4_Vars;
    for (unsigned i = 0; i < G2L1.size(); i++) {
        Var *v = pb.createVar("G2L1_bit" + std::to_string(i), pb.createZeroes());
        G2L1_Vars.push_back(v);
        unicodeCompiler.addTarget(v, re::makeCC(G2L1[i], &cc::Unicode));
    }
    for (unsigned i = 0; i < G2L2.size(); i++) {
        Var *v = pb.createVar("G2L2_bit" + std::to_string(i), pb.createZeroes());
        G2L2_Vars.push_back(v);
        unicodeCompiler.addTarget(v, re::makeCC(G2L2[i], &cc::Unicode));
    }
    for (unsigned i = 0; i < G2L3.size(); i++) {
        Var *v = pb.createVar("G2L3_bit" + std::to_string(i), pb.createZeroes());
        G2L3_Vars.push_back v;
        unicodeCompiler.addTarget(v, re::makeCC(G2L3[i], &cc::Unicode));
    }
    for (unsigned i = 0; i < G2L4.size(); i++) {
        Var *v = pb.createVar("G2L4_bit" + std::to_string(i), pb.createZeroes());
        G2L4_Vars.push_back(v);
        unicodeCompiler.addTarget(v, re::makeCC(G2L4[i], &cc::Unicode));
    }
    if (LLVM_UNLIKELY(re::AlgorithmOptionIsSet(re::DisableIfHierarchy))) {
        unicodeCompiler.compile(UTF::UTF_Compiler::IfHierarchy::None);
    } else {
        unicodeCompiler.compile();
    }
    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    Var *outputVar = getOutputStreamVar("Output");
    std::vector<PabloAST *> output_basis(basis.size());
    for (unsigned i = 0; i < basis.size(); i++) {
        if (i < G2L1.size()) {
            output_basis[i] = pb.createXor(basis[i], G2L1_Vars[i]);
        } else {
            output_basis[i] = basis[i];
        }
        if (i < G2L2.size()) {
            output_basis[i] = pb.createOr(pb.createAdvance(G2L2_Vars[i], 1), output_basis[i]);
        }
        if (i < G2L3.size()) {
            output_basis[i] = pb.createOr(pb.createAdvance(G2L3_Vars[i], 2), output_basis[i]);
        }
        if (i < G2L4.size()) {
            output_basis[i] = pb.createOr(pb.createAdvance(G2L4_Vars[i], 3), output_basis[i]);
        }
        pb.createAssign(pb.createExtract(outputVar, pb.getInteger(i)), output_basis[i]);
    }
}

typedef void (*GreekToLatinFunctionType)(int);

GreekToLatinFunctionType generatePipeline(CPUDriver & pxDriver) {
    auto & b = pxDriver.getBuilder();
    auto P = pxDriver.makePipeline({Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});

    Scalar * fileDescriptor = P->getInputScalar("inputFileDescriptor");
    StreamSet * ByteStream = P->CreateStreamSet(1, 8);

    P->CreateKernelCall<SourceKernel>(fileDescriptor, ByteStream);
    SHOW_BYTES(ByteStream);

    StreamSet * BasisBits = P->CreateStreamSet(8, 1);
    P->CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    SHOW_BIXNUM(BasisBits);

    StreamSet * u8index = P->CreateStreamSet(1, 1);
    P->CreateKernelCall<UTF8_index>(BasisBits, u8index);
    SHOW_STREAM(u8index);

    StreamSet * U21_u8indexed = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);

    StreamSet * U21 = P->CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21_u8indexed, U21);
    SHOW_BIXNUM(U21);

    StreamSet * greekToLatinBasis = P->CreateStreamSet(21, 1);
    GreekToLatin_BixData BixData;
    P->CreateKernelCall<GreekToLatin_Translation>(BixData, U21, greekToLatinBasis);
    SHOW_BIXNUM(greekToLatinBasis);

    StreamSet * const OutputBasis = P->CreateStreamSet(8);
    U21_to_UTF8(P, greekToLatinBasis, OutputBasis);
    SHOW_BIXNUM(OutputBasis);

    StreamSet * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<GreekToLatinFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&GreekToLatin_Options, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    CPUDriver driver("GreekToLatin");

    GreekToLatinFunctionType fn = generatePipeline(driver);

    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        fn(fd);
        close(fd);
    }
    return 0;
}