#include <stdlib.h>

int main() {
    int* ptr = malloc(sizeof(int));
    free(ptr);
    free(ptr); // double-free
}  