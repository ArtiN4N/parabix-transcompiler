#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <array>
#include <regex>
#include <filesystem>


#include "validcode.h"


void toLowercase(std::string& str) {
    std::transform(str.begin(), str.end(), str.begin(),
                   [](unsigned char c) { return std::tolower(c); });
}

void exitTransCompiler() {
    std::cout << "Use the '-h' flag to see more information about using this compiler\n" << std::endl;
    exit(0);
}

std::string LDMLvalidTransformsText() {
    std::string ret = "NULL - does nothing\n";
    return ret;
}

void printFormatAndExit() {
    std::cout << "This program is a transcompiler, which takes LDML transforms as an input, and creates an executable that applies those transforms to an input file\n"
              << "How to use: ./parabix_transcompiler transform1 transform2 ...\n"
              << "This program can take an indefinite number of transforms\n"
              << "you can call this program with '-h' as an argument to display the eligible transforms and what they do\n"
              << "you can call this program with '-li' followed by an text input file to optionally source LDML transforms from a file instead of the command line\n"
              << "sourcing LDML transforms requires each transform to be seperated by a semi-colon character (;)\n"
              << "you can call this program with '-o' followed by an text output file to optionally store the transformed source text by the compiled program into a file\n"
              << "you can call this program with '-n' followed by name to optionally use a custom name for the compiled program\n";
    exitTransCompiler();
}

void printHelpAndExit() {
    std::cout << "This program is a transcompiler, which takes LDML transforms as an input, and creates an executable that applies those transforms to an input file\n"
              << "How to use: ./parabix_transcompiler transform1 transform2 ...\n"
              << "This program can take an indefinite number of transforms\n"
              << "you can call this program with '-h' as an argument to display the eligible transforms and what they do\n"
              << "you can call this program with '-li' followed by an text input file to optionally source LDML transforms from a file instead of the command line\n"
              << "sourcing LDML transforms requires each transform to be seperated by a semi-colon character (;)\n"
              << "you can call this program with '-o' followed by an text output file to optionally store the transformed source text by the compiled program into a file\n"
              << "you can call this program with '-n' followed by name to optionally use a custom name for the compiled program\n"
              << "Below are the valid LDML transforms you can specify:\n\n"
              << LDMLvalidTransformsText();
    exitTransCompiler();
}

void getLDMLfrom(std::string src, std::vector<std::string>& transform) {
    //, std::ios::in | std::ios::binary
    std::ifstream file(src);
    if (!file.is_open()) {
        std::cerr << "Unable to open file: " << src << std::endl;
        exitTransCompiler(); // Exit the program with an error code
    }
    std::string content;
    std::string text;
    while (getline(file,text)) {
        content += text;
    }

    // Remove spaces and newlines
    content.erase(std::remove(content.begin(), content.end(), ' '), content.end());
    content.erase(std::remove(content.begin(), content.end(), '\n'), content.end());

    // Split by cemi-colons and pack into transform
    std::stringstream ss(content);
    std::string item;

    while (getline(ss, item, ';')) {
        transform.push_back(item);
    }
}

enum LDMLtransformEnum {
    NULL_T = 0,
    FULLHALF_T,
    HALFFULL_T,
    LASCII_T,
    LOWER_T,
    UPPER_T,
    TITLE_T,
    REMOVE_T
};

struct LDMLtransformSet {
    std::vector<LDMLtransformEnum> transforms;
    std::array<int, 8> transformUses;
    std::vector<std::string> removeRegex;

    LDMLtransformSet();
};

LDMLtransformSet::LDMLtransformSet() {
    for (int i = 0; i < transformUses.size(); i++) transformUses[i] = 0;
}

