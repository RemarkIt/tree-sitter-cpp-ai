#define send(value) record(value);

int record(int);
const char *label = "send(value) // not a macro call";
const char *raw_label = R"tag(send(value) /* not code */)tag";

int before(int value) {
    send(value);
    return value;
}

#undef send
int send(int);

int after(int value) {
    // send(value) is now an ordinary function.
    send(value + 1);
    return value;
}
