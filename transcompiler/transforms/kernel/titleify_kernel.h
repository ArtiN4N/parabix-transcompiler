#pragma once

#include <vector>
#include <array>
#include <fcntl.h>
#include <string>
#include <iostream>

#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>
#include <unicode/core/unicode_set.h>
#include <unicode/utf/utf_compiler.h>
#include <unicode/utf/transchar.h>
#include <unicode/data/PropertyObjects.h>
#include <unicode/data/PropertyObjectTable.h>

#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/string_insert.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <kernel/pipeline/pipeline_builder.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>

#include <pablo/codegenstate.h>
#include <pablo/pe_zeroes.h>        // for Zeroes
#include <pablo/bixnum/bixnum.h>

#include <grep/grep_kernel.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

#include <re/toolchain/toolchain.h>
#include <re/adt/re_name.h>
#include <re/adt/re_re.h>
#include <re/cc/cc_compiler_target.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_kernel.h>


using namespace kernel;
using namespace llvm;
using namespace pablo;

class Titleify : public pablo::PabloKernel {
public:
    Titleify(KernelBuilder & b, StreamSet * U21, StreamSet * titleBasis, StreamSet * lowerBasis, StreamSet * u32Basis)
    : pablo::PabloKernel(b, "Titleify",
                        {Binding{"U21", U21}, Binding{"titleBasis", titleBasis}, Binding{"lowerBasis", lowerBasis}},
                            {Binding{"u32Basis", u32Basis}}) {}
protected:
    void generatePabloMethod() override;
};

void Titleify::generatePabloMethod() {
    //  pb is an object used for build Pablo language statements
    pablo::PabloBuilder pb(getEntryScope());

    // Get the input stream sets.
    std::vector<PabloAST *> U21 = getInputStreamSet("U21");
    cc::Parabix_CC_Compiler_Builder ccc(getEntryScope(), U21);

    std::vector<PabloAST *> titleBasis = getInputStreamSet("titleBasis");
    std::vector<PabloAST *> lowerBasis = getInputStreamSet("lowerBasis");

    std::vector<PabloAST *> transformedTitle(U21.size());
    std::vector<PabloAST *> transformedLower(U21.size());

    Var * outputBasisVar = getOutputStreamVar("u32Basis");

    // Create a character class from the whitespace property set
    UCD::PropertyObject * whiteSpacesProperty = UCD::get_WSPACE_PropertyObject();
    UCD::UnicodeSet wSpaceSet = whiteSpacesProperty->GetCodepointSet("");
    PabloAST * whiteSpaces = ccc.compileCC(re::makeCC(wSpaceSet));

    // Find all characters after a whitespace
    PabloAST * afterWhiteSpaces = pb.createNot(pb.createAdvance(pb.createNot(whiteSpaces), 1));

    for (unsigned i = 0; i < U21.size(); i++) {
        
        // If the translation set covers said bit, XOR the input bit with the transformation bit
        if (i < titleBasis.size())
            transformedTitle[i] = pb.createXor(titleBasis[i], U21[i]);
        else transformedTitle[i] = U21[i];

        if (i < lowerBasis.size())
            transformedLower[i] = pb.createXor(lowerBasis[i], U21[i]);
        else transformedLower[i] = U21[i];

        // Convert to title case after whitespaces, otherwise, lowercase
        pb.createAssign(pb.createExtract(outputBasisVar, pb.getInteger(i)), pb.createSel(afterWhiteSpaces, transformedTitle[i], transformedLower[i]));
    }
}

void doTitleTransform(PipelineBuilder & P, StreamSet * Basis, StreamSet * Output);

inline void doTitleTransform(const std::unique_ptr<PipelineBuilder> & P, StreamSet * Basis, StreamSet * Output) {
    return doTitleTransform(*P.get(), Basis, Output);
}

inline void doTitleTransform(const std::unique_ptr<ProgramBuilder> & P, StreamSet * Basis, StreamSet * Output) {
    return doTitleTransform(*P.get(), Basis, Output);
}

void doTitleTransform(PipelineBuilder & P, StreamSet * Basis, StreamSet * Output) {
    // Create a bit translation set to title case
    UCD::CodePointPropertyObject* titlePropertyObject = dyn_cast<UCD::CodePointPropertyObject>(UCD::get_STC_PropertyObject());
    unicode::BitTranslationSets titleTranslationSet;
    titleTranslationSet = titlePropertyObject->GetBitTransformSets();

    // Turn the title translation set into a vector of character classes
    std::vector<re::CC *> titleTranslation_ccs;
    for (auto & b : titleTranslationSet) {
        titleTranslation_ccs.push_back(re::makeCC(b, &cc::Unicode));
    }

    StreamSet * titleBasis = P.CreateStreamSet(titleTranslation_ccs.size());
    P.CreateKernelCall<CharClassesKernel>(titleTranslation_ccs, Basis, titleBasis);

    // Create a bit translation set to lower case
    UCD::CodePointPropertyObject* lowerPropertyObject = dyn_cast<UCD::CodePointPropertyObject>(UCD::get_SLC_PropertyObject());
    unicode::BitTranslationSets lowerTranslationSet;
    lowerTranslationSet = lowerPropertyObject->GetBitTransformSets();

    // Turn the title translation set into a vector of character classes
    std::vector<re::CC *> lowerTranslation_ccs;
    for (auto & b : lowerTranslationSet) {
        lowerTranslation_ccs.push_back(re::makeCC(b, &cc::Unicode));
    }

    StreamSet * lowerBasis = P.CreateStreamSet(lowerTranslation_ccs.size());
    P.CreateKernelCall<CharClassesKernel>(lowerTranslation_ccs, Basis, lowerBasis);

    // Perform the logic of the Titleify kernel on the codepoiont values.
    P.CreateKernelCall<Titleify>(Basis, titleBasis, lowerBasis, Output);
}