LDMLtransformSet validateTransforms(std::vector<std::string> transforms) {
    LDMLtransformSet ret;
    ret = LDMLtransformSet();

    for (auto transform : transforms) {
        toLowercase(transform);

        if (transform == "null")
            ret.transforms.push_back(LDMLtransformEnum::NULL_T);
        else if (transform == "fullwidth-halfwidth")
            ret.transforms.push_back(LDMLtransformEnum::FULLHALF_T);
        else if (transform == "halfwidth-fullwidth")
            ret.transforms.push_back(LDMLtransformEnum::HALFFULL_T);
        else if (transform == "latin-ascii")
            ret.transforms.push_back(LDMLtransformEnum::LASCII_T);
        else if (transform == "lower")
            ret.transforms.push_back(LDMLtransformEnum::LOWER_T);
        else if (transform == "upper")
            ret.transforms.push_back(LDMLtransformEnum::UPPER_T);
        else if (transform == "title")
            ret.transforms.push_back(LDMLtransformEnum::TITLE_T);
        else if (transform.substr(0, 6)== "remove") {
            ret.transforms.push_back(LDMLtransformEnum::REMOVE_T);

            std::string regexPat = transform.substr(6);
            ret.removeRegex.push_back(regexPat);
        } else {
            std::cerr << transform << " is not a valid LDML transform" << std::endl;
            exitTransCompiler();
        }
    }
    
    std::cout << std::endl << "The provided LDML transforms are valid!" << std::endl;

    return ret;
}

std::string getKernelFnName(LDMLtransformEnum transform) {
    if (transform == LDMLtransformEnum::FULLHALF_T)
        return "doFullHalfTransform";
    else if (transform == LDMLtransformEnum::HALFFULL_T)
        return "doHalfFullTransform";
    else if (transform == LDMLtransformEnum::LASCII_T)
        return "ReplaceByBixData";
    else if (transform == LDMLtransformEnum::LOWER_T)
        return "doLowerTransform";
    else if (transform == LDMLtransformEnum::UPPER_T)
        return "doUpperTransform";
    else if (transform == LDMLtransformEnum::TITLE_T)
        return "doTitleTransform";
    else if (transform == LDMLtransformEnum::REMOVE_T)
        return "doRemoveTransform";
    else return "null";
}

void codeGenError() {
    std::cerr << "The parabix code could not be generated" << std::endl;
        exitTransCompiler();
}

std::string createPipelineFrom(LDMLtransformSet transformSet, bool outputToFile, std::string transformedOut) {
    std::string ret = "";
    
    std::string codeBegin;
    std::string codePipelineBegin;
    std::string codePipelineDynamic;
    std::string codePipelineEnd;
    std::string codeEnd;
    
    codeBegin += ValidCode::includes;
    codePipelineEnd += ValidCode::pipelineOutputBasisDefine;

    int i = 0;
    int regexI = 0;
    int size = transformSet.transforms.size();
    bool addedTransform = false;
    for (auto transform : transformSet.transforms) {
        if (transform != LDMLtransformEnum::NULL_T) addedTransform = true;
        else {
            size--;
            continue;
        };

        std::string input = "U21";
        if (i > 0) input = "finalBasis" + std::to_string(i);

        std::string fnName = getKernelFnName(transform);

        codePipelineDynamic += "    StreamSet * finalBasis" + std::to_string(i + 1) + " = P->CreateStreamSet(21, 1);\n";

        if (transform == LDMLtransformEnum::LASCII_T) {
            if (transformSet.transformUses[transform] == 0) codeBegin += ValidCode::lasciiDataInclude;
            codePipelineDynamic += "    replace_bixData LAT_replace_data(asciiCodeData);\n";
            codePipelineDynamic += "    ReplaceByBixData(P, LAT_replace_data, " + input + ", finalBasis" + std::to_string(i + 1) + ");\n";
        } else if (transform == LDMLtransformEnum::REMOVE_T) {
            codePipelineDynamic += "    " + fnName + "(P, \"" + transformSet.removeRegex[regexI] + "\", " + input + ", finalBasis" + std::to_string(i + 1) + ");\n";
            regexI++;
        } else {
            codePipelineDynamic += "    " + fnName + "(P, " + input + ", finalBasis" + std::to_string(i + 1) + ");\n";
        }

        transformSet.transformUses[transform] += 1;
        i++;
    }

    codePipelineEnd += "    U21_to_UTF8(P, finalBasis" + std::to_string(i) + ", OutputBasis);";

    codeBegin += ValidCode::beginBoiler;

    if (!addedTransform) {
        codePipelineBegin += ValidCode::pipelineBeginBoilerOnNull;
        codePipelineEnd += "    U21_to_UTF8(P, finalBasis1, OutputBasis);";
    } else
        codePipelineBegin += ValidCode::pipelineBeginBoiler;
    
    codePipelineEnd += ValidCode::pipelineEndBoiler;
    codeEnd += ValidCode::endBoiler;

    std::cout << std::endl << "Successfully created parabix code!" << std::endl;

    ret += codeBegin + codePipelineBegin + codePipelineDynamic + codePipelineEnd + codeEnd;
    std::cout << std::endl << "Here is the generated code:" << std::endl << ret << std::endl;
    return ret;
}

