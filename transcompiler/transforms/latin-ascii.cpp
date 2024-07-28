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
    std::vector<re::CC *> nonAscii_Insertion_BixNumCCs();
    unicode::BitTranslationSets nonAscii_1st_BitXorCCs();
    unicode::BitTranslationSets nonAscii_2nd_BitCCs();
    unicode::BitTranslationSets nonAscii_3rd_BitCCs();
    unicode::BitTranslationSets nonAscii_4th_BitCCs();
    unicode::BitTranslationSets nonAscii_5th_BitCCs();
    std::unordered_map<codepoint_t, unsigned> mnonAscii_length;
    unicode::TranslationMap mnonAscii_CharMap[5];
};

NONASCII_bixData::NONASCII_bixData() {
    std::cout << "generating bixdata" << std::endl;

    for (auto& pair : latinnonasciicodes) {
        mnonAscii_length.emplace(pair.first, pair.second.size());

        unsigned int i = 0;
        for (auto& target : pair.second) {
            mnonAscii_CharMap[i].emplace(pair.first, target);
            i++;
        }
    }
}

std::vector<re::CC *> NONASCII_bixData::nonAscii_Insertion_BixNumCCs() {
    unicode::BitTranslationSets BixNumCCs;

    BixNumCCs.push_back(UCD::UnicodeSet());
    BixNumCCs.push_back(UCD::UnicodeSet());
    BixNumCCs.push_back(UCD::UnicodeSet());

    for (auto& p : mnonAscii_length) {
        

        auto insert_amt = p.second - 1;

        if ((insert_amt & 1) == 1) {
            BixNumCCs[0].insert(p.first);
        }
        if ((insert_amt & 2) == 2) {
            BixNumCCs[1].insert(p.first);
        }
        if ((insert_amt & 4) == 4) {
            BixNumCCs[2].insert(p.first);
        }

    }

    //auto & out = llvm::errs();
    //for (auto& p : BixNumCCs) {
        //p.print(out);
    //}

    return {re::makeCC(BixNumCCs[0], &cc::Unicode),
            re::makeCC(BixNumCCs[1], &cc::Unicode),
            re::makeCC(BixNumCCs[2], &cc::Unicode)
    };
}

unicode::BitTranslationSets NONASCII_bixData::nonAscii_1st_BitXorCCs() {
    return unicode::ComputeBitTranslationSets(mnonAscii_CharMap[0]);
}

unicode::BitTranslationSets NONASCII_bixData::nonAscii_2nd_BitCCs() {
    return unicode::ComputeBitTranslationSets(mnonAscii_CharMap[1], unicode::XlateMode::LiteralBit);
}

unicode::BitTranslationSets NONASCII_bixData::nonAscii_3rd_BitCCs() {
    return unicode::ComputeBitTranslationSets(mnonAscii_CharMap[2], unicode::XlateMode::LiteralBit);
}

unicode::BitTranslationSets NONASCII_bixData::nonAscii_4th_BitCCs() {
    return unicode::ComputeBitTranslationSets(mnonAscii_CharMap[3], unicode::XlateMode::LiteralBit);
}

unicode::BitTranslationSets NONASCII_bixData::nonAscii_5th_BitCCs() {
    return unicode::ComputeBitTranslationSets(mnonAscii_CharMap[4], unicode::XlateMode::LiteralBit);
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

    unicode::BitTranslationSets nAscii1 = mBixData.nonAscii_1st_BitXorCCs();
    unicode::BitTranslationSets nAscii2 = mBixData.nonAscii_2nd_BitCCs();
    unicode::BitTranslationSets nAscii3 = mBixData.nonAscii_3rd_BitCCs();
    unicode::BitTranslationSets nAscii4 = mBixData.nonAscii_4th_BitCCs();
    unicode::BitTranslationSets nAscii5 = mBixData.nonAscii_5th_BitCCs();

    std::vector<Var *> nAscii1_Vars;
    std::vector<Var *> nAscii2_Vars;
    std::vector<Var *> nAscii3_Vars;
    std::vector<Var *> nAscii4_Vars;
    std::vector<Var *> nAscii5_Vars;

    for (unsigned i = 0; i < nAscii1.size(); i++) {
        Var * v = pb.createVar("nAscii1_bit" + std::to_string(i), pb.createZeroes());
        nAscii1_Vars.push_back(v);
        unicodeCompiler.addTarget(v, re::makeCC(nAscii1[i], &cc::Unicode));
    }
    for (unsigned i = 0; i < nAscii2.size(); i++) {
        Var * v = pb.createVar("nAscii2_bit" + std::to_string(i), pb.createZeroes());
        nAscii2_Vars.push_back(v);
        unicodeCompiler.addTarget(v, re::makeCC(nAscii2[i], &cc::Unicode));
    }
    for (unsigned i = 0; i < nAscii3.size(); i++) {
        Var * v = pb.createVar("nAscii3_bit" + std::to_string(i), pb.createZeroes());
        nAscii3_Vars.push_back(v);
        unicodeCompiler.addTarget(v, re::makeCC(nAscii3[i], &cc::Unicode));
    }
    for (unsigned i = 0; i < nAscii4.size(); i++) {
        Var * v = pb.createVar("nAscii4_bit" + std::to_string(i), pb.createZeroes());
        nAscii4_Vars.push_back(v);
        unicodeCompiler.addTarget(v, re::makeCC(nAscii4[i], &cc::Unicode));
    }
    for (unsigned i = 0; i < nAscii5.size(); i++) {
        Var * v = pb.createVar("nAscii5_bit" + std::to_string(i), pb.createZeroes());
        nAscii5_Vars.push_back(v);
        unicodeCompiler.addTarget(v, re::makeCC(nAscii5[i], &cc::Unicode));
    }

    if (LLVM_UNLIKELY(re::AlgorithmOptionIsSet(re::DisableIfHierarchy))) {
        unicodeCompiler.compile(UTF::UTF_Compiler::IfHierarchy::None);
    } else {
        unicodeCompiler.compile();
    }

    std::vector<PabloAST *> basis = getInputStreamSet("basis");
    Var * outputVar = getOutputStreamVar("Output");
    std::vector<PabloAST *> output_basis(basis.size());

    for (unsigned i = 0; i < basis.size(); i++) {
        if (i < nAscii1.size()) {
            output_basis[i] = pb.createXor(basis[i], nAscii1_Vars[i]);
        } else {
            output_basis[i] = basis[i];
        }
        if (i < nAscii2.size()) {
            output_basis[i] = pb.createOr(pb.createAdvance(nAscii2_Vars[i], 1), output_basis[i]);
        }
        if (i < nAscii3.size()) {
            output_basis[i] = pb.createOr(pb.createAdvance(nAscii3_Vars[i], 2), output_basis[i]);
        }
        if (i < nAscii4.size()) {
            output_basis[i] = pb.createOr(pb.createAdvance(nAscii4_Vars[i], 3), output_basis[i]);
        }
        if (i < nAscii5.size()) {
            output_basis[i] = pb.createOr(pb.createAdvance(nAscii5_Vars[i], 3), output_basis[i]);
        }
        pb.createAssign(pb.createExtract(outputVar, pb.getInteger(i)), output_basis[i]);
    }
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
    auto insert_ccs = nonAscii_data.nonAscii_Insertion_BixNumCCs();

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