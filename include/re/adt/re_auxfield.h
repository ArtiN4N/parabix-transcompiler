/*
 *  Copyright (c) 2017 International Characters.
 *  This software is licensed to the public under the Open Software License 3.0.
 *  icgrep is a trademark of International Characters.
 */

#ifndef RE_AUXFIELD_H
#define RE_AUXFIELD_H

#include <re/adt/re_re.h>

namespace re {

class ExternalPropertyName : public RE {
public:
    static ExternalPropertyName * Create(const char * inputName) {return new ExternalPropertyName(inputName);}
    RE_SUBTYPE(ExternalPropertyName)
    const char * getPrecompiledName() const {return mInputName;}
private:
    ExternalPropertyName(const char * inputName)
    : RE(ClassTypeId::ExternalPropertyName), mInputName(inputName) {}
    const char * const mInputName;
};

inline ExternalPropertyName * makeAuxField(const char * inputName) {return ExternalPropertyName::Create(inputName);}

}

#endif // ANY_H
