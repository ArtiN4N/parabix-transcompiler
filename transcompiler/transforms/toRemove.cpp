#include <vector>
#include <fcntl.h>
#include <string>
#include <iostream>

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyAliases.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>


#include <kernel/streamutils/deletion.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>

#include <pablo/codegenstate.h>

#include <grep/grep_kernel.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

#include <re/transforms/re_simplifier.h>
#include <re/toolchain/toolchain.h>
#include <re/unicode/resolve_properties.h>
#include <re/parse/parser.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <re/cc/cc_compiler_target.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_kernel.h>


#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace pablo;

//  These declarations are for command line processing.
//  See the LLVM CommandLine 2.0 Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory RemoveOptions(" e Options", "remove control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(RemoveOptions));

class Removeify : public pablo::PabloKernel {
public:
    Removeify(KernelBuilder & b, StreamSet * U21, StreamSet * removeBasis, StreamSet * lowerBasis, StreamSet * u32Basis)
    : pablo::PabloKernel(b, "Removeify",
                        {Binding{"U21", U21}, Binding{"removeBasis", removeBasis}, Binding{"lowerBasis", lowerBasis}},
                            {Binding{"u32Basis", u32Basis}}) {}
protected:
    void generatePabloMethod() override;
};

void Removeify::generatePabloMethod() {
    //  pb is an object used for build Pablo language statements
    //pablo::PabloBuilder pb(getEntryScope());

    // Get the input stream sets.
    //std::vector<PabloAST *> U21 = getInputStreamSet("U21");
    //cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), U21);

    //std::vector<PabloAST *> removeBasis = getInputStreamSet("removeBasis");
    //std::vector<PabloAST *> lowerBasis = getInputStreamSet("lowerBasis");

    //std::vector<PabloAST *> transformedRemove(U21.size());
    //std::vector<PabloAST *> transformedLower(U21.size());

    //Var * outputBasisVar = getOutputStreamVar("u32Basis");


    
    
    // Find all characters after a whitespace
    //PabloAST * afterWhiteSpaces = pb.createNot(pb.createAdvance(pb.createNot(whiteSpaces), 1));

    //for (unsigned i = 0; i < U21.size(); i++) {
        
        // If the translation set covers said bit, XOR the input bit with the transformation bit
        //if (i < removeBasis.size())
            //transformedRemove[i] = pb.createXor(removeBasis[i], U21[i]);
        //else transformedRemove[i] = U21[i];

        //if (i < lowerBasis.size())
            //transformedLower[i] = pb.createXor(lowerBasis[i], U21[i]);
        //else transformedLower[i] = U21[i];

        // Convert to remove case after whitespaces, otherwise, lowercase
        //pb.createAssign(pb.createExtract(outputBasisVar, pb.getInteger(i)), pb.createSel(afterWhiteSpaces, transformedRemove[i], transformedLower[i]));
    //}
}


typedef void (*ToRemoveFunctionType)(uint32_t fd);

ToRemoveFunctionType generatePipeline(CPUDriver & pxDriver) {
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

    // Get the basis bits
    StreamSet * BasisBits = P->CreateStreamSet(8, 1);
    P->CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    SHOW_BIXNUM(BasisBits);

    // Convert into codepoints
    StreamSet * u8index = P->CreateStreamSet(1, 1);
    P->CreateKernelCall<UTF8_index>(BasisBits, u8index);
    SHOW_STREAM(u8index);

    StreamSet * U21_u8indexed = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);

    StreamSet * U21 = P->CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21_u8indexed, U21);
    SHOW_BIXNUM(U21);

    std::string toRemoveStr = "[abcdef!]";
    re::RE * toRemoveRegex = re::simplifyRE(re::RE_Parser::parse(toRemoveStr));
    toRemoveRegex = UCD::linkAndResolve(toRemoveRegex);
    toRemoveRegex = UCD::externalizeProperties(toRemoveRegex);
    re::CC * toRemoveClass = dyn_cast<re::CC>(toRemoveRegex);

    StreamSet * toRemoveMarker = P->CreateStreamSet(1);
    std::vector<re::CC *> toRemoveMarker_CC = {toRemoveClass};
    P->CreateKernelCall<CharacterClassKernelBuilder>(toRemoveMarker_CC, U21, toRemoveMarker);
    SHOW_STREAM(toRemoveMarker);

    StreamSet * toKeepMarker = P->CreateStreamSet(1);
    P->CreateKernelCall<CharacterClassKernelBuilder>(toRemoveMarker, toKeepMarker);
    SHOW_STREAM(toKeepMarker);

    StreamSet * removeBasis = P->CreateStreamSet(21, 1);
    FilterByMask(P, toKeepMarker, U21, removeBasis);
    //P->CreateKernelCall<DeletionKernel>(U21, toRemoveMarker, removeBasis, )

    // Convert back to UTF8 from codepoints.
    StreamSet * const OutputBasis = P->CreateStreamSet(8);
    U21_to_UTF8(P, removeBasis, OutputBasis);

    SHOW_BIXNUM(OutputBasis);

    StreamSet * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<ToRemoveFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&RemoveOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    //  A CPU driver is capable of compiling and running Parabix programs on the CPU.
    CPUDriver driver("toRemove");

    ToRemoveFunctionType fn = generatePipeline(driver);
    
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