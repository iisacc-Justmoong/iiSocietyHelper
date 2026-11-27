#include "TestWorkspace.h"
#include <iiSocietyHelper.h>
#include <File.h>
#include <SocietyDrive.h>
#include <SharedStorage.h>
#include <DirectoryMetadataIndex.h>
#include <FileCatalog.h>
#include <FileOperations.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QVariantMap>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace iiFileProvider;
namespace fs=std::filesystem;
/// Assert one externally observable storage behavior in an isolated fixture.
static void check(bool ok,const QString &message){if(!ok)throw std::runtime_error(message.toStdString());}
int main(int argc,char**argv)try{
    QCoreApplication app(argc,argv);app.setApplicationName("SocietyHelperIntegration");app.setApplicationVersion("test");
    QTemporaryDir fixture((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/managed-XXXXXX.societycontainer")));
    check(fixture.isValid() && bool(iiSocietyContainer::SocietyDrive::create(fixture.path())),"fixture");
    iiSocietyHelper::FileSystem files(iiSocietyHelper::FileSystem::Initialization::Deferred);
    check(files.open(fixture.path()),files.errorString());
    const auto name=QString::fromUtf8("선택한 문서.txt").normalized(QString::NormalizationForm_D);
    const auto target=files.path("files",name);
    check(!files.saveFile(target,"first bytes",false).isEmpty(),files.errorString());
    const auto first=files.fileInformation("files",name).value("managed").toMap();
    const auto id=first.value("resource").toMap().value("id");
    check(first.value("resourceStatus")=="directory-metadata-only" && !id.toString().isEmpty(),"saved file has no directory metadata owner");
    check(!files.saveFile(target,"edited bytes").isEmpty(),files.errorString());
    const auto edited=files.fileInformation("files",name).value("managed").toMap();
    check(edited.value("resource").toMap().value("id")==id,"atomic edit changed the file metadata owner");
    check(files.setFileMetadata("files",name,{{"title","Launch campaign"},{"keywords",QStringList{"approved"}},{"rating",5},{"color","green"}}),files.errorString());
    check(files.searchFiles({{"text","campaign"},{"keywords",QStringList{"approved"}},{"minimumRating",5}}).value("files").toList().size()==1,files.errorString());
    const auto root=fs::path(fixture.path().toStdU16String());
    check(!fs::exists(iiSocietyContainer::FileRepository::packagePath(root)/"objects.sqlite3"),"save created a payload history");
    check(files.removeFile("files",name),files.errorString());
    check(!QFileInfo::exists(target) && QFileInfo::exists(fixture.filePath("Deleted/"+name)),"remove did not preserve the file in Deleted");
    check(files.removeFile("deleted",name),files.errorString());
    check(!QFileInfo::exists(fixture.filePath("Deleted/"+name)),"final deletion retained the selected file");
    DirectoryMetadataIndex index(root,DirectoryMetadataIndex::Access::ReadOnly);
    check(index.paths().empty(),"final deletion retained the selected metadata path");
    check(!fs::exists(iiSocietyContainer::FileRepository::packagePath(root)/"objects.sqlite3"),"file lifecycle created a payload history");
    const auto inputDirectory=fixture.filePath("Files/Dreamscapes/GenerationInputs/job-a");
    check(QDir().mkpath(inputDirectory),"generation input fixture directory");
    const auto inputFile=inputDirectory+"/input.txt";
    const auto savedInput=files.saveFile(inputFile,"generation input",false);
    check(!savedInput.isEmpty(),"save generation input: "+files.errorString());
    const auto removedInputs=files.removeGenerationInputs(inputDirectory);
    check(removedInputs,"trash generation inputs: "+files.errorString());
    check(!QFileInfo::exists(inputDirectory) && QFileInfo::exists(fixture.filePath("Deleted/job-a/input.txt")),
          "removing generation inputs bypassed Deleted");
    const auto inputDrive=iiSocietyContainer::FileOperations::containingDrive(fixture.filePath("Deleted/job-a"));
    check(bool(inputDrive),"generation input drive");
    const auto deletedInputs=iiSocietyContainer::FileOperations(*inputDrive).perform(
        iiSocietyContainer::FileOperations::Action::Remove,fixture.filePath("Deleted/job-a"));
    check(bool(deletedInputs),"delete generation inputs: "+deletedInputs.error);
    check(index.paths().empty(),"final deletion retained generation input metadata");
    // Editing an existing legacy file preserves its historical object without
    // capturing the replacement bytes as another object-store revision.
    const auto legacyName=QString::fromUtf8("기존 문서.txt").normalized(QString::NormalizationForm_D);
    const auto legacy=files.path("files",legacyName);File::create(legacy,"legacy original");
    const auto legacyLogical=std::string("files/")+legacyName.toStdString();
    const auto storage=iiSocietyContainer::SharedStorage::open(fixture.path());check(bool(storage),"legacy storage fixture");
    std::string legacyKey;std::uint64_t legacyVersion=0,legacyCount=0;
    {
        FileCatalog catalog(root/"Files",FileCatalog::Access::Create,
            iiSocietyContainer::FileRepository::classificationDirectory(root,"files"));
        check(catalog.registerFile(legacyName.toStdString()).metadataWritten,"legacy classification fixture");
        auto repository=storage->fileRepository();
        const auto document=repository->objects().importDocument(fs::path(legacy.toStdU16String()),legacyLogical,
            {legacyName.toStdString()},repository->sessionKey());
        legacyKey=document.file.key;
        (void)repository->observe(legacyLogical);
        legacyVersion=repository->objects().lookup(legacyKey)->version;
        legacyCount=repository->objects().count(true);
    }
    const auto legacySave=files.saveFile(legacy,"replacement");
    check(!legacySave.isEmpty(),"legacy replacement: "+files.errorString());
    check(File::read(legacy)==QByteArray("replacement"),"legacy replacement was not saved");
    {const auto repository=storage->fileRepository({},iiSocietyContainer::FileRepository::Access::ReadOnly);
     const auto old=repository->objects().lookup(legacyKey);
     check(old && old->version==legacyVersion && repository->objects().count(true)==legacyCount &&
           repository->objects().validate(legacyKey,legacyVersion),"replacement changed or duplicated legacy history");}
    const auto legacyTrash=files.removeFile("files",legacyName);
    check(legacyTrash,"legacy trash: "+files.errorString());
    const auto legacyDelete=files.removeFile("deleted",legacyName);
    check(legacyDelete,"legacy final deletion: "+files.errorString());
    {const auto repository=storage->fileRepository({},iiSocietyContainer::FileRepository::Access::ReadOnly);
     check(!repository->objects().lookup(legacyKey),"legacy final deletion retained the selected history");}
    std::cout<<"Helper save, metadata search, Trash and final deletion verified without payload capture\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
