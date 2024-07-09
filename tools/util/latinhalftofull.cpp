
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

//  These declarations are for command line processing.
//  See the LLVM CommandLine 2.0 Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory LatinHalfToFullOptions("latinhalftofull Options", "latinhalftofull control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(LatinHalfToFullOptions));

class FullWidthIfy : public pablo::PabloKernel {
public:
    FullWidthIfy(KernelBuilder & b, StreamSet * halfwidths, StreamSet * U21, StreamSet * fullWidthBasis)
    : pablo::PabloKernel(b, "FullWidthIfy",
                         {Binding{"halfwidths", halfwidths}, Binding{"U21", U21}},
                      {Binding{"fullWidthBasis", fullWidthBasis}}) {}
protected:
    void generatePabloMethod() override;
};

void FullWidthIfy::generatePabloMethod() {
    //  pb is an object used for build Pablo language statements
    pablo::PabloBuilder pb(getEntryScope());

    //  bnc is an object that can perform arithmetic on sets of parallel bit streams
    BixNumCompiler bnc(pb);

    // Get the input stream sets.
    PabloAST * halfwidths = getInputStreamSet("halfwidths")[0];
    std::vector<PabloAST *> U21 = getInputStreamSet("U21");

    // ccc is an object that can compile character classes from a set of 8 parallel bit streams.
    cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), U21);

    std::vector<PabloAST *> fullWidthBasis(8);

    /*
    std::vector<PabloAST *> basisVar(21);
    for (unsigned i = 0; i < 21; i++) {
        basisVar[i] = U21[i];//pb.createVar("basisVar" + std::to_string(i), U21[i]);
    }*/

    /*BixNum VPart = bnc.ZeroExtend(V_index, 21);
    VPart = bnc.AddModular(VPart, Hangul_VBase);

    
    BixNum TPart = bnc.ZeroExtend(T_index, 21);
    TPart = bnc.AddModular(TPart, Hangul_TBase);
    for (unsigned i = 0; i < 21; i++) {
        PabloAST * bit = nested.createSel(Hangul_L, LPart[i], basis[i]);
        bit = nested.createSel(V_position, VPart[i], bit);
        bit = nested.createSel(T_position, TPart[i], bit);
        nested.createAssign(basisVar[i], bit);
    }*/

   BixNum basisVar = bnc.AddModular(U21, 0xFEE0);

    Var * fullWidthBasisVar = getOutputStreamVar("fullWidthBasis");
    for (unsigned i = 0; i < 21; i++) {

        //basisVar[i] = pb.createAdd(basisVar[i], halfwidths);
        
        //basisVar[i] = pb.createSel(halfwidths, lo1[i], lo[i]);
        //pb.createIf(halfwidths);
        //pb.getPabloBlock()
        
        pb.createAssign(pb.createExtract(fullWidthBasisVar, pb.getInteger(i)), pb.createSel(halfwidths, basisVar[i], U21[i]));
    }
}


typedef void (*HalfToFullFunctionType)(uint32_t fd);

HalfToFullFunctionType generatePipeline(CPUDriver & pxDriver) {
    // A Parabix program is build as a set of kernel calls called a pipeline.
    // A pipeline is construction using a Parabix driver object.
    auto & b = pxDriver.getBuilder();
    auto P = pxDriver.makePipeline({Binding{b.getInt32Ty(), "inputFileDecriptor"}}, {});

    //  The program will use a file descriptor as an input.
    Scalar * fileDescriptor = P->getInputScalar("inputFileDecriptor");
    StreamSet * ByteStream = P->CreateStreamSet(1, 8);

    //  ReadSourceKernel is a Parabix Kernel that produces a stream of bytes
    //  from a file descriptor.
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

    UCD::codepoint_t low_cp = 0x0021;
    UCD::codepoint_t hi_cp = low_cp + 105;

    StreamSet * halfwidths = P->CreateStreamSet(1);
    std::vector<re::CC *> halfwidths_CC = {re::makeCC(low_cp, hi_cp, &cc::Unicode)};
    P->CreateKernelCall<CharacterClassKernelBuilder>(halfwidths_CC, U21, halfwidths);
    SHOW_STREAM(halfwidths);

    // Perform the logic of the Hexify kernel.
    StreamSet * fullWidthBasis = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<FullWidthIfy>(halfwidths, U21, fullWidthBasis);
    SHOW_BIXNUM(fullWidthBasis);

    //  The StdOut kernel writes a byte stream to standard output.
    P->CreateKernelCall<StdOutKernel>(fullWidthBasis);
    return reinterpret_cast<HalfToFullFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&LatinHalfToFullOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});
    //  A CPU driver is capable of compiling and running Parabix programs on the CPU.
    CPUDriver driver("latinhalftofull");
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
