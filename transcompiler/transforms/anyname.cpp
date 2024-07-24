#include <vector>
#include <fcntl.h>
#include <string>
#include <cout>

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

namespace cl = llvm::cl;

static cl::OptionCategory AnyNameOptions("any-name Options", "any-name control options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(AnyNameOptions));

class UnicodeNameConverter : public pablo::PabloKernel {
public:
    UnicodeNameConverter(KernelBuilder & b, StreamSet * U21, StreamSet * U21out)
    : pablo::PabloKernel(b, "UnicodeNameConverter",
                         {kernel::Binding{"U21", U21}},
                         {kernel::Binding{"U21out", U21out}}),
                         mNamePropertyObject(dyn_cast<UCD::StringPropertyObject>(UCD::get_NA_PropertyObject())) {
        if (mNamePropertyObject == nullptr) {
            llvm::report_fatal_error("Failed to initialize name property object.");
        }
    }

protected:
    void generatePabloMethod() override;

private:
    void convertCodepointsToNames(PabloBuilder &pb, const std::vector<PabloAST *> &U21, Var * U21out);
    UCD::StringPropertyObject* mNamePropertyObject;
};

void UnicodeNameConverter::generatePabloMethod() {
    pablo::PabloBuilder pb(getEntryScope());
    std::vector<pablo::PabloAST *> U21 = getInputStreamSet("U21");
    pablo::Var * U21out = getOutputStreamVar("U21out");
    //convertCodepointsToNames(pb, U21, U21out);

    for (unsigned i = 0; i < U21.size(); i++) {
        
        UCD::codepoint_t codepoint = static_cast<UCD::codepoint_t>(i); // Simplified for illustration
        std::string name = mNamePropertyObject->GetStringValue(codepoint);
        std::string formattedName = "\\N{" + name + "}";

        std::cout << formattedName << std::endl;

        for (size_t j = 0; j < formattedName.size(); ++j) {
            auto output = pb.getInteger(static_cast<uint8_t>(formattedName[j]));
            //pb.getInteger(i * 8 + j)
            pb.createAssign(pb.createExtract(U21out, pb.getInteger(i)), U21[i]);
        }
    }
}

void UnicodeNameConverter::convertCodepointsToNames(PabloBuilder &pb, const std::vector<PabloAST *> &U21, Var * U21out) {
    
}

typedef void (*AnyNameFunctionType)(uint32_t fd);

AnyNameFunctionType generatePipeline(CPUDriver & pxDriver) {
    auto & b = pxDriver.getBuilder();
    auto P = pxDriver.makePipeline({kernel::Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});

    auto * fileDescriptor = P->getInputScalar("inputFileDescriptor");
    auto * ByteStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    SHOW_BYTES(ByteStream);

    auto * BasisBits = P->CreateStreamSet(8, 1);
    P->CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    SHOW_BIXNUM(BasisBits);

    auto * u8index = P->CreateStreamSet(1, 1);
    P->CreateKernelCall<UTF8_index>(BasisBits, u8index);
    SHOW_STREAM(u8index);

    auto * U21_u8indexed = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);

    auto * U21 = P->CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21_u8indexed, U21);
    SHOW_BIXNUM(U21);

    auto * U21out = P->CreateStreamSet(21, 1);
    P->CreateKernelCall<UnicodeNameConverter>(U21, U21out);
    SHOW_BIXNUM(U21out);

    auto * OutputBasis = P->CreateStreamSet(8);
    U21_to_UTF8(P, U21out, OutputBasis);
    SHOW_BIXNUM(OutputBasis);

    auto * OutputBytes = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P->CreateKernelCall<StdOutKernel>(OutputBytes);

    return reinterpret_cast<AnyNameFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&AnyNameOptions, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});

    CPUDriver driver("any_name");

    AnyNameFunctionType fn = generatePipeline(driver);

    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        fn(fd);
        close(fd);
    }
    return 0;
}