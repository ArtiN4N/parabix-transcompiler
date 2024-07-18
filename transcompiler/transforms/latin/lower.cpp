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
#include <unicode/data/CaseFolding.h>
#include <unicode/data/Equivalence.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>
#include <codecvt>
#include <re/toolchain/toolchain.h>

#include <iostream>


#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace pablo;

//  These declarations are for command line processing.
//  See the LLVM CommandLine 2.0 Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory LowerOptions("lower Options", "lower control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(LowerOptions));

class Lowerify : public pablo::PabloKernel {
public:
    Lowerify(KernelBuilder & b, StreamSet * U21, StreamSet * outputBasis)
    : pablo::PabloKernel(b, "Lowerify",
                         {Binding{"U21", U21}},
                      {Binding{"outputBasis", outputBasis}}) {}
protected:
    void generatePabloMethod() override;
};

void Lowerify::generatePabloMethod() {
    //  pb is an object used for build Pablo language statements
    pablo::PabloBuilder pb(getEntryScope());

    //  bnc is an object that can perform arithmetic on sets of parallel bit streams
    BixNumCompiler bnc(pb);

    // Get the input stream sets.
    //PabloAST * halfwidths = getInputStreamSet("halfwidths")[0];
    std::vector<PabloAST *> U21 = getInputStreamSet("U21");

    // ccc is an object that can compile character classes from a set of 8 parallel bit streams.
    cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), U21);

    //CodePointPropertyObject(UCD::property_t p, const UnicodeSet && nullSet, const UnicodeSet && mapsToSelf,
    //const std::unordered_map<UCD::codepoint_t, UCD::codepoint_t> && explicit_map)
    
    // To ask:
    // what are valid inpouts for "getcodepointset"
    // how can i use this to map upper property to lower property, etc.
    // how can i use the unicode set iterator

    // my own dictionary:
    // UCD::PropertyObject * --> is a property object, see PropertyObjects.h
    // UCD::UnicodeSet --> is a set of unicode codepoints
    // UCD::PropertyObject*->GetCodepointSet(string) --> if empty string, gets the codepoint set of the objects property
    // UCD::property_t --> is the property
    // UCD::PropertyObject*->GetPropertyIntersection(UCD::PropertyObject*) --> finds the intersection set between two properties
    // UCD::UnicodeSet.at(int) --> gets the codepoint at index int

    
    UCD::PropertyObject * lowerObject = UCD::get_LOWER_PropertyObject();
    UCD::UnicodeSet lSet = lowerObject->GetCodepointSet("");

    // ok wait this crazy  
    UCD::codepoint_t theCodePoint = 0x41;
    UCD::UnicodeSet equivalentCaselessCharacter = UCD::equivalentCodepoints(theCodePoint, UCD::EquivalenceOptions::Caseless);
    UCD::UnicodeSet lowerCaseCharacter = equivalentCaselessCharacter & lSet;
    

    //UCD::property_t upperProperty = upperObject->getPropertyCode();
    //UCD::UnicodeSet lInterUSet = lowerObject->GetCodepointSet(UCD::getPropertyEnumName(upperProperty));
    

    BixNum basisVar;

    Var * outputBasisVar = getOutputStreamVar("outputBasis");
    for (unsigned i = 0; i < 21; i++) {
        pb.createAssign(pb.createExtract(outputBasisVar, pb.getInteger(i)), U21[i]);
    }
}


typedef void (*ToLowerFunctionType)(uint32_t fd);

ToLowerFunctionType generatePipeline(CPUDriver & pxDriver) {
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

    // Perform the logic of the Lowerify kernel on the codepoiont values.
    StreamSet * outputBasis = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<Lowerify>(U21, outputBasis);
    SHOW_BIXNUM(outputBasis);

    // Convert back to UTF8 from codepoints.
    StreamSet * const OutputBasis = P->CreateStreamSet(8);
    U21_to_UTF8(P, outputBasis, OutputBasis);

    SHOW_BIXNUM(OutputBasis);

    StreamSet * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<ToLowerFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&LowerOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    //  A CPU driver is capable of compiling and running Parabix programs on the CPU.
    CPUDriver driver("tolower");

    //  Build and compile the Parabix pipeline by calling the Pipeline function above.
    ToLowerFunctionType fn = generatePipeline(driver);
    
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