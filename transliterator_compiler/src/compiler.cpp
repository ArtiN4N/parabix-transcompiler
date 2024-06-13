#include "compiler.h"
#include "icu_parser.h"
#include "kernel_calls.h"

using namespace std;

void TransliteratorCompiler::compileAndTransform(string &input) {
    ICUParser parser;
    auto transformMap = parser.parseUppercaseToLowercase();
    
    transformUppercaseToLowercase(input);
}