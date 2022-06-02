#include <re/transforms/reference_transform.h>

#include <re/adt/adt.h>
#include <re/analysis/capture-ref.h>
#include <re/analysis/re_analysis.h>
#include <re/transforms/re_transformer.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace re {


struct FixedReferenceTransformer : public RE_Transformer {
public:
    FixedReferenceTransformer(const ReferenceInfo & info) :
        RE_Transformer("FixedReferenceTransformer"),
        mRefInfo(info) {}
    RE * transformReference(Reference * r) override {
        auto rg1 = getLengthRange(r->getCapture(), &cc::Unicode);
        if (rg1.first != rg1.second) return r;
        auto mapping = mRefInfo.twixtREs.find(r->getName());
        if (mapping == mRefInfo.twixtREs.end()) return r;
        auto rg2 = getLengthRange(mapping->second, &cc::Unicode);
        if (rg2.first != rg2.second) return r;
        int fixed_dist = rg1.first + rg2.first;
        return re::makeName("back_ref_dist=" + std::to_string(fixed_dist), r);
    };
private:
    const ReferenceInfo & mRefInfo;
};

RE * fixedReferenceTransform(const ReferenceInfo & info, RE * r) {
    return FixedReferenceTransformer(info).transformRE(r);
}

}
