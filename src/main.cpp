#include <windows.h>

#include "app/Application.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    veloplay::Application application;
    return application.run();
}
