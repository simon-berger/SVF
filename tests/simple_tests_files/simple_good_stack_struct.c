struct myStruct {
  int val_1;
  double val_2;
};

struct myStruct create_struct(){
    struct myStruct my_struct;;
    return my_struct; // struct will be out-of-scope after the return
}

int main() {
    struct myStruct my_struct = create_struct();
    my_struct.val_1 = 42; // use-after-scope
}