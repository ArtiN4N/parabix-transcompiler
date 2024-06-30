#ifndef TRANSFORM_KERNEL_H
#define TRANSFORM_KERNEL_H

#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/pipeline_builder.h>

void remove_whitespace(kernel::KernelBuilder &b, kernel::StreamSet *input, kernel::StreamSet *output);
void nfd(kernel::KernelBuilder &b, kernel::StreamSet *input, kernel::StreamSet *output);
void nfc(kernel::KernelBuilder &b, kernel::StreamSet *input, kernel::StreamSet *output);

#endif // TRANSFORM_KERNEL_H
