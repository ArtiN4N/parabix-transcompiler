#include "compiler.h"
#include <iostream>
#include <vector>
#include <string>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <commands>" << std::endl;
        return 1;
    }

    std::string commandStr = argv[1];
    std::vector<std::string> commands;
    size_t pos = 0;
    while ((pos = commandStr.find(';')) != std::string::npos) {
        commands.push_back(commandStr.substr(0, pos));
        commandStr.erase(0, pos + 1);
    }

    compileAndTransform(commands);

    return 0;
}
