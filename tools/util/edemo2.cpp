#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <kernel/core/kernel_builder.h>
#include <toolchain/toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/core/streamset.h>
#include <kernel/io/stdout_kernel.h>
#include <llvm/ADT/StringRef.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <fcntl.h>

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P->captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P->captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P->captureByteData(#name, name)

using namespace kernel;
using namespace llvm;
using namespace codegen;

static cl::OptionCategory ExpandDemoOptions("Expand Demo Options", "Expand demo options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(ExpandDemoOptions));

class ExpandKernel final : public MultiBlockKernel {
public:
    ExpandKernel(KernelBuilder & b, StreamSet * const byteStream, StreamSet * const expandedStream);
    static constexpr unsigned fw = 8;
    static constexpr unsigned inputRate = 1;
    static constexpr unsigned outputRate = 2;
protected:
    void generateMultiBlockLogic(KernelBuilder & b, llvm::Value * const numOfStrides) override;
};

ExpandKernel::ExpandKernel(KernelBuilder & b, StreamSet * const byteStream, StreamSet * const expandedStream)
: MultiBlockKernel(b, "expand_kernel",
{Binding{"byteStream", byteStream, FixedRate(inputRate)}},
    {Binding{"Expanded", expandedStream, FixedRate(outputRate)}}, {}, {}, {}) {}

void ExpandKernel::generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) {
    
    llvm::errs() << "Starting generateMultiBlockLogic\n" ; // for testing

    // Define the field width (fw) as a static constexpr.
    static constexpr unsigned fw = 8; // field width in bits

    // Calculate the number of input packs per stride based on the input rate.
    const unsigned inputPacksPerStride = fw * inputRate;

    // Calculate the number of output packs per stride based on the output rate.
    const unsigned outputPacksPerStride = fw * outputRate;

    // llvm::errs() << "inputPacksPerStride: " << inputPacksPerStride << "\n"; // for testing
    // llvm::errs() << "outputPacksPerStride: " << outputPacksPerStride << "\n"; // for testing

    // Get the current insertion block (entry point).
    BasicBlock * entry = b.GetInsertBlock();

    // Create a new basic block for the expansion loop.
    BasicBlock * expandLoop = b.CreateBasicBlock("expandLoop");

    // Create a new basic block for finalizing the expansion.
    BasicBlock * expandFinalize = b.CreateBasicBlock("expandFinalize");

    // Define a constant value of zero.
    Constant * const ZERO = b.getSize(0);

    // Initialize the number of blocks to process with the given number of strides.
    Value * numOfBlocks = numOfStrides;

    // llvm::errs() << "This is Entry: " << entry << "\n"; // for testing
    // llvm::errs() << "This is ExpandLoop: " << expandLoop << "\n"; // for testing
    // llvm::errs() << "This is expandFinalize: " << expandFinalize << "\n"; // for testing
    // llvm::errs() << "This is ZERO: " << ZERO << "\n"; // for testing
    // llvm::errs() << "Number of blocks to process: " << *numOfBlocks << "\n"; // for testing

    // Adjust the number of blocks if the stride is different from the bit block width.
    if (getStride() != b.getBitBlockWidth()) {
        numOfBlocks = b.CreateShl(numOfStrides, b.getSize(std::log2(getStride() / b.getBitBlockWidth())));
        // llvm::errs() << "stride = " << getStride() << "\n"; // for testing
    }

    // Create a branch to the expansion loop.
    b.CreateBr(expandLoop);
    // Set the insertion point to the expansion loop block.
    b.SetInsertPoint(expandLoop);
    // Create a PHI node to manage the block offset.
    PHINode * blockOffsetPhi = b.CreatePHI(b.getSizeTy(), 2);
    blockOffsetPhi->addIncoming(ZERO, entry);
    
    // Declare an array to hold the input byte packs.
    Value * bytepack[inputPacksPerStride];
    // Load input byte packs from the input stream.
    for (unsigned i = 0; i < inputPacksPerStride; i++) {
        bytepack[i] = b.loadInputStreamPack("byteStream", ZERO, b.getInt32(i), blockOffsetPhi);
        // Print the loaded byte pack for debugging purposes.
        // llvm::errs() << "Loaded bytepack[" << i << "]: " << *bytepack[i] << "\n"; // for testing
    }

    // Declare an array to hold the expanded output packs.
    Value * expanded[outputPacksPerStride];
    // Expand each byte pack using SIMD operations and store them in the output packs.
    for (unsigned i = 0; i < inputPacksPerStride; i++) {
        expanded[2*i] = b.esimd_mergeh(8, bytepack[i], bytepack[i]); // high merge
        expanded[2*i+1] = b.esimd_mergel(8, bytepack[i], bytepack[i]); // low merge

        // converting <16 x i8> to <32 x i16> by bit casting
        /*
        Value * high_i16 = b.CreateBitCast(high, FixedVectorType::get(b.getInt16Ty(), 32));
        Value * low_i16 = b.CreateBitCast(low, FixedVectorType::get(b.getInt16Ty(), 32));

        // bitcasting back to <64 x i8>
        expanded[2*i] = b.CreateBitCast(high_i16, FixedVectorType::get(b.getInt8Ty(), 64));
        expanded[2*i+1] = b.CreateBitCast(low_i16, FixedVectorType::get(b.getInt8Ty(), 64));*/

        // Print the expanded packs for debugging purposes.
        // llvm::errs() << "Expanded[" << 2*i << "]: " << *expanded[2*i] << "\n"; // for testing
        // llvm::errs() << "Expanded[" << 2*i+1 << "]: " << *expanded[2*i+1] << "\n"; // for testing

        // Store the expanded packs in the output stream.
        b.storeOutputStreamPack("Expanded", ZERO, b.getInt32(i), blockOffsetPhi, expanded[2*i]);
        b.storeOutputStreamPack("Expanded", ZERO, b.getInt32(i), blockOffsetPhi, expanded[2*i+1]);
    }
    for (unsigned i = 0; i < inputPacksPerStride; i++) {
        Value * high = b.esimd_mergeh(8, bytepack[i], bytepack[i]); // high merge
        Value * low  = b.esimd_mergel(8, bytepack[i], bytepack[i]); // low merge

        // converting <16 x i8> to <32 x i16> by bit casting
        Value * high_i16 = b.CreateBitCast(high, FixedVectorType::get(b.getInt16Ty(), 32));
        Value * low_i16 = b.CreateBitCast(low, FixedVectorType::get(b.getInt16Ty(), 32));

        // bitcasting back to <64 x i8>
        expanded[2*i] = b.CreateBitCast(high_i16, FixedVectorType::get(b.getInt8Ty(), 64));
        expanded[2*i+1] = b.CreateBitCast(low_i16, FixedVectorType::get(b.getInt8Ty(), 64));

        // Print the expanded packs for debugging purposes.
        // llvm::errs() << "Expanded[" << 2*i << "]: " << *expanded[2*i] << "\n"; // for testing
        // llvm::errs() << "Expanded[" << 2*i+1 << "]: " << *expanded[2*i+1] << "\n"; // for testing

        // Store the expanded packs in the output stream.
        b.storeOutputStreamPack("Expanded", ZERO, b.getInt32(i), blockOffsetPhi, expanded[2*i]);
        b.storeOutputStreamPack("Expanded", ZERO, b.getInt32(i), blockOffsetPhi, expanded[2*i+1]);
    }
    // Calculate the next block offset by adding 1 to the current block offset.
    Value * nextBlk = b.CreateAdd(blockOffsetPhi, b.getSize(1));
    blockOffsetPhi->addIncoming(nextBlk, expandLoop);
    // Compare if there are more blocks to process.
    Value * moreToDo = b.CreateICmpNE(nextBlk, numOfBlocks);

    // Create a conditional branch to either loop back or finalize.
    b.CreateCondBr(moreToDo, expandLoop, expandFinalize);
    // Set the insertion point to the finalization block.
    b.SetInsertPoint(expandFinalize);
}

typedef void (*ExpandDemoFunctionType)(uint32_t fd);

ExpandDemoFunctionType expanddemo_gen (CPUDriver & driver) {

    auto & b = driver.getBuilder();
    auto P = driver.makePipeline({Binding{b.getInt32Ty(), "inputFileDescriptor"}}, {});

    Scalar * fileDescriptor = P->getInputScalar("inputFileDescriptor");

    // Source data
    StreamSet * const codeUnitStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<ReadSourceKernel>(fileDescriptor, codeUnitStream);
    SHOW_BYTES(codeUnitStream);

    StreamSet * const expandedStream = P->CreateStreamSet(1, 8);
    P->CreateKernelCall<ExpandKernel>(codeUnitStream, expandedStream); 
    SHOW_BYTES(expandedStream); 
    SHOW_BIXNUM(expandedStream); 
    SHOW_BYTES(expandedStream);

    P->CreateKernelCall<StdOutKernel>(expandedStream);

    return reinterpret_cast<ExpandDemoFunctionType>(P->compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&ExpandDemoOptions, codegen::codegen_flags()});
    CPUDriver pxDriver("expanddemo");
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
    } else {
        ExpandDemoFunctionType func = nullptr;
        func = expanddemo_gen(pxDriver);
        func(fd);
        close(fd);
    }
    return 0;
}