fs::path getExecutablePath() {
    char buffer[1024];
    ssize_t count = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (count != -1) {
        buffer[count] = '\0';
        return fs::path(buffer).remove_filename();
    } else {
        throw std::runtime_error("Failed to get executable path");
    }
}

void createCMakeLists(const std::string& cmakeListFile, const std::string& programName, const std::string& sourceFile) {
    std::ofstream cmakeFile(cmakeListFile);
    if (cmakeFile.is_open()) {
        cmakeFile << "parabix_add_executable(\n"
                  << "NAME\n"
                  << "    " << programName << "\n"
                  << "SRC\n"
                  << "    " << sourceFile << "\n"
                  << "DEPS\n"
                  << "    grep\n"
                  << "    pablo\n"
                  << "    kernel.basis\n"
                  << "    kernel.io\n"
                  << "    kernel.pipeline\n"
                  << "    kernel.streamutils\n"
                  << "    kernel.util\n"
                  << "    re.cc\n"
                  << "    toolchain\n"
                  << ")";
        cmakeFile.close();
    } else {
        std::cerr << "Unable to open CMakeLists.txt file.";
        exit(1);
    }
}

void runCommand(const std::string& command) {
    int result = system(command.c_str());
    if (result != 0) {
        std::cerr << "Command failed: " << command << std::endl;
        exit(1);
    }
}

std::string compilePipeline(std::string piplineCode, bool usesCustomProgName, std::string customProgName) {
    std::filesystem::path exePath = getExecutablePath();
    // Change the current working directory to the directory of the executable
    std::filesystem::current_path(exePath);

    std::string codeFileName = "../../transcompiler/transforms/TRANSCOMPILERAUTOGENTEMPSOURCECODE.cpp";
    std::string makeName = "TRANSCOMPILERAUTOGEN";
    if (usesCustomProgName) makeName = customProgName;

    std::ofstream outFile(codeFileName);
    if (!outFile.is_open()) {
        std::cerr << "Error: Could not open file " << codeFileName << std::endl;
        exitTransCompiler();
    }

    outFile << sourceCode;
    outFile.close();

    createCMakeLists("../../transcompiler/transforms/CMakeLists.txt", makeName, codeFileName);

    runCommand("../cmake ..");

    runCommand("../../transcompiler/bash bash maketarget.sh " + makeName);

    return makeName;
}

int main(int argc, char* argv[]) {
    std::vector<std::string> LMDLtransforms;

    bool readLDMLFromFile = false;
    std::string LMDLsrc;

    bool outputToFile = false;
    std::string transformedOut;

    bool usesCustomProgName = false;
    std::string customProgName;

    std::cout << std::endl;

    if (argc == 1) printFormatAndExit();

    // Take an unlimited amount of input, with each being a transform
    // However, if the '-li' flag is used, ignore transform input, and take from an input file
    // If the '-o' flag is used, instead of outputting to the command line, output to the specified file
    // If the '-n' flag is used, the compiled program will use a user specified name
    // If the '-h' flag is used, show the user how to use the compiler, and abort
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h") printHelpAndExit();

        if (arg == "-li") {
            if (i + 1 >= argc) printFormatAndExit();
            readLDMLFromFile = true;
            i++;
            LMDLsrc = argv[i];
            continue;
        }

        if (arg == "-o") {
            if (i + 1 >= argc) printFormatAndExit();
            outputToFile = true;
            i++;
            transformedOut = argv[i];
            continue;
        }

        if (arg == "-n") {
            if (i + 1 >= argc) printFormatAndExit();
            usesCustomProgName = true;
            i++;
            customProgName = argv[i];
            continue;
        }

        if (readLDMLFromFile) printFormatAndExit();

        LMDLtransforms.push_back(arg);
    }

    if (readLDMLFromFile)
        getLDMLfrom(LMDLsrc, LMDLtransforms);

    LDMLtransformSet validTransforms = validateTransforms(LMDLtransforms);
    
    std::string piplineCode = createPipelineFrom(validTransforms, outputToFile, transformedOut);
    std::string compiledProgFilename = compilePipeline(piplineCode, usesCustomProgName, customProgName);

    std::cout << std::endl << compiledProgFilename << " compiled successfully!" << std::endl;

    return 0;
}