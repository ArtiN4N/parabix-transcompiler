#include "compiler.h"
#include <iostream>

using namespace std;

int main() {
    string input = "HELLO WORLD!";
    cout << "This is the original input: " << input << endl;
    TransliteratorCompiler compiler;
    compiler.compileAndTransform(input);
    cout << "Transformed Text: " << input << endl;
    return 0;
}