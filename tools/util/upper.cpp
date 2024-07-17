/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <cstdio>
#include <vector>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <re/adt/re_range.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/streamutils/stream_shift.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/scan/scanmatchgen.h>
#include <re/adt/re_name.h>
#include <re/cc/cc_kernel.h>
#include <re/cc/cc_compiler.h>
#include <re/transforms/name_intro.h>
#include <re/transforms/re_transformer.h>
#include <re/cc/cc_compiler_target.h>
#include <re/unicode/resolve_properties.h>
#include <string>
#include <toolchain/toolchain.h>
#include <pablo/pablo_toolchain.h>
#include <pablo/builder.hpp>
#include <pablo/pe_ones.h>
#include <pablo/pe_zeroes.h>
#include <pablo/bixnum/bixnum.h>
#include <fcntl.h>
#include <iostream>
#include <kernel/pipeline/driver/cpudriver.h>

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

// Command line options
static cl::OptionCategory UpperOptions("upper Options", "upper control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(UpperOptions));

class ToUpperKernel : public PabloKernel {
public:
    ToUpperKernel(KernelBuilder & kb, StreamSet * input, StreamSet * output)
        : PabloKernel(kb, "ToUpperKernel",
                      {Binding{"input", input}},
                      {Binding{"output", output}}) {}
protected:
    void generatePabloMethod() override;
};

void ToUpperKernel::generatePabloMethod() {
    //  pb is an object used for build Pablo language statements
    pablo::PabloBuilder pb(getEntryScope());

    //  bnc is an object that can perform arithmetic on sets of parallel bit streams
    BixNumCompiler bnc(pb);

    Var * input = getInputStreamVar("input");

    // ccc is an object that can compile character classes from a set of 8 parallel bit streams.
    cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), input);

    // Create a bitmask for lowercase 'a' to 'z'
    // PabloAST * a_to_z = pb.createAnd(pb.createGT(input, pb.createLiteral(0x60)), pb.createLT(input, pb.createLiteral(0x7B)));
    PabloAST * a_to_z = ccc.compileCC(re::makeCC(0x61, 0x7a, &cc::Unicode));

    BixNum basisVar = bnc.SubModular(input, 0x20);

    Var * fullWidthBasisVar = getOutputStreamVar("output");
    for (unsigned i = 0; i < 21; i++) {

        pb.createAssign(pb.createExtract(fullWidthBasisVar, pb.getInteger(i)), pb.createSel(a_to_z, basisVar[i], input[i]));
    }
}



// void ToUpperKernel::generatePabloMethod() {
//     // Create a PabloBuilder instance to help build the Pablo code
//     pablo::PabloBuilder pb(getEntryScope());
    
//     // Get the input stream set (the stream of characters to be processed)
//     PabloAST * input = getInputStreamSet("input")[0];

//     // Log the input stream set
//     // llvm::errs() << "Input StreamSet: " << visualizePabloAST(input) << "\n";

//     llvm::errs() << "ToUpperKernel: generatePabloMethod started\n";

//     // Initialize a variable to hold the ORed result of all lowercase checks
//     PabloAST * isLower = nullptr;
    
//     // Loop through each lowercase character 'a' to 'z' (0x61 to 0x7A in ASCII)
//     for (unsigned int c = 0x61; c <= 0x7A; ++c) {
//         // Create a character class (CC) for the current lowercase character
//         re::CC * singleLowerCase = re::makeCC(c);
        
//         // Log the singleLowerCase
//         llvm::errs() << "singleLowerCase for " << char(c) << " (" << c << "): " << singleLowerCase << "\n";
        
//         // Compile the character class into a Pablo AST that checks if the input matches the current character
//         cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), {input});
//         PabloAST * isLowerCase = ccc.compileCC("singleLowerCase", singleLowerCase, pb);
        
//         // Log the current character and its check result
//         // llvm::errs() << "isLowerCase for " << char(c) << " (" << c << "): " << visualizePabloAST(isLowerCase) << "\n";
        
//         // Combine the current character check with the previous checks using OR
//         if (isLower == nullptr) {
//             isLower = isLowerCase;
//         } else {
//             isLower = pb.createOr(isLower, isLowerCase);
//         }
//     }
    
//     // Log the constructed isLower condition (which checks for any lowercase character)
//     // llvm::errs() << "isLower constructed: " << visualizePabloAST(isLower) << "\n";

//     // Calculate the uppercase value (subtract 32 from the lowercase value)
//     PabloAST * lowerToUpperOffset = pb.createAdvance(input, pb.getInteger(-32));
//     llvm::errs() << "lowerToUpperOffset: " << static_cast<void*>(lowerToUpperOffset) << "\n";

//     // Create the selection to choose between the original value and the transformed value
//     PabloAST * upper = pb.createSel(isLower, lowerToUpperOffset, input);
    
//     // Log the result of the uppercase transformation
//     // llvm::errs() << "Upper: " << visualizePabloAST(upper) << "\n";

//     // Get the output stream variable
//     Var * output = getOutputStreamVar("output");
    
