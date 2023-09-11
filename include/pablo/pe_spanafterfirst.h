#ifndef PE_SPANAFTERFIRST_H
#define PE_SPANAFTERFIRST_H

#include <pablo/pabloAST.h>

namespace pablo {

class SpanAfterFirst final : public CarryProducingStatement {
    friend class PabloBlock;
public:
    static inline bool classof(const PabloAST * e) {
        return e->getClassTypeId() == ClassTypeId::SpanAfterFirst;
    }
    static inline bool classof(const void *) {
        return false;
    }
    virtual ~SpanAfterFirst(){
    }
    PabloAST * getExpr() const {
        return getOperand(0);
    }
protected:
    SpanAfterFirst(PabloAST * expr, const String * name, Allocator & allocator)
    : CarryProducingStatement(ClassTypeId::SpanAfterFirst, expr->getType(), {expr}, name, allocator) {

    }
};

}

#endif // PE_SPANAFTERFIRST_H
