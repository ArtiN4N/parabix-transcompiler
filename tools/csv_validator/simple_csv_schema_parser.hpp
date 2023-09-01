/*
 *  Copyright (c) 2018 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef SIMPLE_CSV_SCHEMA_PARSER_H
#define SIMPLE_CSV_SCHEMA_PARSER_H

#include <re/parse/parser.h>

namespace re {
    class SimpleCSVSchemaParser : public RE_Parser {
    public:
        SimpleCSVSchemaParser(const std::string & regular_expression) : RE_Parser(regular_expression) {
            mReSyntax = RE_Syntax::PCRE;
        }

        re::RE * parse() {
            return parse_RE();
        }

    protected:
        virtual bool isSetEscapeChar(char c) override;
    };
}

#endif
