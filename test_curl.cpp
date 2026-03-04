#include <iostream>
#include <string>
#include <unistd.h>
#include <format>
int main() {
    std::string tmp_req = std::format("/tmp/agentos_req_{}.json", getpid());
    std::cout << tmp_req << std::endl;
    return 0;
}
