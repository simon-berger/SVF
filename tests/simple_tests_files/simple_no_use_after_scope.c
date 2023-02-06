#include <stdlib.h>

char* f(){
    char* buf = malloc(8);
    return buf;
}

int main() {
    char* buf = f();
    buf[0] = 'A';
    free(buf);
}