//     // Assign the transformed output to the output stream
//     pb.createAssign(pb.createExtract(output, pb.getInteger(0)), upper);

//     // Log the final output
//     // llvm::errs() << "Output: " << visualizePabloAST(output) << "\n";
//     llvm::errs() << "ToUpperKernel: generatePabloMethod completed\n";
// }

PabloAST * createUnicodeToUpperMapping(PabloBuilder &pb, PabloAST *input, PabloAST *isLower) {
    std::vector<std::pair<UCD::codepoint_t, UCD::codepoint_t>> mapping = {
        {0x61, 0x41}, // 'a' -> 'A'
        {0x62, 0x42}, // 'b' -> 'B'
        {0x63, 0x43}, // 'c' -> 'C'
        {0x64, 0x44}, // 'd' -> 'D'
        {0x65, 0x45}, // 'e' -> 'E'
        {0x66, 0x46}, // 'f' -> 'F'
        {0x67, 0x47}, // 'g' -> 'G'
        {0x68, 0x48}, // 'h' -> 'H'
        {0x69, 0x49}, // 'i' -> 'I'
        {0x6A, 0x4A}, // 'j' -> 'J'
        {0x6B, 0x4B}, // 'k' -> 'K'
        {0x6C, 0x4C}, // 'l' -> 'L'
        {0x6D, 0x4D}, // 'm' -> 'M'
        {0x6E, 0x4E}, // 'n' -> 'N'
        {0x6F, 0x4F}, // 'o' -> 'O'
        {0x70, 0x50}, // 'p' -> 'P'
        {0x71, 0x51}, // 'q' -> 'Q'
        {0x72, 0x52}, // 'r' -> 'R'
        {0x73, 0x53}, // 's' -> 'S'
        {0x74, 0x54}, // 't' -> 'T'
        {0x75, 0x55}, // 'u' -> 'U'
        {0x76, 0x56}, // 'v' -> 'V'
        {0x77, 0x57}, // 'w' -> 'W'
        {0x78, 0x58}, // 'x' -> 'X'
        {0x79, 0x59}, // 'y' -> 'Y'
        {0x7A, 0x5A}  // 'z' -> 'Z'
    };

    PabloAST * result = nullptr;

    for (const auto &pair : mapping) {
        PabloAST * lowercaseChar = pb.createAdvance(pb.createOnes(), pb.getInteger(pair.first));
        PabloAST * isCurrentLower = pb.createAnd(isLower, pb.createOr(input, lowercaseChar));
        PabloAST * uppercaseChar = pb.createAdvance(pb.createOnes(), pb.getInteger(pair.second));

        llvm::errs() << "Mapping: " << static_cast<char>(pair.first) << " -> " << static_cast<char>(pair.second) << "\n";

        if (result == nullptr) {
            result = pb.createSel(isCurrentLower, uppercaseChar, input);
        } else {
            result = pb.createSel(isCurrentLower, uppercaseChar, result);
        }
    }

    return result;
}


typedef void (*UpperFunctionType)(uint32_t fd);

UpperFunctionType generatePipeline(CPUDriver & pxDriver) {
    auto & b = pxDriver.getBuilder();
    auto P = pxDriver.makePipeline({Binding{b.getInt32Ty(), "inputFileDecriptor"}}, {});
    
    Scalar * fileDescriptor = P->getInputScalar("inputFileDecriptor");
    StreamSet * ByteStream = P->CreateStreamSet(1, 8);
    
    llvm::errs() << "generatePipeline: ReadSourceKernel started\n";
    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    SHOW_BYTES(ByteStream);
    llvm::errs() << "generatePipeline: ReadSourceKernel completed\n";
    
    StreamSet * upperStream = P->CreateStreamSet(1, 8);
    llvm::errs() << "generatePipeline: ToUpperKernel started\n";
    P->CreateKernelCall<ToUpperKernel>(ByteStream, upperStream);
    SHOW_BYTES(upperStream);
    llvm::errs() << "generatePipeline: ToUpperKernel completed\n";

    llvm::errs() << "generatePipeline: StdOutKernel started\n";
    P->CreateKernelCall<StdOutKernel>(upperStream);
    llvm::errs() << "generatePipeline: StdOutKernel completed\n";

    return reinterpret_cast<UpperFunctionType>(P->compile());
}



int main(int argc, char *argv[]) {
    llvm::errs() << "main: ParseCommandLineOptions started\n";
    codegen::ParseCommandLineOptions(argc, argv, {&UpperOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});
    llvm::errs() << "main: ParseCommandLineOptions completed\n";

    llvm::errs() << "main: CPUDriver initialization started\n";
    CPUDriver driver("upper");
    llvm::errs() << "main: CPUDriver initialization completed\n";

    llvm::errs() << "main: generatePipeline started\n";
    UpperFunctionType fn = generatePipeline(driver);
    llvm::errs() << "main: generatePipeline completed\n";
    
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        llvm::errs() << "main: Pipeline execution started\n";
        fn(fd);
        llvm::errs() << "main: Pipeline execution completed\n";
        close(fd);
    }
    return 0;
}
