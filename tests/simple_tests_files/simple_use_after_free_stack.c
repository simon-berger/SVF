#include <stdio.h>

int* f(){
    int arr[8];
    return arr;
}

int main(int argc, char **argv) {
    int* arr = f();
    arr[0] = 42;
}