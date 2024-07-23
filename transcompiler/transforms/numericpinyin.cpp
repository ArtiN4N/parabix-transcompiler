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
#include <pablo/bixnum/bixnum.h>

#include <grep/grep_kernel.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

#include <re/toolchain/toolchain.h>
#include <re/transforms/exclude_CC.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <re/cc/cc_compiler_target.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_kernel.h>

#include <kernel/core/idisa_target.h>
#include <boost/filesystem.hpp>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/transforms/re_simplifier.h>
#include <re/adt/adt.h>
#include <re/parse/parser.h>
#include <re/unicode/resolve_properties.h>
#include <re/cc/cc_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/core/streamset.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/streamutils/stream_select.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/pablo_toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <grep/grep_kernel.h>
#include <toolchain/toolchain.h>
#include <fileselect/file_select.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <map>


#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace pablo;

//  These declarations are for command line processing.
//  See the LLVM CommandLine 2.0 Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory numericPinyinOptions("numericPinyin Options", "numericPinyin control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(numericPinyinOptions));

class numericPinyinify : public pablo::PabloKernel {
public:
    numericPinyinify(KernelBuilder & b, StreamSet * U21, StreamSet * u32Basis)
    : pablo::PabloKernel(b, "numericPinyinify",
                        {Binding{"U21", U21}},
                            {Binding{"u32Basis", u32Basis}}) {}
protected:
    void generatePabloMethod() override;
};

void numericPinyinify::generatePabloMethod() {
    //  pb is an object used for build Pablo language statements
    pablo::PabloBuilder pb(getEntryScope());

    BixNumCompiler bnc(pb);

    // Get the input stream sets.
    std::vector<PabloAST *> U21 = getInputStreamSet("U21");
    cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), U21);

    //std::vector<PabloAST *> translationBasis = getInputStreamSet("translationBasis");
    std::vector<PabloAST *> transformed(U21.size());

    //std::cout << U21.size() << std::endl;

    // Step 0 - create set of pinyin tones
    // thank you http://ktmatu.com/info/hanyu-pinyin-characters/unicode-character-set.utf8.html
    //UCD::UnicodeSet pinyinTonesSet;
    //int pinyinCodes[47] = {0xC0,0xC1,0xC8,0xC9,0xCC,0xCD,0xD2,0xD3,0xD9,0xDA,0xE0,0xE1,0xE8,0xE9,0xEC,0xED,0xF2,0xF3,0xF9,0xFA,0x100,0x112,0x113,0x11A,0x11B,0x12A,0x12B,0x14C,0x14D,0x16A,0x16B,0x16D,0x1CD,0x1CE,0x1CF,0x1D0,0x1D1,0x1D2,0x1D3,0x1D5,0x1D6,0x1D7,0x1D8,0x1D9,0x1DA,0x1DB,0x1DC};
    //for (int i = 0; i < 47; i++) pinyinTonesSet.insert(pinyinCodes[i]);
    //PabloAST * pinyinTones = ccc.compileCC(re::makeCC(pinyinTonesSet));

    // Create a character class from the whitespace property set
    //UCD::PropertyObject * whiteSpacesProperty = UCD::get_WSPACE_PropertyObject();
    //UCD::UnicodeSet wSpaceSet = whiteSpacesProperty->GetCodepointSet("");
    //PabloAST * whiteSpaces = ccc.compileCC(re::makeCC(wSpaceSet));

    // Find all characters after a whitespace
    //PabloAST * afterWhiteSpaces = pb.createNot(pb.createAdvance(pb.createNot(whiteSpaces), 1));

    //PabloAST * pinyinCount = pb.createCount(pb.createMatchStar(U21[0], afterWhiteSpaces));
    //pb.createDebugPrint(pinyinCount);
    
    //for (unsigned i = 1; i < U21.size(); i++) {
        //pinyinCount = pb.createAdd(pinyinCount, pb.createCount(pb.createMatchStar(U21[i], afterWhiteSpaces)));
    //}   
    
    //pb.createDebugPrint(pinyinCount);
    

    // Step 1 - count the number of tones, and add space for that many characters
    // Step 2 - step through transformed, and set the whole thing equal to the lastest tone numeric
    // Step 3 - assign u21 to output, except for when ther numeric spot is matched (character class ORs ?)

    Var * outputBasisVar = getOutputStreamVar("u32Basis");

    // For each bit of the input stream
    for (unsigned i = 0; i < U21.size(); i++) {
        //// If the translation set covers said bit
        //if (i < translationBasis.size()) // XOR the input bit with the transformation bit  
           //transformed[i] = pb.createXor(translationBasis[i], U21[i]);
        //else transformed[i] = U21[i];

        pb.createAssign(pb.createExtract(outputBasisVar, pb.getInteger(i)), U21[i]);
    }
}


