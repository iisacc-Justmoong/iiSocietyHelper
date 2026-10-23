#include <iiSocietyHelper.h>
#include <File.h>
#include <SocietyDrive.h>
#include <SharedStorage.h>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QVariantMap>
#include <iostream>
#include <stdexcept>
using namespace iiFileProvider;
void check(bool ok,const QString &message){if(!ok)throw std::runtime_error(message.toStdString());}
int main(int argc,char**argv)try{
    QCoreApplication app(argc,argv);app.setApplicationName("SocietyHelperIntegration");app.setApplicationVersion("test");
    QTemporaryDir fixture(QString(HELPER_TEST_DIRECTORY)+"/managed-XXXXXX.societycontainer");
    check(fixture.isValid() && bool(iiSocietyContainer::SocietyDrive::create(fixture.path())),"fixture");
    iiSocietyHelper::FileSystem files(iiSocietyHelper::FileSystem::Initialization::Deferred);
    check(files.open(fixture.path()),files.errorString());
    const auto unicodeName=QString::fromUtf8("선택한 문서.txt").normalized(QString::NormalizationForm_D);
    const auto unicodePath=files.path("files",unicodeName);
    check(!files.saveFile(unicodePath,"Unicode first",false).isEmpty(),files.errorString());
    check(!files.saveFile(unicodePath,"Unicode edited").isEmpty(),files.errorString());
    check(files.fileInformation("files",unicodeName).value("managed").toMap().value("status")=="ready","decomposed Unicode save and edit are tracked");
    const auto target=files.path("files","hello.txt");check(!files.saveFile(target,"first bytes",false).isEmpty(),files.errorString());
    auto first=files.fileInformation("files","hello.txt").value("managed").toMap();check(first.value("status")=="ready","save captured RAW");
    check(!files.saveFile(target,"edited bytes").isEmpty(),files.errorString());
    auto edited=files.fileInformation("files","hello.txt").value("managed").toMap();
    check(first.value("objectKey")==edited.value("objectKey") && first.value("originalSha256")==edited.value("originalSha256") && first.value("version")!=edited.value("version"),"actual save retains original and revision chain");
    check(files.setFileMetadata("files","hello.txt",{{"title","Launch campaign"},{"keywords",QStringList{"approved"}},{"rating",5},{"color","green"}}),files.errorString());
    check(files.searchFiles({{"text","campaign"},{"keywords",QStringList{"approved"}},{"minimumRating",5}}).value("files").toList().size()==1,files.errorString());
    const auto storage=iiSocietyContainer::SharedStorage::open(fixture.path());auto repo=storage->fileRepository();
    check(files.applyOntology({OntologyClass{"ProjectAsset","Project","",{iiSocietyContainer::FileRepository::fileClass}},OntologyTypeAssignment{OntologyResource::file(first.value("objectKey").toString().toStdString()),"ProjectAsset"},OntologyDirectory{"Projects/Launch","Launch",{"ProjectAsset"}}},repo->ontology().revision()),files.errorString());
    check(files.searchFiles({{"directory","Projects/Launch"}}).value("files").toList().size()==1,files.errorString());
    check(files.fileJournal({},128,first.value("objectKey").toString()).size()>=2 && !files.workSessions().isEmpty(),files.errorString());
    File::create(files.path("files","external.txt"),"external native input");check(files.indexFiles(),files.errorString());
    check(files.fileInformation("files","external.txt").value("managed").toMap().value("status")=="ready","external intake captured");
    check(files.removeFile("files","hello.txt"),files.errorString());
    check(files.searchFiles({{"directory","Projects/Launch"}}).value("files").toList().isEmpty(),files.errorString());
    check(repo->objects().lookup(first.value("objectKey").toString().toStdString())->deleted,"actual remove journal");
    repo.reset();std::cout<<"Helper actual save, edit, metadata, ontology, journal, sessions and intake verified\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
