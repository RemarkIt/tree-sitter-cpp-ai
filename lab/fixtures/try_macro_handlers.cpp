#define CATCH_RETURN() catch (...) { return -1; }
#define CATCH_LOG_MSG(message) catch (...) { (void)message; }
int guarded(int value) {
    try {
        value = value * 2;
    }
    CATCH_RETURN();
    return value;
}
void logged(int *value) {
    try
    {
        *value = 1;
    }
    CATCH_LOG_MSG("failed to set")
    int after = 0;
}
