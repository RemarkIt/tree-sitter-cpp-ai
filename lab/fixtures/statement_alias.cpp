#define send(value) record(value);
#define forward(value) send(value)

int record(int);

int sequence(int value) {
    forward(value)
    send((value + 1))
    return value;
}
