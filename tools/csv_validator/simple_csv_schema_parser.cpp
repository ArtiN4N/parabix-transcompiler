/*
 *  Copyright (c) 2018 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#include "simple_csv_schema_parser.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <fstream>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>
#include <boost/container/flat_set.hpp>
#include <re/parse/PCRE_parser.h>
#include <re/printer/re_printer.h>

#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace boost;

#define BEGIN_SCOPED_REGION {
#define END_SCOPED_REGION }

namespace csv {

// https://digital-preservation.github.io/csv-schema/csv-schema-1.1.html

CSVSchema CSVSchemaParser::load(const llvm::StringRef fileName) {

    CSVSchema schema;

    std::ifstream file(fileName.str());
    if (file.is_open()) {
        std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        auto cur = data.cbegin();
        const auto end = data.cend();

        auto skipSpace = [&]() {
            while (cur != end && std::isspace(*cur)) {
                ++cur;
            }
        };

        auto skipSpaceUntilNewLine = [&]() -> bool {
            while (cur != end) {
                const auto c = *cur;
                if (c == '\n') {
                    ++cur;
                    return true;
                } else if (c == '\r') {
                    if ((cur != end) && (*cur == '\n')) {
                        ++cur;
                    }
                    return true;
                } else if (!std::isspace(c)) {
                    break;
                }
                ++cur;
            }
            return false;
        };



        auto skipSpaceOrComments = [&]() {
            while (cur != end) {
                if (std::isspace(*cur)) {
                     ++cur;
                } else if (LLVM_UNLIKELY(*cur == '/')) {
                    auto p = cur + 1;
                    if (*p == '/') {
                        // in single line comment. parse until new line
                        while (++p != end) {
                            const auto c = *p;
                            if (LLVM_UNLIKELY(c == '\r')) {
                                if ((++p != end) && (*p == '\n')) {
                                    ++p;
                                }
                                break;
                            } else if (LLVM_UNLIKELY(c == '\n')) {
                                ++p;
                                break;
                            }
                        }
                        cur = p;
                    } else if (*p == '*') {
                        // in multi-line comment; parse until */
                        while (++p != end) {
                            if (LLVM_UNLIKELY(*p == '*')) {
                                if (++p != end) {
                                    if (LLVM_LIKELY(*p == '/')) {
                                        cur = p + 1;
                                        break;
                                    }
                                }
                            }
                        }
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
        };

        auto reportParsingFailure = [&](const StringRef msg) {
            // TODO: report position
            llvm::report_fatal_error(msg);
        };

        auto matchChar = [&](const char ch) {
            if (LLVM_UNLIKELY(cur == end)) return false;
            if (*cur == ch) {
                ++cur;
                return true;
            }
            return false;
        };

        auto requireChar = [&](const char ch, StringRef onFailMessage) {
            if (LLVM_UNLIKELY(!matchChar(ch))) {
                reportParsingFailure(onFailMessage);
            }
        };

        auto matchString = [&](const StringRef str) {
            const auto l = str.size();
            if (std::distance(cur, end) < l) {
                return false;
            }
            auto p = cur;
            for (size_t i = 0; i < l; ++i) {
                if (*p != str[i]) {
                    return false;
                }
                ++p;
            }
            cur += l;
            return true;
        };

        auto requireString = [&](const StringRef str, StringRef onFailMessage) {
            if (LLVM_UNLIKELY(!matchString(str))) {
                reportParsingFailure(onFailMessage);
            }
        };

        auto parseInt = [&](size_t & val) {
            if (LLVM_LIKELY(cur != end)) {
                size_t v = *cur;
                if (LLVM_LIKELY(std::isdigit(v))) {
                    v -= '0';
                    assert (v >= 0 && v < 10);
                    while (LLVM_LIKELY(++cur != end)) {
                        const auto c = *cur;
                        if (std::isdigit(c)) {
                            v = (v * 10) + (c - '0');
                        } else {
                            break;
                        }
                    }
                }
                val = v;
            } else {
                reportParsingFailure("Expected Int value");
            }
        };

        SmallVector<char, 256> string;

        auto parseString = [&]() -> StringRef {
            string.clear();
            raw_svector_ostream tmp(string);
            while (LLVM_LIKELY(cur != end)) {
                const auto c = *cur;
                if (std::isspace(c)) {
                    break;
                }
                tmp << c;
                ++cur;
            }
            return tmp.str();
        };

        // Schema	::=	Prolog Body

        // Prolog	::=	VersionDecl GlobalDirectives

        // VersionDecl	::=	("version 1.0" | "version 1.1")

        size_t major, minor;

        skipSpace();
        requireString("version", "Expected version number");
        skipSpace();
        parseInt(major);
        requireChar('.', "Expected version number");
        parseInt(minor);

        if (major != 1 && minor != 0) {
            // TODO: look into 1.1 and 1.2 later
            return schema;
        }

        //	GlobalDirectives	::=	SeparatorDirective? QuotedDirective? TotalColumnsDirective?
        //                          PermitEmptyDirective? (NoHeaderDirective | IgnoreColumnNameCaseDirective)?
        //	DirectivePrefix	::=	"@"

        // [6]	SeparatorDirective	::=	DirectivePrefix "separator" (SeparatorTabExpr | SeparatorChar)
        // [7]	SeparatorTabExpr	::=	"TAB" | '\t'
        // [8]	SeparatorChar	::=	CharacterLiteral
        //  CharacterLiteral	::=	"'" [^\r\n\f'] "'"

        // [9]	QuotedDirective	::=	DirectivePrefix "quoted"

        // [10]	TotalColumnsDirective	::=	DirectivePrefix "totalColumns" PositiveNonZeroIntegerLiteral
        //      PositiveNonZeroIntegerLiteral	::=	[1-9][0-9]*

        // [11]	PermitEmptyDirective	::=	DirectivePrefix "permitEmpty"

        // [12]	NoHeaderDirective	::=	DirectivePrefix "noHeader"

        // [13]	IgnoreColumnNameCaseDirective	::=	DirectivePrefix "ignoreColumnNameCase"

        for (;;) {
            skipSpace();
            //	DirectivePrefix	::=	"@"
            if (matchChar('@')) {
                if (matchString("separator")) {
                    skipSpace();
                    StringRef sep = parseString();
                    if (LLVM_LIKELY(sep.size() == 3)) {
                        if (sep[0] == '\'' && sep[2] == '\'') {
                            schema.Separator = sep[1];
                        } else if (sep == "TAB") {
                            schema.Separator = '\t';
                        } else {
                            reportParsingFailure("Invalid separator");
                        }
                    } else if (sep.size() == 2 && sep == "\\t") {
                        schema.Separator = '\t';
                    } else {
                        reportParsingFailure("Invalid separator");
                    }
                } else if (matchString("quoted")) {
                    schema.Quoted = true;
                } else if (matchString("totalColumns")) {
                    skipSpace();
                    parseInt(schema.TotalColumns);
                } else if (matchString("permitEmpty")) {
                    schema.PermitEmpty = true;
                } else if (matchString("noHeader")) {
                    schema.NoHeader = true;
                } else if (matchString("ignoreColumnNameCase")) {
                    schema.IgnoreColumnNameCase = true;
                } else {
                     reportParsingFailure("Invalid global directive");
                }
            } else {
                break;
            }
        }

        // [14]	Body	::=	BodyPart+
        // [15]	BodyPart	::=	Comment* ColumnDefinition Comment*

        std::map<std::string, size_t> M;

        auto findOrAddKey = [&](const std::string & name) -> size_t {
            auto f = M.find(name);
            if (f != M.end()) {
                return f->second;
            } else {
                const auto v = M.size();
                M.emplace(std::string{name}, M.size());
                return v;
            }
        };

        for (;;) {

            skipSpaceOrComments();

            if (LLVM_UNLIKELY(cur == end)) {
                break;
            }

            // [19]	ColumnDefinition	::=	(ColumnIdentifier | QuotedColumnIdentifier) ":" ColumnRule

            // [20]	ColumnIdentifier	::=	PositiveNonZeroIntegerLiteral | Ident
            // [21]	QuotedColumnIdentifier	::=	StringLiteral

            // StringLiteral	::=	"\"" [^"]* "\""

            auto parseStringLiteral = [&]() -> StringRef {
                string.clear();
                raw_svector_ostream tmp(string);
                if (*cur == '"') {
                    // StringLiteral	::=	"\"" [^"]* "\""
                    for (;;) {
                        assert (cur != end);
                        if (++cur == end) {
                            reportParsingFailure("unterminated quoted identifier");
                        }
                        // string literal doesn't allow for any double quotes? modified rule
                        // to permit escaped characters.
                        if (*cur == '\\') {
                            auto p = cur + 1;
                            if (*p == '"') {
                               cur = p;
                            }
                        } else if (*cur == '"') {
                            ++cur;
                            break;
                        }
                        tmp << *cur;
                    }
                }

                return tmp.str();
            };

            auto parseQuotedOrNonQuotedColumnIdentifier = [&]() -> StringRef {
                if (cur == end) {
                    reportParsingFailure("expected positive number of identifier");
                }
                if (*cur == '"') {
                    return parseStringLiteral();
                } else {
                    // PositiveNonZeroIntegerLiteral	::=	[1-9][0-9]*
                    // Ident	::=	[A-Za-z0-9\-_\.]+
                    string.clear();
                    raw_svector_ostream tmp(string);
                    const auto start = cur;
                    for (;;) {
                        const char c = *cur;
                        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
                            tmp << c;
                            ++cur;
                        } else {
                            break;
                        }
                    }

                    if (cur == start) {
                        reportParsingFailure("identifier expected");
                    }

                    return tmp.str();
                }

            };

            auto name = parseQuotedOrNonQuotedColumnIdentifier();

            for (unsigned i = 0; i < schema.Column.size(); ++i) {
                if (LLVM_UNLIKELY(name.equals(StringRef{schema.Column[i].Name}))) {
                    reportParsingFailure("duplicate column name");
                }
            }

            CSVSchemaColumnRule rule;
            rule.Name = name.str();

            // ColumnRule	::=	ColumnValidationExpr* ColumnDirectives

            // ColumnValidationExpr	::=	CombinatorialExpr | NonCombinatorialExpr
            // ...
            // NonConditionalExpr	::=	SingleExpr | ExternalSingleExpr | ParenthesizedExpr


            // SingleExpr	::=	... (... RegExpExpr | ... UniqueExpr | ... | Uuid4Expr | ...)

            bool hasValidationRule = false;

            auto readColumnValidationExprName = [&]() {
                string.clear();
                raw_svector_ostream tmp(string);
                const auto start = cur;
                for (;;) {
                    if (cur == end) {
                        break;
                    }
                    if (std::isalpha(*cur)) {
                        tmp << *cur++;
                    } else {
                        break;
                    }
                }
                if (cur == start) {
                    reportParsingFailure("expression type expected");
                }
                return tmp.str();
            };

            skipSpace();
            requireChar(':', "expected :");

            // White space is not generally important within a Column Rule, but the whole rule must be on a single line.

            for (;;) {
                if (skipSpaceUntilNewLine()) {
                    goto done_parsing_column_rule;
                }
                if (matchChar('@')) {
                    goto parse_column_directives;
                }

                const StringRef exprName = readColumnValidationExprName();
                if (exprName.equals("regex")) {

                    // 	RegExpExpr	::=	"regex(" StringLiteral ")"

                    // A Regular Expression Expression checks the value of the column against the supplied Regular Expression.

                    skipSpaceUntilNewLine();
                    requireChar('(', "expected (");
                    skipSpaceUntilNewLine();
                    const auto re = parseStringLiteral();
                    rule.Expression = re::RE_Parser::parse(re.str());
//                    errs() << "INPUT RE:\n\n" << re << ":\n\n"
//                           << Printer_RE::PrintRE(rule.Expression) << "\n\n\n";
                    skipSpace();
                    requireChar(')', "expected )");
                } else if (exprName.equals("unique")) {
                    // 	UniqueExpr	::=	"unique" ("(" ColumnRef ("," ColumnRef)* ")")?
                    // [36]	ColumnRef	::=	"$" (ColumnIdentifier | QuotedColumnIdentifier)

                    // A Unique Expression checks that the column value is unique within the CSV file being validated
                    // (within the current column, the value may occur elsewhere in the file in another column, as in
                    // a primary-foreign key relationship in a database). You can also specify a comma separated list
                    // of Column References in which case the combination of values of those columns (for the current
                    // row) must be unique within the whole CSV file.

                    // NOTE: we can technically permit more than one unique list for a key, and those could both
                    // overlap but not be a subset of each other.

                    const auto u = findOrAddKey(rule.Name);

                    CSVSchemaCompositeKey key;
                    auto & fields = key.Fields;
                    fields.push_back(u);

                    skipSpaceUntilNewLine();

                    if (matchChar('(')) {
                        for (;;) {
                            skipSpaceUntilNewLine();
                            requireChar('$', "expected $ before column identifier");
                            auto name = parseQuotedOrNonQuotedColumnIdentifier();
                            const auto v = findOrAddKey(name.str());
                            fields.push_back(v);
                            skipSpaceUntilNewLine();
                            if (matchChar(')')) {
                                break;
                            } else {
                                requireChar(',', "expected , or )");
                            }
                        }

                        std::sort(fields.begin(), fields.end());
                        fields.erase(std::unique( fields.begin(), fields.end()), fields.end() );
                    }

                    schema.CompositeKey.emplace_back(key);
                } else {
                    break;
                }
            }

            // ColumnDirectives	::=	OptionalDirective? MatchIsFalseDirective? IgnoreCaseDirective? WarningDirective?	/* xgc:unordered */

            for (;;) {
                skipSpaceUntilNewLine();
                if (matchChar('@')) {
parse_column_directives:
                    if (matchString("optional")) {
                        rule.Optional = true;
                    } else if (matchString("matchIsFalse")) {
                        rule.MatchIsFalse = true;
                    } else if (matchString("ignoreCase")) {
                        rule.IgnoreCase = true;
                    } else if (matchString("warning")) {
                        schema.AnyWarnings = true;
                        rule.Warning = true;
                    } else {
                        reportParsingFailure("unknown column directive");
                    }
                } else {
                    break;
                }
            }
done_parsing_column_rule:
            schema.Column.emplace_back(rule);
        }

        if (LLVM_UNLIKELY(!M.empty())) {
            std::vector<size_t> L(M.size());
            for (auto p : M) {
                #ifndef NDEBUG
                bool found = false;
                #endif
                for (unsigned i = 0; i < schema.Column.size(); ++i) {
                    if (schema.Column[i].Name.compare(p.first) == 0) {
                        L[p.second] = i;
                        #ifndef NDEBUG
                        found = true;
                        #endif
                        break;
                    }
                }
            }
            for (CSVSchemaCompositeKey & key : schema.CompositeKey) {
                for (auto & k : key.Fields) {
                    k = L[k];
                }
                std::sort(key.Fields.begin(), key.Fields.end());
            }
        }

        if (schema.TotalColumns == 0) {
            schema.TotalColumns = schema.Column.size();
        } else if (schema.TotalColumns != schema.Column.size()) {
            report_fatal_error("Total columns does not match number of column rules");
        }
    } else {
        report_fatal_error("Cannot open " + fileName);
    }

    return schema;
}




}
