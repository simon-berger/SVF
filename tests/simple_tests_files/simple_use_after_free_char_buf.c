#include <stdlib.h>
#include <stdio.h>

int main() {
    char* buf = malloc(512);
    free(buf);
    sprintf(buf, "%s", "Hello World!"); // use-after-free
}  