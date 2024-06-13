#include "icu_parser.h"

using namespace std;

map<char, char> ICUParser::parseUppercaseToLowercase() {
    map<char, char> transformMap;
    for (char c = 'A'; c <= 'Z'; ++c) {
        transformMap[c] = c + ('a' - 'A');
    }
    return transformMap;
}