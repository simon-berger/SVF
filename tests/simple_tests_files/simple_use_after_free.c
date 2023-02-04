#include <stdlib.h>

int main() {
    char* buf = malloc(512);
    free(buf);
    buf = "Hello World!";
}  