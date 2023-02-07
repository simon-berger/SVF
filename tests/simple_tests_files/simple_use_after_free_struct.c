#include <stdlib.h>

struct myStruct {
  int val_1;
  double val_2;
};

int main() {
    struct myStruct* my_struct = malloc(sizeof(struct myStruct));
    free(my_struct);
    my_struct->val_1 = 42; // use-after-free
}  