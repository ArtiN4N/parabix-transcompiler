#ifndef CSV_VALIDATOR_UTIL_H
#define CSV_VALIDATOR_UTIL_H

#include <pablo/pablo_kernel.h>

namespace kernel {

class CSVDataLexer : public pablo::PabloKernel {
public:
    CSVDataLexer(BuilderRef kb, StreamSet * Source, StreamSet * CSVlexical)
        : PabloKernel(kb, "CSVDataLexer",
                      {Binding{"Source", Source}},
                      {Binding{"CSVlexical", CSVlexical, FixedRate(), Add1()}}) {}
protected:
    void generatePabloMethod() override;
};


class CSVDataParser : public pablo::PabloKernel {

    static std::string makeNameFromOptions();
public:
    CSVDataParser(BuilderRef kb, StreamSet * csvMarks,
                  StreamSet * fieldData, StreamSet * recordSeparators, StreamSet * allSeperators)
        : PabloKernel(kb, makeNameFromOptions(),
                      {Binding{"csvMarks", csvMarks, FixedRate(), LookAhead(1)}},
                      {Binding{"fieldData", fieldData}, Binding{"recordSeparators", recordSeparators}, Binding{"allSeperators", allSeperators}}) {
        addAttribute(SideEffecting());
        assert (csvMarks->getNumElements() == 5);
    }
protected:
    void generatePabloMethod() override;
};

class IdentifyLastSelector final: public pablo::PabloKernel {
public:
    IdentifyLastSelector(BuilderRef b, StreamSet * selector_span, StreamSet * selectors)
    : PabloKernel(b, "IdentifyLastSelector",
                  {Binding{"selector_span", selector_span, FixedRate(), LookAhead(1)}},
                  {Binding{"selectors", selectors}}) {}
protected:
    void generatePabloMethod() override;
};


class ExtractCoordinateSequence : public MultiBlockKernel {
public:
    ExtractCoordinateSequence(BuilderRef b, StreamSet * const Matches, StreamSet * const Coordinates, unsigned strideBlocks = 1);
private:
    void generateMultiBlockLogic(BuilderRef iBuilder, llvm::Value * const numOfStrides) override;
};

}


#endif // CSV_VALIDATOR_UTIL_H
