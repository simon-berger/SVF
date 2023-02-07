#include <stdio.h>

char* create_buf(){
    char buf[16];
    return buf; // buf will be out-of-scope after the return
}

int main() {
    char* buf = create_buf();
    sprintf(buf, "%s", "Hello World!"); // use-after-scope
}