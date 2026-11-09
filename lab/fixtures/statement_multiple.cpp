#define repeat(value) record(value); record((value) + 1);

int record(int);

int sequence(int value) {
    repeat(value)
    return value;
}
