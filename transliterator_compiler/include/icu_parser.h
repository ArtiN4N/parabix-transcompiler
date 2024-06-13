#ifndef ICU_PARSER_H
#define ICU_PARSER_H

#include <map>

using namespace std;

class ICUParser {
public:
    map<char, char> parseUppercaseToLowercase();
};

#endif // ICU_PARSER_H