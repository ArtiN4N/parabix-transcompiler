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

#include "kernel/fullhalfify_kernel.h"
#include "kernel/halffullify_kernel.h"
#include "kernel/lowerify_kernel.h"
#include "kernel/removeify_kernel.h"

#include "kernel/titleify_kernel.h"
#include "kernel/upperify_kernel.h"
#include "data/latingreekungegndata.h"
//#include "data/greeklatindata.h"
#include "data/lasciidata.h"
/*#include "data/latingujaratidata.h"
#include "data/gujaratilatindata.h"
#include "data/latingurmukhidata.h"
#include "data/gurmukhilatindata.h"
#include "data/latinhanguldata.h"
#include "data/hangullatindata.h"
#include "data/latinhebrewdata.h"
#include "data/hebrewlatindata.h"*/
#include "kernel/replaceify_kernel.h"

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace pablo;

//  These declarations are for command line processing.
//  See the LLVM CommandLine 2.0 Library Manual https://llvm.org/docs/CommandLine.html
static cl::OptionCategory TranscompilerAutoGenOptions("transcompilerAutoGen Options", "transcompilerAutoGen control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(TranscompilerAutoGenOptions));

typedef void (*TranscompilerAutoGenFunctionType)(uint32_t fd);

TranscompilerAutoGenFunctionType generatePipeline(CPUDriver & pxDriver) {
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

    // Get the basis bits
    StreamSet * BasisBits = P->CreateStreamSet(8, 1);
    P->CreateKernelCall<S2PKernel>(ByteStream, BasisBits);

    // Convert into codepoints
    StreamSet * u8index = P->CreateStreamSet(1, 1);
    P->CreateKernelCall<UTF8_index>(BasisBits, u8index);

    StreamSet * U21_u8indexed = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);

    StreamSet * U21 = P->CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21_u8indexed, U21);

    StreamSet * finalBasis1 = P->CreateStreamSet(21, 1);
    replace_bixData LAT_replace_data(asciiCodeData);
    ReplaceByBixData(P, LAT_replace_data, U21, finalBasis1);

    StreamSet * finalBasis2 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data1(latingreekungegndata);
    ReplaceByBixData(P, SCRIPT_replace_data1, finalBasis1, finalBasis2);
    //SHOW_BIXNUM(finalBasis1);
    
    /*StreamSet * finalBasis2 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data2(greeklatindata);
    ReplaceByBixData(P, SCRIPT_replace_data2, U21, finalBasis2);
    SHOW_BIXNUM(finalBasis2);*/
    /*StreamSet * finalBasis3 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data3(latingujaratidata);
    ReplaceByBixData(P, SCRIPT_replace_data3, finalBasis2, finalBasis3);
    StreamSet * finalBasis4 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data4(gujaratilatindata);
    ReplaceByBixData(P, SCRIPT_replace_data4, finalBasis3, finalBasis4);
    StreamSet * finalBasis5 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data5(latingurmukhidata);
    ReplaceByBixData(P, SCRIPT_replace_data5, finalBasis4, finalBasis5);
    StreamSet * finalBasis6 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data6(gurmukhilatindata);
    ReplaceByBixData(P, SCRIPT_replace_data6, finalBasis5, finalBasis6);
    StreamSet * finalBasis7 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data7(latinhanguldata);
    ReplaceByBixData(P, SCRIPT_replace_data7, finalBasis6, finalBasis7);
    StreamSet * finalBasis8 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data8(hangullatindata);
    ReplaceByBixData(P, SCRIPT_replace_data8, finalBasis7, finalBasis8);
    StreamSet * finalBasis9 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data9(latinhebrewdata);
    ReplaceByBixData(P, SCRIPT_replace_data9, finalBasis8, finalBasis9);
    StreamSet * finalBasis10 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data10(hebrewlatindata);
    ReplaceByBixData(P, SCRIPT_replace_data10, finalBasis9, finalBasis10);*/

    StreamSet * const OutputBasis = P->CreateStreamSet(8);

    U21_to_UTF8(P, finalBasis2, OutputBasis);

    StreamSet * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<TranscompilerAutoGenFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    //  ParseCommandLineOptions uses the LLVM CommandLine processor, but we also add
    //  standard Parabix command line options such as -help, -ShowPablo and many others.
    codegen::ParseCommandLineOptions(argc, argv, {&TranscompilerAutoGenOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    //  A CPU driver is capable of compiling and running Parabix programs on the CPU.
    CPUDriver driver("transcompilerAutoGen");

    //  Build and compile the Parabix pipeline by calling the Pipeline function above.
    TranscompilerAutoGenFunctionType fn = generatePipeline(driver);

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