typedef void (*TonumericPinyinFunctionType)(uint32_t fd);

TonumericPinyinFunctionType generatePipeline(CPUDriver & pxDriver) {
    std::string pinyinCharClassesText[4] = {
        "[uāi,uái,uǎi,uài,uāng,uáng,uǎng,uàng,iāo,iáo,iǎo,iào,iāng,iáng,iǎng,iàng,ōu,óu,ǒu,òu,uō,uó,uǒ,uò,iōng,ióng,iǒng,iòng,ēi,éi,ěi,èi,ēng,éng,ěng,èng,ēr,ér,ěr,èr,iē,ié,iě,iè,uē,ué,uě,uè,uī,uí,uǐ,uì,īng,íng,ǐng,ìng,ūn,ún,ŭn,ùn,īu,íu,ǐu,ìu,ǖn,ǘn,ǚn,ǜn]",
        "[āi,ái,ǎi,ài,āo,áo,ǎo,ào,āng,áng,ǎng,àng,uān,uán,uǎn,uàn,iān,ián,iǎn,iàn,ōng,óng,ǒng,òng,ēn,én,ěn,èn,īn,ín,ǐn,ìn]",
        "[ān,án,ǎn,àn,uā,uá,uǎ,uà,iā,iá,iǎ,ià,ō,ó,ǒ,ò,ē,é,ě,è]",
        "[ā,á,ǎ,à,ī,í,ǐ,ì,ū,ú,ŭ,ù,ǖ,ǘ,ǚ,ǜ]"
    };

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

    re::CC* pinyinCharClasses[4] = {};
    // mane what da hell
    pinyinCharClasses[0] = dyn_cast<re::CC>(UCD::externalizeProperties(UCD::linkAndResolve(re::simplifyRE(re::RE_Parser::parse(pinyinCharClassesText[0], re::ModeFlagType::CASE_INSENSITIVE_MODE_FLAG)))));
    pinyinCharClasses[1] = dyn_cast<re::CC>(re::exclude_CC(UCD::externalizeProperties(UCD::linkAndResolve(re::simplifyRE(re::RE_Parser::parse(pinyinCharClassesText[1], re::ModeFlagType::CASE_INSENSITIVE_MODE_FLAG)))), pinyinCharClasses[0]));
    pinyinCharClasses[2] = dyn_cast<re::CC>(re::exclude_CC(re::exclude_CC(UCD::externalizeProperties(UCD::linkAndResolve(re::simplifyRE(re::RE_Parser::parse(pinyinCharClassesText[2], re::ModeFlagType::CASE_INSENSITIVE_MODE_FLAG)))), pinyinCharClasses[0]), pinyinCharClasses[1]));
    pinyinCharClasses[3] = dyn_cast<re::CC>(re::exclude_CC(re::exclude_CC(re::exclude_CC(UCD::externalizeProperties(UCD::linkAndResolve(re::simplifyRE(re::RE_Parser::parse(pinyinCharClassesText[3], re::ModeFlagType::CASE_INSENSITIVE_MODE_FLAG)))), pinyinCharClasses[0]), pinyinCharClasses[1]), pinyinCharClasses[2]));


    StreamSet * inPinyinLabel1 = P->CreateStreamSet(1);
    std::vector<re::CC *> inPinyinLabel1_CC = {pinyinCharClasses[0]};
    P->CreateKernelCall<CharacterClassKernelBuilder>(inPinyinLabel1_CC, U21, inPinyinLabel1);
    SHOW_BIXNUM(inPinyinLabel1);


    // Perform the logic of the numericPinyinify kernel on the codepoiont values.
    StreamSet * u32Basis = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<numericPinyinify>(U21, u32Basis);
    SHOW_BIXNUM(u32Basis);

    // Convert back to UTF8 from codepoints.
    StreamSet * const OutputBasis = P->CreateStreamSet(8);
    U21_to_UTF8(P, u32Basis, OutputBasis);

    SHOW_BIXNUM(OutputBasis);

    StreamSet * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<TonumericPinyinFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&numericPinyinOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    //  A CPU driver is capable of compiling and running Parabix programs on the CPU.
    CPUDriver driver("tonumericPinyin");

    //  Build and compile the Parabix pipeline by calling the Pipeline function above.
    TonumericPinyinFunctionType fn = generatePipeline(driver);
    
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