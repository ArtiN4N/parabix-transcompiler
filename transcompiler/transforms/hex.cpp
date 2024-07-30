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
#include "kernel/replaceify_kernel.h"
#include "kernel/titleify_kernel.h"
#include "kernel/upperify_kernel.h"
#include "data/latinarabicdata.h"
#include "data/arabiclatindata.h"
#include "data/latinarmeniandata.h"
#include "data/armenianlatindata.h"
#include "data/latinbengalidata.h"
#include "data/bengalilatindata.h"
#include "data/latinbopomofodata.h"
#include "data/bopomofolatindata.h"
#include "data/latincanadianaboriginaldata.h"
#include "data/canadianaboriginallatindata.h"
#include "data/latincyrillicdata.h"
#include "data/cyrilliclatindata.h"
#include "data/latindevanagaridata.h"
#include "data/devanagarilatindata.h"
#include "data/latinethiopicdata.h"
#include "data/ethiopiclatindata.h"
#include "data/latinethiopicalalocdata.h"
#include "data/latinethiopicaethiopicadata.h"
#include "data/latinethiopicbeta_metsehafdata.h"
#include "data/latinethiopicies_jes_1964data.h"
#include "data/latinethiopiclambdindata.h"
#include "data/latinethiopicseradata.h"
#include "data/latinethiopictekie_alibekitdata.h"
#include "data/latinethiopicwilliamsondata.h"
#include "data/latinethiopicxalagetdata.h"
#include "data/ethiopiclatindata.h"
#include "data/latingeorgiandata.h"
#include "data/georgianlatindata.h"
#include "data/latingreekdata.h"
#include "data/greeklatindata.h"
#include "data/latingreekungegndata.h"
#include "data/greeklatindata.h"
#include "data/latingujaratidata.h"
#include "data/gujaratilatindata.h"
#include "data/latingurmukhidata.h"
#include "data/gurmukhilatindata.h"
#include "data/latinhanguldata.h"
#include "data/hangullatindata.h"
#include "data/latinhebrewdata.h"
#include "data/hebrewlatindata.h"
#include "data/latinhiraganadata.h"
#include "data/hiraganalatindata.h"
#include "data/latinjamodata.h"
#include "data/hangullatindata.h"
#include "data/latinkannadadata.h"
#include "data/kannadalatindata.h"
#include "data/latinkatakanadata.h"
#include "data/katakanalatindata.h"
#include "data/latinmalayalamdata.h"
#include "data/malayalamlatindata.h"
#include "data/latinoriyadata.h"
#include "data/oriyalatindata.h"
#include "data/latinsyriacdata.h"
#include "data/syriaclatindata.h"
#include "data/latintamildata.h"
#include "data/tamillatindata.h"
#include "data/latintelugudata.h"
#include "data/telugulatindata.h"
#include "data/latinthaanadata.h"
#include "data/thaanalatindata.h"
#include "data/latinthaidata.h"
#include "data/thailatindata.h"
#include "data/latinrussianbgndata.h"

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
    replace_bixData SCRIPT_replace_data1(latinarabicdata);
    ReplaceByBixData(P, SCRIPT_replace_data1, U21, finalBasis1);
    StreamSet * finalBasis2 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data2(arabiclatindata);
    ReplaceByBixData(P, SCRIPT_replace_data2, finalBasis1, finalBasis2);
    StreamSet * finalBasis3 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data3(latinarmeniandata);
    ReplaceByBixData(P, SCRIPT_replace_data3, finalBasis2, finalBasis3);
    StreamSet * finalBasis4 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data4(armenianlatindata);
    ReplaceByBixData(P, SCRIPT_replace_data4, finalBasis3, finalBasis4);
    StreamSet * finalBasis5 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data5(latinbengalidata);
    ReplaceByBixData(P, SCRIPT_replace_data5, finalBasis4, finalBasis5);
    StreamSet * finalBasis6 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data6(bengalilatindata);
    ReplaceByBixData(P, SCRIPT_replace_data6, finalBasis5, finalBasis6);
    StreamSet * finalBasis7 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data7(latinbopomofodata);
    ReplaceByBixData(P, SCRIPT_replace_data7, finalBasis6, finalBasis7);
    StreamSet * finalBasis8 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data8(bopomofolatindata);
    ReplaceByBixData(P, SCRIPT_replace_data8, finalBasis7, finalBasis8);
    StreamSet * finalBasis9 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data9(latincanadianaboriginaldata);
    ReplaceByBixData(P, SCRIPT_replace_data9, finalBasis8, finalBasis9);
    StreamSet * finalBasis10 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data10(canadianaboriginallatindata);
    ReplaceByBixData(P, SCRIPT_replace_data10, finalBasis9, finalBasis10);
    StreamSet * finalBasis11 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data11(latincyrillicdata);
    ReplaceByBixData(P, SCRIPT_replace_data11, finalBasis10, finalBasis11);
    StreamSet * finalBasis12 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data12(cyrilliclatindata);
    ReplaceByBixData(P, SCRIPT_replace_data12, finalBasis11, finalBasis12);
    StreamSet * finalBasis13 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data13(latindevanagaridata);
    ReplaceByBixData(P, SCRIPT_replace_data13, finalBasis12, finalBasis13);
    StreamSet * finalBasis14 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data14(devanagarilatindata);
    ReplaceByBixData(P, SCRIPT_replace_data14, finalBasis13, finalBasis14);
    StreamSet * finalBasis15 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data15(latinethiopicdata);
    ReplaceByBixData(P, SCRIPT_replace_data15, finalBasis14, finalBasis15);
    StreamSet * finalBasis16 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data16(ethiopiclatindata);
    ReplaceByBixData(P, SCRIPT_replace_data16, finalBasis15, finalBasis16);
    StreamSet * finalBasis17 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data17(latinethiopicalalocdata);
    ReplaceByBixData(P, SCRIPT_replace_data17, finalBasis16, finalBasis17);
    StreamSet * finalBasis18 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data18(latinethiopicaethiopicadata);
    ReplaceByBixData(P, SCRIPT_replace_data18, finalBasis17, finalBasis18);
    StreamSet * finalBasis19 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data19(latinethiopicbeta_metsehafdata);
    ReplaceByBixData(P, SCRIPT_replace_data19, finalBasis18, finalBasis19);
    StreamSet * finalBasis20 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data20(latinethiopicies_jes_1964data);
    ReplaceByBixData(P, SCRIPT_replace_data20, finalBasis19, finalBasis20);
    StreamSet * finalBasis21 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data21(latinethiopiclambdindata);
    ReplaceByBixData(P, SCRIPT_replace_data21, finalBasis20, finalBasis21);
    StreamSet * finalBasis22 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data22(latinethiopicseradata);
    ReplaceByBixData(P, SCRIPT_replace_data22, finalBasis21, finalBasis22);
    StreamSet * finalBasis23 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data23(latinethiopictekie_alibekitdata);
    ReplaceByBixData(P, SCRIPT_replace_data23, finalBasis22, finalBasis23);
    StreamSet * finalBasis24 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data24(latinethiopicwilliamsondata);
    ReplaceByBixData(P, SCRIPT_replace_data24, finalBasis23, finalBasis24);
    StreamSet * finalBasis25 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data25(latinethiopicxalagetdata);
    ReplaceByBixData(P, SCRIPT_replace_data25, finalBasis24, finalBasis25);
    StreamSet * finalBasis26 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data26(ethiopiclatindata);
    ReplaceByBixData(P, SCRIPT_replace_data26, finalBasis25, finalBasis26);
    StreamSet * finalBasis27 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data27(latingeorgiandata);
    ReplaceByBixData(P, SCRIPT_replace_data27, finalBasis26, finalBasis27);
    StreamSet * finalBasis28 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data28(georgianlatindata);
    ReplaceByBixData(P, SCRIPT_replace_data28, finalBasis27, finalBasis28);
    StreamSet * finalBasis29 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data29(latingreekdata);
    ReplaceByBixData(P, SCRIPT_replace_data29, finalBasis28, finalBasis29);
    StreamSet * finalBasis30 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data30(greeklatindata);
    ReplaceByBixData(P, SCRIPT_replace_data30, finalBasis29, finalBasis30);
    StreamSet * finalBasis31 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data31(latingreekungegndata);
    ReplaceByBixData(P, SCRIPT_replace_data31, finalBasis30, finalBasis31);
    StreamSet * finalBasis32 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data32(greeklatindata);
    ReplaceByBixData(P, SCRIPT_replace_data32, finalBasis31, finalBasis32);
    StreamSet * finalBasis33 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data33(latingujaratidata);
    ReplaceByBixData(P, SCRIPT_replace_data33, finalBasis32, finalBasis33);
    StreamSet * finalBasis34 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data34(gujaratilatindata);
    ReplaceByBixData(P, SCRIPT_replace_data34, finalBasis33, finalBasis34);
    StreamSet * finalBasis35 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data35(latingurmukhidata);
    ReplaceByBixData(P, SCRIPT_replace_data35, finalBasis34, finalBasis35);
    StreamSet * finalBasis36 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data36(gurmukhilatindata);
    ReplaceByBixData(P, SCRIPT_replace_data36, finalBasis35, finalBasis36);
    StreamSet * finalBasis37 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data37(latinhanguldata);
    ReplaceByBixData(P, SCRIPT_replace_data37, finalBasis36, finalBasis37);
    StreamSet * finalBasis38 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data38(hangullatindata);
    ReplaceByBixData(P, SCRIPT_replace_data38, finalBasis37, finalBasis38);
    StreamSet * finalBasis39 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data39(latinhebrewdata);
    ReplaceByBixData(P, SCRIPT_replace_data39, finalBasis38, finalBasis39);
    StreamSet * finalBasis40 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data40(hebrewlatindata);
    ReplaceByBixData(P, SCRIPT_replace_data40, finalBasis39, finalBasis40);
    StreamSet * finalBasis41 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data41(latinhiraganadata);
    ReplaceByBixData(P, SCRIPT_replace_data41, finalBasis40, finalBasis41);
    StreamSet * finalBasis42 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data42(hiraganalatindata);
    ReplaceByBixData(P, SCRIPT_replace_data42, finalBasis41, finalBasis42);
    StreamSet * finalBasis43 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data43(latinjamodata);
    ReplaceByBixData(P, SCRIPT_replace_data43, finalBasis42, finalBasis43);
    StreamSet * finalBasis44 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data44(hangullatindata);
    ReplaceByBixData(P, SCRIPT_replace_data44, finalBasis43, finalBasis44);
    StreamSet * finalBasis45 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data45(latinkannadadata);
    ReplaceByBixData(P, SCRIPT_replace_data45, finalBasis44, finalBasis45);
    StreamSet * finalBasis46 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data46(kannadalatindata);
    ReplaceByBixData(P, SCRIPT_replace_data46, finalBasis45, finalBasis46);
    StreamSet * finalBasis47 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data47(latinkatakanadata);
    ReplaceByBixData(P, SCRIPT_replace_data47, finalBasis46, finalBasis47);
    StreamSet * finalBasis48 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data48(katakanalatindata);
    ReplaceByBixData(P, SCRIPT_replace_data48, finalBasis47, finalBasis48);
    StreamSet * finalBasis49 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data49(latinmalayalamdata);
    ReplaceByBixData(P, SCRIPT_replace_data49, finalBasis48, finalBasis49);
    StreamSet * finalBasis50 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data50(malayalamlatindata);
    ReplaceByBixData(P, SCRIPT_replace_data50, finalBasis49, finalBasis50);
    StreamSet * finalBasis51 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data51(latinoriyadata);
    ReplaceByBixData(P, SCRIPT_replace_data51, finalBasis50, finalBasis51);
    StreamSet * finalBasis52 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data52(oriyalatindata);
    ReplaceByBixData(P, SCRIPT_replace_data52, finalBasis51, finalBasis52);
    StreamSet * finalBasis53 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data53(latinsyriacdata);
    ReplaceByBixData(P, SCRIPT_replace_data53, finalBasis52, finalBasis53);
    StreamSet * finalBasis54 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data54(syriaclatindata);
    ReplaceByBixData(P, SCRIPT_replace_data54, finalBasis53, finalBasis54);
    StreamSet * finalBasis55 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data55(latintamildata);
    ReplaceByBixData(P, SCRIPT_replace_data55, finalBasis54, finalBasis55);
    StreamSet * finalBasis56 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data56(tamillatindata);
    ReplaceByBixData(P, SCRIPT_replace_data56, finalBasis55, finalBasis56);
    StreamSet * finalBasis57 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data57(latintelugudata);
    ReplaceByBixData(P, SCRIPT_replace_data57, finalBasis56, finalBasis57);
    StreamSet * finalBasis58 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data58(telugulatindata);
    ReplaceByBixData(P, SCRIPT_replace_data58, finalBasis57, finalBasis58);
    StreamSet * finalBasis59 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data59(latinthaanadata);
    ReplaceByBixData(P, SCRIPT_replace_data59, finalBasis58, finalBasis59);
    StreamSet * finalBasis60 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data60(thaanalatindata);
    ReplaceByBixData(P, SCRIPT_replace_data60, finalBasis59, finalBasis60);
    StreamSet * finalBasis61 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data61(latinthaidata);
    ReplaceByBixData(P, SCRIPT_replace_data61, finalBasis60, finalBasis61);
    StreamSet * finalBasis62 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data62(thailatindata);
    ReplaceByBixData(P, SCRIPT_replace_data62, finalBasis61, finalBasis62);
    StreamSet * finalBasis63 = P->CreateStreamSet(21, 1);
    replace_bixData SCRIPT_replace_data63(latinrussianbgndata);
    ReplaceByBixData(P, SCRIPT_replace_data63, finalBasis62, finalBasis63);

    StreamSet * const OutputBasis = P->CreateStreamSet(8);

    U21_to_UTF8(P, finalBasis63, OutputBasis);

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