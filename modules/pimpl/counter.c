#include "state.h"

// A second source file, reaching the same state without being handed anything.
void bump(uint32_t times) {
    for (uint32_t i = 0; i < times; i++) state()->counter++;
}
