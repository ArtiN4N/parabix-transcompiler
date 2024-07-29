#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <sstream>


void exitTransCompiler() {
    std::cout << std::endl;
    exit(0);
}

std::string LDMLvalidTransformsText() {
    std::string ret = "NULL - does nothing\n";
    return ret;
}

void printFormatAndExit() {
    std::cout << "This program is a transcompiler, which takes LDML transforms as an input, and applies them to a text source file\n"
              << "How to use: ./parabix_transcompiler sourcefile.txt transform1 transform2 ...\n"
              << "This program can take an indefinite number of transforms\n"
              << "you can call this program with '-h' as an argument to display the eligible transforms and what they do\n"
              << "you can call this program with '-li' followed by an text input file to optionally source LDML transforms from a file instead of the command line\n"
              << "sourcing LDML transforms requires each transform to be seperated by a semi-colon character (;)\n"
              << "you can call this program with '-o' followed by an text output file to optionally store the transformed source text into a file\n";
    exitTransCompiler();
}

void printHelpAndExit() {
    std::cout << "This program is a transcompiler, which takes LDML transforms as an input, and applies them to a text source file\n"
              << "How to use: ./parabix_transcompiler sourcefile.txt transform1 transform2 ...\n"
              << "This program can take an indefinite number of transforms\n"
              << "you can call this program with '-h' as an argument to display the eligible transforms and what they do\n"
              << "you can call this program with '-li' followed by an text input file to optionally source LDML transforms from a file instead of the command line\n"
              << "sourcing LDML transforms requires each transform to be seperated by a semi-colon character (;)\n"
              << "you can call this program with '-o' followed by an text output file to optionally store the transformed source text into a file\n"
              << "Below are the valid LDML transforms you can specify:\n\n"
              << LDMLvalidTransformsText();
    exitTransCompiler();
}

void getLDMLfrom(std::string src, std::vector<std::string>& transform) {
    std::ifstream file(src);
    if (!file.is_open()) {
        std::cerr << "Unable to open file: " << src << std::endl;
        exitTransCompiler(); // Exit the program with an error code
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

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

int main(int argc, char* argv[]) {
    std::vector<std::string> LMDLtransforms;

    bool readTextSrc = false;
    std::string textInputSrc;

    bool readLDMLFromFile = false;
    std::string LMDLsrc;

    bool outputToFile = false;
    std::string LDMLout;

    std::cout << std::endl;

    if (argc == 1) printFormatAndExit();

    // Take an unlimited amount of input, with each being a transform
    // However, if the '-li' flag is used, ignore transform input, and take from an input file
    // If the '-o' flag is used, instead of outputting to the command line, output to the specified file
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
            LDMLout = argv[i];
            continue;
        }

        if (!readTextSrc) {
            textInputSrc = arg;
            readTextSrc = true;
            continue;
        }

        if (readLDMLFromFile) printFormatAndExit();

        LMDLtransforms.push_back(arg);
    }

    if (readLDMLFromFile) getLDMLfrom(LMDLsrc, LMDLtransforms);

    std::cout << "Given transforms:" << std::endl;
    for (auto transform : LMDLtransforms) {
        std::cout << transform << std::endl;
    }

    if (outputToFile) {
        std::cout << "Writing transformed input to file: " << LDMLout << std::endl;
    }

    std::cout << std::endl;

    return 0;
}