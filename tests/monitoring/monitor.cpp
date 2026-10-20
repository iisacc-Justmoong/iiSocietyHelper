#include <iiSocietyHelper/Monitoring/Monitor.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <iiFileProviderPresentation.h>
namespace fs=std::filesystem;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char**argv){
  const auto root=fs::path(argv[1])/"monitor-test.societycontainer";
  fs::remove_all(root);fs::create_directories(root);
  for(const auto name:std::array{"Files","Photos","Asset Library","Generation History","Models","Thinking Space","Forked","Published","Deleted"})fs::create_directories(root/name);
  const std::string id="11111111-2222-4333-8444-555555555555";
  std::ofstream(root/".society-drive.json")<<"{\"type\":\"SocietyDrive\",\"schemaVersion\":1,\"identifier\":\""<<id<<"\"}";
  try{
    iiSocietyHelper::Monitoring::Publisher publisher;
    const auto started=publisher.start(root,id);require(bool(started),started?"":started.error().c_str());
    auto first=publisher.poll();require(first&&*first,"Initial state must be published");
    auto current=iiSocietyHelper::Monitoring::read(root,id);require(bool(current),"Late reader must receive current state");
    require(current->available&&current->daemonRunning&&current->sections.size()==9,"All nine sections must be present");
    const auto revision=current->revision;
    require(publisher.poll().value()==false,"Heartbeat must not increment revision");
    fs::create_directories(root/"Files/nested");std::ofstream(root/"Files/nested/hello.txt")<<"hello";
    require(publisher.poll().value(),"Nested additions must emit a new state");
    current=iiSocietyHelper::Monitoring::read(root,id);require(current&&current->revision>revision,"Reader must see new revision");
    bool files=false;for(const auto&s:current->sections)if(s.key=="files") {
      files=s.files==1&&s.bytes==5;
      const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      require(s.lastModified>now-60000&&s.lastModified<=now+1000,"File timestamps must use the system clock epoch on every platform");
    }
    require(files,"Section inventory must report file count and logical bytes");
    fs::create_directories(root/".society-sync");std::ofstream(root/".society-sync/catalog.json")<<"{}";
    require(publisher.poll().value(),"Replica catalog metadata changes must emit namespace invalidation");
    current=iiSocietyHelper::Monitoring::read(root,id);
    require(current->changedSections.size()==9&&current->sections.front().files==1,"Catalog invalidation must preserve physical inventory counts");
    const auto prior=current->revision;
    fs::rename(root/"Files/nested/hello.txt",root/"Files/nested/renamed.txt");
    require(publisher.poll().value(),"Same-size renames must change the fingerprint");
    current=iiSocietyHelper::Monitoring::read(root,id);require(current->revision>prior,"Rename revision must advance");
    fs::create_symlink(root/".society-drive.json",root/"Files/redirect.json");
    require(publisher.poll().has_value(),"Redirected payloads must be skipped safely");
    current=iiSocietyHelper::Monitoring::read(root,id);
    require(current->sections.front().files==1,"Redirects must not add file counts or bytes");
    const auto state=iiSocietyHelper::Monitoring::statePath(root);
    const auto saved=root/"saved.cbor";
    fs::copy_file(state,saved);
    std::ofstream(state,std::ios::binary|std::ios::trunc)<<"truncated";
    require(!iiSocietyHelper::Monitoring::read(root,id),"Malformed wire state must be rejected");
    fs::copy_file(saved,state,fs::copy_options::overwrite_existing);fs::remove(saved);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto stale=iiSocietyHelper::Monitoring::read(root,id,std::chrono::milliseconds(1));
    require(stale&&!stale->daemonRunning&&stale->available,"Stale heartbeat must preserve container availability");
    iiSocietyHelper::Monitoring::Publisher duplicate;require(!duplicate.start(root,id),"Only one producer may own a container");
    require(!iiSocietyHelper::Monitoring::read(root,"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),"Identity mismatch must be rejected");
    publisher.stop();current=iiSocietyHelper::Monitoring::read(root,id);require(current&&!current->daemonRunning,"Shutdown must publish offline state");
    require(fs::file_size(root/"Files/nested/renamed.txt")==5,"Monitoring must preserve user data");
    iiSocietyHelper::Monitoring::Publisher bounded;require(bool(bounded.start(root,id,{0,2})),"Bounded publisher must start");
    require(bool(bounded.poll()),"Bounded scan must publish a partial state");
    require(!bounded.state().complete,"Truncated scans must not claim completeness");bounded.stop();
    const auto selection=root.parent_path()/"monitor-selection.json";
    std::ofstream(selection)<<"{\"schemaVersion\":\"1\",\"path\":\""<<root.generic_string()<<"\",\"containerId\":\""<<id<<"\"}";
    require(!iiSocietyHelper::Monitoring::readSelection(selection),"String schema version must not be accepted as a number");
    std::ofstream(selection)<<"{\"schemaVersion\":1,\"path\":\""<<root.generic_string()<<"\",\"containerId\":\""<<id<<"\"}";
    require(bool(iiSocietyHelper::Monitoring::readSelection(selection)),"Valid selection must resolve");fs::remove(selection);
    iiSocietyHelper::Monitoring::Publisher pinned;require(bool(pinned.start(root,id)),"Pinned publisher must start");require(bool(pinned.poll()),"Pinned publisher must publish");
    // Runtime snapshots schedule optional metadata publication. Quiesce that
    // fixture work before testing root replacement on Windows.
    iiFileProvider::native::Presentation::cancelPending(root);
#ifdef _WIN32
    const auto moved=fs::path(root.string()+".moved");
    std::error_code moveError;fs::rename(root,moved,moveError);
    require(moveError==std::errc::permission_denied,"An active Windows owner must prevent ancestor renames");
    require(bool(pinned.poll()),"A denied rename must preserve the pinned publisher");
    pinned.stop();
    iiFileProvider::native::Presentation::cancelPending(root);
    fs::rename(root,moved);fs::create_directory_symlink(moved,root);
    require(!iiSocietyHelper::Monitoring::read(root,id),"Redirected root must be rejected by readers");
#else
    const auto moved=fs::path(root.string()+".moved");fs::rename(root,moved);fs::create_directory_symlink(moved,root);
    require(!pinned.poll(),"Redirecting a selected root must stop publication");
    require(!iiSocietyHelper::Monitoring::read(root,id),"Redirected root must be rejected by readers");
#endif
    fs::remove(root);fs::rename(moved,root);
    fs::remove_all(root);std::cout<<"Monitor contract passed\n";return 0;
  }catch(const std::exception&e){std::cerr<<e.what()<<'\n';fs::remove_all(root);return 1;}
}
