#include <vector>
#include <fcntl.h>
#include <string>
#include <iostream>

#include "lascii.h"

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>

#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/string_insert.h>
#include <kernel/streamutils/pdep_kernel.h>
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
#include <pablo/pe_zeroes.h>        // for Zeroes
#include <pablo/bixnum/bixnum.h>

#include <grep/grep_kernel.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

#include <re/toolchain/toolchain.h>
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

struct NONASCII_bixData {
    NONASCII_bixData();
    std::vector<re::CC *> insertionBixNumCCs();
    unicode::BitTranslationSets matchBitXorCCs(unsigned);
    unicode::BitTranslationSets matchBitCCs(unsigned);
    unsigned bitsNeeded;
    unsigned maxAdd;
private:
    std::vector<std::pair<UCD::codepoint_t, std::vector<UCD::codepoint_t>>> mUnicodeMap;
    std::unordered_map<codepoint_t, unsigned> mInsertLength;
    unicode::TranslationMap mCharMap[5];
};

NONASCII_bixData::NONASCII_bixData() {
    mUnicodeMap = asciiCodeData;

    maxAdd = 0;
    for (auto& pair : mUnicodeMap) {
        mInsertLength.emplace(pair.first, pair.second.size());
        if (pair.second.size() > maxAdd) {
            maxAdd++;
        }

        unsigned int i = 0;
        for (auto& target : pair.second) {
            mCharMap[i].emplace(pair.first, target);
            i++;
        }
    }

    unsigned n = maxAdd;

    bitsNeeded = 0;
    while (n) {
        bitsNeeded++;
        n >>= 1;
    }
}

std::vector<re::CC *> NONASCII_bixData::insertionBixNumCCs() {
    unicode::BitTranslationSets BixNumCCs;

    for (unsigned i = 0; i < bitsNeeded; i++) {
        BixNumCCs.push_back(UCD::UnicodeSet());
    }

    for (auto& p : mInsertLength) {
        auto insert_amt = p.second - 1;

        unsigned bitAmt = 1;
        for (unsigned i = 0; i < bitsNeeded; i++) {
            if ((insert_amt & bitAmt) == bitAmt) {
                BixNumCCs[i].insert(p.first);
            }
            bitAmt <<= 1;
        }
    }

    std::vector<re::CC *> ret;
    for (unsigned i = 0; i < bitsNeeded; i++) {
        ret.push_back(re::makeCC(BixNumCCs[i], &cc::Unicode));
    }
    

    return ret;
}

unicode::BitTranslationSets NONASCII_bixData::matchBitXorCCs(unsigned i) {
    return unicode::ComputeBitTranslationSets(mCharMap[i]);
}

unicode::BitTranslationSets NONASCII_bixData::matchBitCCs(unsigned i) {
    return unicode::ComputeBitTranslationSets(mCharMap[i], unicode::XlateMode::LiteralBit);
}

//  These declarations are for command line processing.
//  See the LLVM CommandLine 2.0 Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory LasciiOptions("lascii Options", "lascii control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(LasciiOptions));

class Lasciify : public pablo::PabloKernel {
public:
    Lasciify(KernelBuilder & b, NONASCII_bixData & BixData, StreamSet * Basis, StreamSet * Output);
protected:
    void generatePabloMethod() override;
    NONASCII_bixData & mBixData;
};

Lasciify::Lasciify (KernelBuilder & b, NONASCII_bixData & BixData, StreamSet * Basis, StreamSet * Output)
: PabloKernel(b, "Lasciify" + std::to_string(Basis->getNumElements()) + "x1",
// inputs
{Binding{"basis", Basis}},
// output
{Binding{"Output", Output}}), mBixData(BixData) {
}

