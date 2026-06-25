#pragma once
#include <stdint.h>

class IAMI {
public:
    virtual uintptr_t GetLib(const char* soname) = 0;
    virtual void*     GetSym(uintptr_t base, const char* sym) = 0;
    virtual void      Redirect(uintptr_t got, uintptr_t to, uintptr_t* orig) = 0;
};

extern IAMI* AML;
