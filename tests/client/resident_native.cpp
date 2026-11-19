#include "../../src/Client/ResidentNative.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using iiSocietyHelper::ResidentNative;
static std::string readFrame(int fd) {
    unsigned char h[4]; assert(::recv(fd,h,4,MSG_WAITALL)==4);
    const auto n=(unsigned(h[0])<<24)|(unsigned(h[1])<<16)|(unsigned(h[2])<<8)|unsigned(h[3]);
    assert(n>0 && n<1024*1024); std::string body(n,'\0');
    assert(::recv(fd,body.data(),n,MSG_WAITALL)==ssize_t(n)); return body;
}
static void sendFrame(int fd,const std::string &body) {
    const auto n=unsigned(body.size()); const unsigned char h[]{static_cast<unsigned char>(n>>24),static_cast<unsigned char>(n>>16),
        static_cast<unsigned char>(n>>8),static_cast<unsigned char>(n)};
    assert(::send(fd,h,4,0)==4); assert(::send(fd,body.data(),body.size(),0)==ssize_t(body.size()));
}
int main() {
    namespace fs=std::filesystem;
    const auto root=fs::path(RESIDENT_TEST_DIRECTORY)/("resident-"+std::to_string(::getpid()));
    assert(fs::create_directory(root)); ::chmod(root.c_str(),0700);
    const auto path=root/"backend.sock"; const int server=::socket(AF_UNIX,SOCK_STREAM,0); assert(server>=0);
    sockaddr_un address{}; address.sun_family=AF_UNIX; assert(path.string().size()<sizeof(address.sun_path));
    std::strcpy(address.sun_path,path.c_str());
    assert(::bind(server,reinterpret_cast<sockaddr *>(&address),sizeof(address))==0);
    assert(::chmod(path.c_str(),0600)==0); assert(::listen(server,2)==0);
    std::jthread daemon([&] {
        const int client=::accept(server,nullptr,nullptr); assert(client>=0);
        const auto subscribe=readFrame(client); assert(subscribe.find("iisaccsonSubscribe")!=std::string::npos);
        sendFrame(client,R"({"type":"iisaccsonState","resident":{"state":"ready"}})");
        const auto request=readFrame(client);
        assert(request.find("iisaccsonGenerate")!=std::string::npos);
        assert(request.find("hello \\\"app\\\"")!=std::string::npos);
        sendFrame(client,R"({"type":"iisaccsonEvent","event":{"kind":"delta","text":"answer"}})");
        assert(readFrame(client).find("iisaccsonCancel")!=std::string::npos);
        const unsigned char invalid[]{0x7f,0xff,0xff,0xff}; assert(::send(client,invalid,4,0)==4);
        ::close(client);
    });
    ResidentNative client;
    assert(::chmod(path.c_str(),0666)==0); assert(!client.connect(root)); assert(::chmod(path.c_str(),0600)==0);
    assert(client.connect(root)); assert(client.subscribe());
    const auto status=client.receiveJson(); assert(status && status->find("ready")!=std::string::npos);
    const auto request=client.generate({{"user","hello \"app\""}}); assert(request);
    const auto answer=client.receiveJson(); assert(answer && answer->find("answer")!=std::string::npos);
    assert(client.cancel(*request)); assert(!client.receiveJson());
    client.close(); daemon.join();
    std::jthread waitingDaemon([&] {
        const int peer=::accept(server,nullptr,nullptr); assert(peer>=0);
        assert(readFrame(peer).find("iisaccsonSubscribe")!=std::string::npos);
        char byte{}; assert(::recv(peer,&byte,1,0)==0); ::close(peer);
    });
    ResidentNative waiting; assert(waiting.connect(root)); assert(waiting.subscribe());
    std::promise<void> entered; auto enteredFuture=entered.get_future();
    auto read=std::async(std::launch::async,[&] { entered.set_value(); return waiting.receiveJson(); });
    enteredFuture.get(); waiting.close();
    assert(read.wait_for(std::chrono::seconds(2))==std::future_status::ready); assert(!read.get());
    waitingDaemon.join();

    // A partial frame cannot keep a bounded read alive indefinitely. Closing
    // that connection also prevents the tail from becoming a new header.
    std::jthread partialDaemon([&] {
        const int peer=::accept(server,nullptr,nullptr); assert(peer>=0);
        const unsigned char partial[]{0,0,0,5,'{'};
        assert(::send(peer,partial,sizeof(partial),0)==sizeof(partial));
        char byte{}; assert(::recv(peer,&byte,1,0)==0); ::close(peer);
    });
    using namespace std::chrono_literals;
    ResidentNative bounded;
    const auto begin=std::chrono::steady_clock::now();
    const iiSocietyHelper::ResidentWait wait{.deadline=begin+150ms};
    assert(bounded.connect(root,wait));
    const auto timeout=bounded.receiveJson(wait);
    assert(!timeout && timeout.error().find("timed out")!=std::string::npos);
    assert(std::chrono::steady_clock::now()-begin<2s);
    partialDaemon.join();

    // An already retired request must not open a fresh daemon connection.
    const iiSocietyHelper::ResidentWait canceled{.canceled=[] {return true;}};
    assert(!bounded.connect(root,canceled));
    ::close(server); fs::remove(path); fs::remove(root);
}
