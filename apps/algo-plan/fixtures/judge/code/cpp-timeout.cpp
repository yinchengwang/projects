#include <iostream>

// 死循环用的自增变量：volatile 保证优化器不会把它删掉
volatile long long spin = 0;

int main() {
    // 故意死循环：用来触发 timeout
    for (;;) {
        spin = spin + 1;
    }
    return 0;
}
