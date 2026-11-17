#include <iiSocietyHelperNative/ResidentNative.h>
#include <iostream>

int main() {
    iiSocietyHelper::ResidentNative client;
    if (auto connected=client.connect(); !connected) { std::cerr<<connected.error()<<'\n'; return 1; }
    if (auto subscribed=client.subscribe(); !subscribed) { std::cerr<<subscribed.error()<<'\n'; return 1; }
    // A real application should parse status events and submit only after ready.
    if (auto status=client.receiveJson(); status) std::cout<<*status<<'\n';
    else { std::cerr<<status.error()<<'\n'; return 1; }
}