void Lasciify::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    UTF::UTF_Compiler unicodeCompiler(getInput(0), pb);

    std::cout << "before asciiset init" << std::endl;
    std::vector<unicode::BitTranslationSets> nAsciiSets;
    nAsciiSets.push_back(mBixData.matchBitXorCCs(0));
    for (unsigned i = 1; i < mBixData.maxAdd; i++) {
        nAsciiSets.push_back(mBixData.matchBitCCs(i));
    }
    std::cout << "after asciiset init" << std::endl;

    std::vector<std::vector<Var *>> nAsciiVars;
    nAsciiVars.assign(mBixData.maxAdd, {});

    std::cout << "before asciivar init" << std::endl;
    unsigned j = 0;
    for (auto& set : nAsciiSets) {
        std::cout << set.size() << std::endl; 
        for (unsigned i = 0; i < set.size(); i++) {
            std::cout << "before iteration " << i << " init" << std::endl;
            std::cout << "  before createvar" << std::endl;
            Var * v = pb.createVar("nAscii" + std::to_string(j) + "_bit" + std::to_string(i), pb.createZeroes());
            std::cout << "  after createvar" << std::endl;
            nAsciiVars[j].push_back(v);
            std::cout << "  before addtarget" << std::endl;
            unicodeCompiler.addTarget(v, re::makeCC(set[i], &cc::Unicode));
            std::cout << "  after addtarget" << std::endl;
            std::cout << "after iteration " << i << " init" << std::endl;
        }

        j++;
    }
    std::cout << "after asciivar init" << std::endl;

    if (LLVM_UNLIKELY(re::AlgorithmOptionIsSet(re::DisableIfHierarchy))) {
        unicodeCompiler.compile(UTF::UTF_Compiler::IfHierarchy::None);
    } else {
        unicodeCompiler.compile();
    }

    
    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    Var * outputVar = getOutputStreamVar("Output");
    std::vector<PabloAST *> output_basis(basis.size());

    std::cout << "before output assignment" << std::endl;
    for (unsigned i = 0; i < basis.size(); i++) {

        std::cout << "before initset use" << std::endl;
        auto initSet = nAsciiVars[0];
        if (i < initSet.size()) {
            output_basis[i] = pb.createXor(basis[i], initSet[i]);
        } else {
            output_basis[i] = basis[i];
        }

        std::cout << "before jset use" << std::endl;
        for (unsigned j = 1; j < mBixData.maxAdd; j++) {
            auto set = nAsciiVars[j];
            if (i < set.size()) {
                output_basis[i] = pb.createOr(pb.createAdvance(set[i], j), output_basis[i]);
            }
        }
        std::cout << "after jset use" << std::endl;

        pb.createAssign(pb.createExtract(outputVar, pb.getInteger(i)), output_basis[i]);
    }
    std::cout << "after output assignment" << std::endl;
}


typedef void (*ToLasciiFunctionType)(uint32_t fd);

ToLasciiFunctionType generatePipeline(CPUDriver & pxDriver) {
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
    //SHOW_STREAM(u8index);

    StreamSet * U21_u8indexed = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);

    StreamSet * U21 = P->CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21_u8indexed, U21);
    SHOW_BIXNUM(U21);

    NONASCII_bixData nonAscii_data;
    std::cout << "before insertion bixnums" << std::endl;
    auto insert_ccs = nonAscii_data.insertionBixNumCCs();
    std::cout << "after insertion bixnums" << std::endl;

    StreamSet * Insertion_BixNum = P->CreateStreamSet(insert_ccs.size());
    P->CreateKernelCall<CharClassesKernel>(insert_ccs, U21, Insertion_BixNum);
    SHOW_STREAM(Insertion_BixNum);

    StreamSet * SpreadMask = InsertionSpreadMask(P, Insertion_BixNum, InsertPosition::After);
    SHOW_STREAM(SpreadMask);

    StreamSet * ExpandedBasis = P->CreateStreamSet(21, 1);
    SpreadByMask(P, SpreadMask, U21, ExpandedBasis);
    SHOW_BIXNUM(ExpandedBasis);

    StreamSet * ascii_Basis = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<Lasciify>(nonAscii_data, ExpandedBasis, ascii_Basis);
    SHOW_BIXNUM(ascii_Basis);

    // Convert back to UTF8 from codepoints.
    StreamSet * const OutputBasis = P->CreateStreamSet(8);
    U21_to_UTF8(P, ascii_Basis, OutputBasis);

    SHOW_BIXNUM(OutputBasis);

    StreamSet * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<ToLasciiFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&LasciiOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    //  A CPU driver is capable of compiling and running Parabix programs on the CPU.
    CPUDriver driver("tolascii");

    //  Build and compile the Parabix pipeline by calling the Pipeline function above.
    ToLasciiFunctionType fn = generatePipeline(driver);
    
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