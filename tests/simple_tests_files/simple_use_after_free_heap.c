#include <stdlib.h>

int main(int argc, char **argv) {
    char* buf = malloc(512);
    free(buf);
    buf = "Hello World!";
}  