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

class Lowerify : public pablo::PabloKernel {
public:
    Lowerify(KernelBuilder & b, StreamSet * U21, StreamSet * translationBasis, StreamSet * u32Basis)
    : pablo::PabloKernel(b, "Lowerify",
                        {Binding{"U21", U21}, Binding{"translationBasis", translationBasis}},
                            {Binding{"u32Basis", u32Basis}}) {}
protected:
    void generatePabloMethod() override;
};

void Lowerify::generatePabloMethod() {
    std::cout << "called lowerify" << std::endl;
    //  pb is an object used for build Pablo language statements
    pablo::PabloBuilder pb(getEntryScope());

    // Get the input stream sets.
    std::vector<PabloAST *> U21 = getInputStreamSet("U21");

    std::vector<PabloAST *> translationBasis = getInputStreamSet("translationBasis");
    std::vector<PabloAST *> transformed(U21.size());

    Var * outputBasisVar = getOutputStreamVar("u32Basis");

    // For each bit of the input stream
    for (unsigned i = 0; i < U21.size(); i++) {
        // If the translation set covers said bit
        if (i < translationBasis.size()) // XOR the input bit with the transformation bit  
            transformed[i] = pb.createXor(translationBasis[i], U21[i]);
        else transformed[i] = U21[i];

        pb.createAssign(pb.createExtract(outputBasisVar, pb.getInteger(i)), transformed[i]);
    }
}

void doLowerTransform(PipelineBuilder & P, StreamSet * Basis, StreamSet * Output);

inline void doLowerTransform(const std::unique_ptr<PipelineBuilder> & P, StreamSet * Basis, StreamSet * Output) {
    return doLowerTransform(*P.get(), Basis, Output);
}

inline void doLowerTransform(const std::unique_ptr<ProgramBuilder> & P, StreamSet * Basis, StreamSet * Output) {
    return doLowerTransform(*P.get(), Basis, Output);
}

void doLowerTransform(PipelineBuilder & P, StreamSet * Basis, StreamSet * Output) {
    // Get the lowercase mapping object, can create a translation set from that
    UCD::CodePointPropertyObject* propertyObject = dyn_cast<UCD::CodePointPropertyObject>(UCD::get_SLC_PropertyObject());
    unicode::BitTranslationSets translationSet;
    translationSet = propertyObject->GetBitTransformSets();

    // Turn the lower translation set into a vector of character classes
    std::vector<re::CC *> translation_ccs;
    for (auto & b : translationSet) {
        translation_ccs.push_back(re::makeCC(b, &cc::Unicode));
    }

    StreamSet * translationBasis = P.CreateStreamSet(translation_ccs.size());
    P.CreateKernelCall<CharClassesKernel>(translation_ccs, Basis, translationBasis);

    // Perform the logic of the Lowerify kernel on the codepoiont values.
    P.CreateKernelCall<Lowerify>(Basis, translationBasis, Output);
}