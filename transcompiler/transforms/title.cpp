#include <vector>
#include <fcntl.h>
#include <string>
#include <iostream>

#include <unicode/data/PropertyObjects.h>
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
static cl::OptionCategory TitleOptions("title Options", "title control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(TitleOptions));

class Titleify : public pablo::PabloKernel {
public:
    Titleify(KernelBuilder & b, StreamSet * U21, StreamSet * translationBasis, StreamSet * u32Basis)
    : pablo::PabloKernel(b, "Titleify",
                        {Binding{"U21", U21}, Binding{"translationBasis", translationBasis}},
                            {Binding{"u32Basis", u32Basis}}) {}
protected:
    void generatePabloMethod() override;
};

void Titleify::generatePabloMethod() {
    //  pb is an object used for build Pablo language statements
    pablo::PabloBuilder pb(getEntryScope());

    // Get the input stream sets.
    std::vector<PabloAST *> U21 = getInputStreamSet("U21");
    

    //cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), U21);

    std::vector<PabloAST *> translationBasis = getInputStreamSet("translationBasis");
    std::vector<PabloAST *> transformed(U21.size());

    //PabloAST * beforeTitleElig = getInputStreamSet("beforeTitleElig")[0];
    cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), U21);

    // We use regex to find the first charatcer, and every character after a space
    // These characters are valid for title case, and the rest will become lowercase
    // "^" is the start of a line, "\\s" is any whitespace character, "." is any char except for a newline

    std::cout << "doing regex" << std::endl;
    // "(^|\\s)(.)\\X"
    re::RE * CC_re = re::simplifyRE(re::RE_Parser::parse("\\s"));
    std::cout << "\\s" << std::endl;
    std::cout << "doing link" << std::endl;
    CC_re = UCD::linkAndResolve(CC_re);
    std::cout << "doing externalize" << std::endl;
    CC_re = UCD::externalizeProperties(CC_re);

    std::cout << "doing recast" << std::endl;
    re::CC * titlePositions_CC = dyn_cast<re::CC>(CC_re);
    std::cout << "compiling regex" << std::endl;

    PabloAST * re = ccc.compileCC(titlePositions_CC);
    PabloAST * regex = pb.createLookahead(re, 1)
    

    std::cout << "compiled regex" << std::endl;

    Var * outputBasisVar = getOutputStreamVar("u32Basis");

    std::cout << "doing index 0" << std::endl;

    // Since beforeTitleElig marks the characters before title eligible characters, we need to shift everything
    // As well, the first character is title eligible
    if (0 < translationBasis.size())
        transformed[0] = pb.createXor(translationBasis[0], U21[0]);
    else transformed[0] = U21[0];
    pb.createAssign(pb.createExtract(outputBasisVar, pb.getInteger(0)), transformed[0]);

    // For each bit of the input stream
    for (unsigned i = 1; i < U21.size() - 1; i++) {
        std::cout << "assigning beforeTitleElig: " << i << std::endl;
        

        // If the translation set covers said bit
        if (i < translationBasis.size()) // XOR the input bit with the transformation bit  
            transformed[i] = pb.createXor(translationBasis[i], U21[i]);
        else transformed[i] = U21[i];

        std::cout << "assigning output" << std::endl;
        // Only select transformed characters when they are title eligible
        pb.createAssign(pb.createExtract(outputBasisVar, pb.getInteger(i)), pb.createSel(regex, transformed[i], U21[i]));
    }

    std::cout << "doing index final" << std::endl;

    if (U21.size() - 1 < translationBasis.size())
        transformed[U21.size() - 1] = pb.createXor(translationBasis[U21.size() - 1], U21[U21.size() - 1]);
    else transformed[U21.size() - 1] = U21[U21.size() - 1];
    pb.createAssign(pb.createExtract(outputBasisVar, pb.getInteger(U21.size() - 1)), transformed[U21.size() - 1]);
}


typedef void (*ToTitleFunctionType)(uint32_t fd);

ToTitleFunctionType generatePipeline(CPUDriver & pxDriver, unicode::BitTranslationSets titleTranslationSet) {
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

    // Turn the title translation set into a vector of character classes
    std::vector<re::CC *> titleTranslation_ccs;
    for (auto & b : titleTranslationSet) {
        titleTranslation_ccs.push_back(re::makeCC(b, &cc::Unicode));
    }

    StreamSet * translationBasis = P->CreateStreamSet(titleTranslation_ccs.size());
    P->CreateKernelCall<CharClassesKernel>(titleTranslation_ccs, U21, translationBasis);
    SHOW_BIXNUM(translationBasis);

    std::cout << "creating elig streamset" << std::endl;
    //  We need to know which characters are title eligible
    // Characters are title eligible if they come after a space
    //StreamSet * beforeTitleElig = P->CreateStreamSet(1);
    
    //std::vector<re::CC *> beforeTitleElig_CC = {re::makeCC(0x0020, &cc::Unicode)};
    std::cout << "creating titlepos ccs" << std::endl;
    //std::vector<re::CC *> titlePositions_ccs = {titlePositions_CC};
    std::cout << "building elig streamset" << std::endl;
    //P->CreateKernelCall<CharacterClassKernelBuilder>(titlePositions_ccs, U21, beforeTitleElig);
    std::cout << "segflt?" << std::endl;
    //SHOW_STREAM(beforeTitleElig);

    // Perform the logic of the Titleify kernel on the codepoiont values.
    StreamSet * u32Basis = P->CreateStreamSet(21, 1);
    std::cout << "passing elig streamset" << std::endl;
    P->CreateKernelCall<Titleify>(U21, translationBasis, u32Basis);
    SHOW_BIXNUM(u32Basis);

    // Convert back to UTF8 from codepoints.
    StreamSet * const OutputBasis = P->CreateStreamSet(8);
    U21_to_UTF8(P, u32Basis, OutputBasis);

    SHOW_BIXNUM(OutputBasis);

    StreamSet * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<ToTitleFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&TitleOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    //  A CPU driver is capable of compiling and running Parabix programs on the CPU.
    CPUDriver driver("toTitle");

    // Get the titlecase mapping object, can create a translation set from that
    UCD::CodePointPropertyObject* titlePropertyObject = dyn_cast<UCD::CodePointPropertyObject>(UCD::get_SUC_PropertyObject());

    unicode::BitTranslationSets titleTranslationSet;

    titleTranslationSet = titlePropertyObject->GetBitTransformSets();

    
    ToTitleFunctionType fn = generatePipeline(driver, titleTranslationSet);
    
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