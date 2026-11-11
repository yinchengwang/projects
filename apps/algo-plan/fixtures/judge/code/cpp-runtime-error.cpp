#include <iostream>
#include <stdexcept>

int main() {
    std::cout << "start" << std::endl;
    // 故意抛一个没人接的异常：进程非零退出（实测退出码 3）
    throw std::runtime_error("boom");
}
