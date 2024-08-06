#include "compiler.h"
#include "transform_kernel.h"
#include <kernel/io/stdout_kernel.h>
#include <map>
#include <iostream>

using namespace kernel;

typedef void (*TransformFunction)(KernelBuilder &, StreamSet *, StreamSet *);

std::map<std::string, TransformFunction> transformMap = {
    {"remove_whitespace", remove_whitespace},
    {"nfd", nfd},
    {"nfc", nfc}
};

void compileAndTransform(const std::vector<std::string> &commands) {
    CPUDriver driver("TransliteratorCompiler");
    auto &b = driver.getBuilder();
    auto P = driver.makePipeline({}, {});

    StreamSet *input = P->CreateStreamSet(1, 8);
    StreamSet *output = P->CreateStreamSet(1, 8);

    for (const auto &command : commands) {
        if (transformMap.find(command) != transformMap.end()) {
            transformMap[command](b, input, output);
            input = output;
            output = P->CreateStreamSet(1, 8);
        } else {
            std::cerr << "Unknown command: " << command << std::endl;
        }
    }

    P->CreateKernelCall<StdOutKernel>(output);
    auto fn = reinterpret_cast<void (*)(const uint8_t *, uint8_t *)>(P->compile());

    std::vector<uint8_t> inputData; // Populate this with actual input data
    std::vector<uint8_t> outputData(inputData.size());

    fn(inputData.data(), outputData.data());

    for (auto byte : outputData) {
        std::cout << byte;
    }
}
