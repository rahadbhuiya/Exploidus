#include "../libc/syscall.h"
int main(void)
{
    write(1, "Hello from Exploidus disk!\n", 27);
    write(1, "Disk execution works!\n", 22);
    return 0;
}
