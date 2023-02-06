char* f(){
    char arr[8];
    return arr; // arr will be out-of-scope after the return
}

int main() {
    char* arr = f();
    arr[0] = 'A'; // use-after-scope
}