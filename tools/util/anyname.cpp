#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <regex>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/IR/Module.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <unicode/unistr.h>
#include <unicode/uchar.h>
#include <unicode/ustream.h>
#include <fcntl.h>

using namespace kernel;
using namespace llvm;
using namespace pablo;

static cl::OptionCategory UnicodeConverterOptions("unicode_converter Options", "unicode_converter control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(UnicodeConverterOptions));

class UnicodeNameConverter : public pablo::PabloKernel {
public:
    UnicodeNameConverter(KernelBuilder & b, StreamSet * inputStream, StreamSet * outputStream)
    : pablo::PabloKernel(b, "UnicodeNameConverter",
                         {Binding{"inputStream", inputStream}},
                         {Binding{"outputStream", outputStream}}) {}

protected:
    void generatePabloMethod() override;
};

void UnicodeNameConverter::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    BixNumCompiler bnc(pb);

    std::vector<PabloAST *> U21 = getInputStreamSet("inputStream");

    std::unordered_map<UCD::codepoint_t, std::string> charToNameMap;
    std::unordered_map<std::string, UCD::codepoint_t> nameToCharMap;

    for (UCD::codepoint_t cp = 32; cp < 127; ++cp) {
        UErrorCode status = U_ZERO_ERROR;
        char buffer[100];
        u_charName(cp, U_UNICODE_CHAR_NAME, buffer, 100, &status);
        std::string nameStr(buffer);

        charToNameMap[cp] = nameStr;
        nameToCharMap[nameStr] = cp;
    }

    Var * unicodeNamesVar = getOutputStreamVar("outputStream");
    for (unsigned i = 0; i < 21; ++i) {
        UCD::codepoint_t charCode = static_cast<UCD::codepoint_t>(U21[i]->value());
        PabloAST * nameAst = pb.createString(charToNameMap[charCode]);
        pb.createAssign(pb.createExtract(unicodeNamesVar, pb.getInteger(i)), nameAst);
    }
}

typedef void (*ConversionFunctionType)(uint32_t fd);

ConversionFunctionType generatePipeline(CPUDriver & pxDriver) {
    auto & b = pxDriver.getBuilder();
    auto P = pxDriver.makePipeline({Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});

    Scalar * fileDescriptor = P->getInputScalar("inputFileDescriptor");
    StreamSet * ByteStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);

    StreamSet * BasisBits = P->CreateStreamSet(8, 1);
    P->CreateKernelCall<S2PKernel>(ByteStream, BasisBits);

    StreamSet * u8index = P->CreateStreamSet(1, 1);
    P->CreateKernelCall<UTF8_index>(BasisBits, u8index);

    StreamSet * U21_u8indexed = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);

    StreamSet * U21 = P->CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21_u8indexed, U21);

    StreamSet * unicodeNames = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<UnicodeNameConverter>(U21, unicodeNames);

    StreamSet * OutputBasis = P->CreateStreamSet(8);
    U21_to_UTF8(P, unicodeNames, OutputBasis);

    StreamSet * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<ConversionFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&UnicodeConverterOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    CPUDriver driver("unicode_converter");

    ConversionFunctionType fn = generatePipeline(driver);

    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        fn(fd);
        close(fd);
    }
    return 0;
}
