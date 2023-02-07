#include <stdlib.h>
#include <stdio.h>

int main() {
    char* buf = malloc(512);
    sprintf(buf, "%s", "Hello World!");
    free(buf);
}  