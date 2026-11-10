struct Counter {
    int total;
};
int tally(int n) {
    int sum = 0;
    for (int i = 0; i < n; ++i) int step = i;
    if (n > 3) Counter extra{n};
    else sum += n;
    while (sum > 10) int half = sum / 2;
    return sum;
}
int after(int x) {
    return x;
}
