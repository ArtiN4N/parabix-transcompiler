#include "kernel_calls.h"

using namespace std;

void transformUppercaseToLowercase(string &input) {
    for (auto &c : input) {
        if (c >= 'A' && c <= 'Z') {
            c = c + ('a' - 'A');
        }
    }
}