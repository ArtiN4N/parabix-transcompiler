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
#include <unordered_map>

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace pablo;

std::unordered_map<UCD::codepoint_t, UCD::codepoint_t> halfwidthToFullwidthHangulMap = {
    {0xFFA1, 0x1100}, //ㄱ
    {0xFFA2, 0x1101}, //ㄲ
    {0xFFA4, 0x1102}, //ㄴ
    {0xFFA7, 0x1103}, //ㄷ
    {0xFFA8, 0x1104}, //ㄸ
    {0xFFA9, 0x1105}, //ㄹ
    {0xFFB1, 0x1106}, //ㅁ
    {0xFFB2, 0x1107}, //ㅂ
    {0xFFB3, 0x1108}, //ㅃ
    {0xFFB5, 0xFFB5}, //ㅅ
    {0xFFB6, 0x110A}, //ㅆ
    {0xFFB7, 0x110B}, //ㅇ
    {0xFFB8, 0x110C}, //ㅈ
    {0xFFB9, 0x110D}, //ㅉ
    {0xFFBA, 0x110E}, //ㅊ
    {0xFFBB, 0x110F}, //ㅋ
    {0xFFBC, 0x1110}, //ㅌ
    {0xFFBD, 0x1111}, //ㅍ
    {0xFFBE, 0x1112}, //ㅎ
    {0xFFC2, 0x1161}, //ㅏ
    {0xFFC3, 0x1162}, //ㅐ
    {0xFFC4, 0x1163}, //ㅑ
    {0xFFC5, 0x1164}, //ㅒ
    {0xFFC6, 0x1165}, //ㅓ
    {0xFFC7, 0x1166}, //ㅔ
    {0xFFCA, 0x1167}, //ㅕ
    {0xFFCB, 0x1168}, //ㅖ
    {0xFFCC, 0x1169}, //ㅗ
    {0xFFCD, 0x116A}, //ㅘ
    {0xFFCE, 0x116B}, //ㅙ
    {0xFFCF, 0x116C}, //ㅚ
    {0xFFD2, 0x116D}, //ㅛ
    {0xFFD3, 0x116E}, //ㅜ
    {0xFFD4, 0x116F}, //ㅝ
    {0xFFD5, 0x1170}, //ㅞ
    {0xFFD6, 0x1171}, //ㅟ
    {0xFFD7, 0x1172}, //ㅠ
    {0xFFDA, 0x1173}, //ㅡ
    {0xFFDB, 0x1174}, //ㅢ
    {0xFFDC, 0x1175}, //ㅣ
};


static cl::OptionCategory HangulHalfToFullOptions("hangulhalftofull Options", "hangulhalftofull control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(HangulHalfToFullOptions));

class FullWidthIfy : public pablo::PabloKernel {
public:
    FullWidthIfy(KernelBuilder & b, StreamSet * U21, StreamSet * fullWidthBasis)
    : pablo::PabloKernel(b, "FullWidthIfy",
                         {Binding{"U21", U21}},
                      {Binding{"fullWidthBasis", fullWidthBasis}}) {}
protected:
    void generatePabloMethod() override;
};

void FullWidthIfy::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> U21 = getInputStreamSet("U21");
    cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), U21);

    std::vector<PabloAST *> fullwidthStreams(21, nullptr);
    for (unsigned i = 0; i < 21; i++) {
        fullwidthStreams[i] = pb.createZeroes();
    }

    for (auto &entry : halfwidthToFullwidthHangulMap) {
        UCD::codepoint_t halfwidthCodepoint = entry.first;
        UCD::codepoint_t fullwidthCodepoint = entry.second;
        PabloAST * halfwidthPattern = ccc.compileCC(re::makeCC(halfwidthCodepoint, halfwidthCodepoint, &cc::Unicode));

        for (unsigned i = 0; i < 21; i++) {
            PabloAST * fullwidthBitPattern = ccc.compileCC(re::makeCC((fullwidthCodepoint >> i) & 1, (fullwidthCodepoint >> i) & 1, &cc::Unicode));
            PabloAST * bit = pb.createAnd(halfwidthPattern, fullwidthBitPattern);
            fullwidthStreams[i] = pb.createOr(fullwidthStreams[i], bit);
        }
    }

    Var * fullWidthBasisVar = getOutputStreamVar("fullWidthBasis");
    for (unsigned i = 0; i < 21; i++) {
        pb.createAssign(pb.createExtract(fullWidthBasisVar, pb.getInteger(i)), fullwidthStreams[i]);
    }
}


typedef void (*HalfToFullFunctionType)(uint32_t fd);

HalfToFullFunctionType generatePipeline(CPUDriver & pxDriver) {
    auto & b = pxDriver.getBuilder();
    auto P = pxDriver.makePipeline({Binding{b.getInt32Ty(), "inputFileDecriptor"}}, {});

    Scalar * fileDescriptor = P->getInputScalar("inputFileDecriptor");
    StreamSet * ByteStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
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

    StreamSet * fullWidthBasis = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<FullWidthIfy>(U21, fullWidthBasis);
    SHOW_BIXNUM(fullWidthBasis);

    StreamSet * const OutputBasis = P->CreateStreamSet(8);
    U21_to_UTF8(P, fullWidthBasis, OutputBasis);
    
    SHOW_BIXNUM(OutputBasis);

    StreamSet * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<HalfToFullFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&HangulHalfToFullOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    //  A CPU driver is capable of compiling and running Parabix programs on the CPU.
    CPUDriver driver("hangulhalftofull");

    //  Build and compile the Parabix pipeline by calling the Pipeline function above.
    HalfToFullFunctionType fn = generatePipeline(driver);
    
    //  The compile function "fn"  can now be used.   It takes a file
    //  descriptor as an input, which is specified by the filename given by
    //  the inputFile command line option.
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        //  Run the pipeline.
        fn(fd);
        close(fd);
    }
    return 0;
}