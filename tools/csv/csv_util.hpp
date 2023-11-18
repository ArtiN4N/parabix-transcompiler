#include <pablo/pablo_kernel.h>

std::vector<std::string> parse_CSV_headers(std::string headerString);

std::vector<std::string> get_CSV_headers(std::string filename);

std::vector<std::string> JSONfieldPrefixes(std::vector<std::string> fieldNames);

constexpr char charLF = 0xA;
constexpr char charCR = 0xD;
constexpr char charDQ = 0x22;
constexpr char charComma = 0x2C;

namespace kernel {

class CSVlexer : public pablo::PabloKernel {
public:
    CSVlexer(BuilderRef kb, kernel::StreamSet * Source, kernel::StreamSet * CSVlexical)
        : PabloKernel(kb, "CSVlexer",
                      {Binding{"Source", Source}},
                      {Binding{"CSVlexical", CSVlexical, FixedRate(), Add1()}}) {}
protected:
    void generatePabloMethod() override;
};

enum {markLF = 0, markCR = 1, markDQ = 2, markComma = 3, markEOF = 4};


class CSVparser : public pablo::PabloKernel {
public:
    CSVparser(BuilderRef kb, StreamSet * csvMarks, StreamSet * recordSeparators, StreamSet * fieldSeparators, StreamSet * quoteEscape)
        : PabloKernel(kb, "CSVparser",
                      {Binding{"csvMarks", csvMarks, FixedRate(), LookAhead(1)}},
                      {Binding{"recordSeparators", recordSeparators},
                       Binding{"fieldSeparators", fieldSeparators},
                       Binding{"quoteEscape", quoteEscape}}) {}
protected:
    void generatePabloMethod() override;
};


class CSVdataFieldMask : public pablo::PabloKernel {
public:
    CSVdataFieldMask(BuilderRef kb, StreamSet * csvMarks, StreamSet * recordSeparators, StreamSet * quoteEscape, StreamSet * toKeep, bool deleteHeader = true)
        : PabloKernel(kb, "CSVdataFieldMask" + std::to_string(deleteHeader),
                      {Binding{"csvMarks", csvMarks, FixedRate(), LookAhead(1)},
                       Binding{"recordSeparators", recordSeparators},
                       Binding{"quoteEscape", quoteEscape}},
                      {Binding{"toKeep", toKeep}})
    , mDeleteHeader(deleteHeader) {}
protected:
    void generatePabloMethod() override;
    bool mDeleteHeader;
};

//
//  FieldNumberingKernel(N) 
//  two input streams: record marks, field marks, N fields per record
//  output: at the start position after each mark, a bixnum value equal to the
//          sequential field number (counting from 1 at each record start).
//

class FieldNumberingKernel : public pablo::PabloKernel {
public:
    FieldNumberingKernel(BuilderRef kb, StreamSet * SeparatorNum, StreamSet * RecordMarks, StreamSet * FieldBixNum);
protected:
    void generatePabloMethod() override;
    unsigned mNumberingBits;
};



class CSV_Char_Replacement : public pablo::PabloKernel {
public:
    CSV_Char_Replacement(BuilderRef kb, StreamSet * quoteEscape, StreamSet * basis,
                         StreamSet * translatedBasis)
        : PabloKernel(kb, "CSV_Char_Replacement",
                      {Binding{"quoteEscape", quoteEscape}, Binding{"basis", basis}},
                      {Binding{"translatedBasis", translatedBasis}}) {}
protected:
    void generatePabloMethod() override;
};


class AddFieldSuffix : public pablo::PabloKernel {
public:
    AddFieldSuffix(BuilderRef kb, StreamSet * suffixSpreadMask, StreamSet * basis,
                         StreamSet * updatedBasis)
        : PabloKernel(kb, "AddFieldSuffix",
                      {Binding{"suffixSpreadMask", suffixSpreadMask}, Binding{"basis", basis}},
                      {Binding{"updatedBasis", updatedBasis}}) {}
protected:
    void generatePabloMethod() override;
};

}
