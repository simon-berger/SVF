#include <stdio.h>

int main() {
    int i = 0;
    int* ptr = &i;
    *ptr = 42;
}