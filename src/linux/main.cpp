#include "linux/LinuxApplication.hpp"

int main(int argc, char** argv) {
    veloplay::linuxos::LinuxApplication application(argc, argv);
    return application.run();
}
