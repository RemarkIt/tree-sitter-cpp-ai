using Token = int;
struct Device {
    Device(Token);
    ~Device();
};
Device::Device(Token) {}
Device::~Device() {}
