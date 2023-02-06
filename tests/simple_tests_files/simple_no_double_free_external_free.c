#include <stdlib.h>

void _free(void* p){
    free(p);
}

int main() {
    char* buf = malloc(512);
    _free(buf);
}  