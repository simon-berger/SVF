#include <stdlib.h>

struct myStruct {
  int val_1;
  double val_2;
};

int main() {
    struct myStruct* my_struct = malloc(sizeof(struct myStruct));
    my_struct->val_1 = 42;
    free(my_struct);
}  