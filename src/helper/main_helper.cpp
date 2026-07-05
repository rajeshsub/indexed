#include "Version.h"
#include <cstdio>

int main() {
    std::printf("indexed-helper v%s\n", indexed::GetVersionString());
    return 0;
}
