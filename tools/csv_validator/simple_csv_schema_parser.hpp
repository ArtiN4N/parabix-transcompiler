/*
 *  Copyright (c) 2018 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef SIMPLE_CSV_SCHEMA_PARSER_H
#define SIMPLE_CSV_SCHEMA_PARSER_H

#include "csv_schema_validator.h"

namespace csv {

    class CSVSchemaParser {
    public:
        static CSVSchema load(const llvm::StringRef fileName);
    };
}

#endif
