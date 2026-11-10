int perform(int value);
int login(int a, int b) {
#ifndef DISABLE_DIGEST
    if (a)
        return perform(1);
    else
#endif
    if (b)
        return perform(2);
    return 0;
}
int close_log(int file) {
#ifndef DISABLE_TTY
    if (file != 1)
#endif
        file = perform(file);
#ifdef VMS
    if (perform(3) == -1)
#else
    if (perform(4) == -1)
#endif
    {
        int failed = 1;
        return failed;
    }
    return file;
}
