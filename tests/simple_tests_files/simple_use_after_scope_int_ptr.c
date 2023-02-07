#include <stdio.h>

int* create_ptr(){
    int i = 0;
    return &i; // ptr will be out-of-scope after the return
}

int main() {
    int* ptr = create_ptr();
    *ptr = 42; // use-after-scope
}