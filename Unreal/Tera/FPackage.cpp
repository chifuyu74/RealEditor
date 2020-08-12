#include "FPackage.h"
#include "ALog.h"
#include "FStream.h"
#include "FObjectResource.h"
#include "UObject.h"
#include "UClass.h"
#include "UMetaData.h"
#include "UPersistentCookerData.h"
#include "Cast.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <ppl.h>

const char* PackageMapperName = "PkgMapper";
const char* CompositePackageMapperName = "CompositePackageMapper";
const char* ObjectRedirectorMapperName = "ObjectRedirectorMapper";
const char* PackageListName = "DirCache.re";
const char* PersistentDataName = "GlobalPersistentCookerData";

const char Key1[] = { 12, 6, 9, 4, 3, 14, 1, 10, 13, 2, 7, 15, 0, 8, 5, 11 };
const char Key2[] = { 'G', 'e', 'n', 'e', 'r', 'a', 't', 'e', 'P', 'a', 'c', 'k', 'a', 'g', 'e', 'M', 'a', 'p', 'p', 'e', 'r' };

const std::vector<FString> DefaultClassPackageNames = { "Core.u", "Engine.u", "S1Game.u", "GameFramework.u", "GFxUI.u" };

FString FPackage::RootDir;
std::recursive_mutex FPackage::PackagesMutex;
std::vector<std::shared_ptr<FPackage>> FPackage::LoadedPackages;
std::vector<std::shared_ptr<FPackage>> FPackage::DefaultClassPackages;
std::vector<FString> FPackage::DirCache;
std::unordered_map<FString, FString> FPackage::PkgMap;
std::unordered_map<FString, FString> FPackage::ObjectRedirectorMap;
std::unordered_map<FString, FCompositePackageMapEntry> FPackage::CompositPackageMap;
std::unordered_map<FString, FBulkDataInfo> FPackage::BulkDataMap;
std::unordered_map<FString, FTextureFileCacheInfo> FPackage::TextureCacheMap;
std::mutex FPackage::ClassMapMutex;
std::unordered_map<FString, UObject*> FPackage::ClassMap;
std::unordered_set<FString> FPackage::MissingClasses;

uint16 FPackage::CoreVersion = VER_TERA_CLASSIC;

void BuildPackageList(const FString& path, std::vector<FString>& dirCache)
{
  std::filesystem::path fspath(path.WString());
  dirCache.resize(0);
  std::vector<std::filesystem::path> tmpPaths;
  for (auto& p : std::filesystem::recursive_directory_iterator(fspath))
  {
    if (!p.is_regular_file() || !p.file_size())
    {
      continue;
    }
    std::filesystem::path itemPath = p.path();
    std::string ext = itemPath.extension().string();
    if (_stricmp(ext.c_str(), ".gpk") || _stricmp(ext.c_str(), ".gmp") || _stricmp(ext.c_str(), ".upk") || _stricmp(ext.c_str(), ".u"))
    {
      std::error_code err;
      std::filesystem::path rel = std::filesystem::relative(itemPath, fspath, err);
      if (!err)
      {
        tmpPaths.push_back(rel);
      }
    }
  }

  std::sort(tmpPaths.begin(), tmpPaths.end(), [](std::filesystem::path& a, std::filesystem::path& b) {
    auto tA = a.filename().replace_extension();
    auto tB = b.filename().replace_extension();
    return tA.wstring() < tB.wstring();
  });

  std::filesystem::path listPath = fspath / PackageListName;
  std::ofstream s(listPath.wstring());
  for (const auto& path : tmpPaths)
  {
    s << dirCache.emplace_back(W2A(path.wstring())).String() << std::endl;
  }
}

void DecryptMapper(const std::filesystem::path& path, FString& decrypted)
{
  if (!std::filesystem::exists(path))
  {
    UThrow("File \"%ls\" does not exist!", path.wstring().c_str());
  }
  std::vector<char> encrypted;
  size_t size = 0;

  {
    std::ifstream s(path.wstring(), std::ios::binary | std::ios::ate);
    if (!s.is_open())
    {
      UThrow("Can't open \"%ls\"!", path.wstring().c_str());
    }
    s.seekg(0, std::ios_base::end);
    size = s.tellg();
    s.seekg(0, std::ios_base::beg);
    encrypted.resize(size);
    decrypted.Resize(size);
    s.read(&encrypted[0], size);
  }
  
  LogI("Decrypting %s", path.filename().string().c_str());

  size_t offset = 0;
  for (; offset + sizeof(Key1) < size; offset += sizeof(Key1))
  {
    for (size_t idx = 0; idx < sizeof(Key1); ++idx)
    {
      decrypted[offset + idx] = encrypted[offset + Key1[idx]];
    }
  }
  for (; offset < size; ++offset)
  {
    decrypted[offset] = encrypted[offset];
  }

  {
    size_t a = 1;
    size_t b = size - 1;
    for (offset = (size / 2 + 1) / 2; offset; --offset, a += 2, b -= 2)
    {
      std::swap(decrypted[a], decrypted[b]);
    }
  }

  for (offset = 0; offset < size; ++offset)
  {
    decrypted[offset] ^= Key2[offset % sizeof(Key2)];
  }

  if ((*(wchar*)decrypted.C_str()) == 0xFEFF)
  {
    FString utfstr = ((wchar*)decrypted.C_str()) + 1;
    decrypted = utfstr;
  }
}

void FPackage::SetRootPath(const FString& path)
{
  RootDir = path;
  std::filesystem::path listPath = std::filesystem::path(path.WString()) / PackageListName;
  std::ifstream s(listPath.wstring());
  if (s.is_open())
  {
    std::stringstream buffer;
    buffer << s.rdbuf();
    size_t pos = 0;
    size_t prevPos = 0;
    FString str = buffer.str();
    while ((pos = str.Find('\n', prevPos)) != std::string::npos)
    {
      DirCache.push_back(str.Substr(prevPos, pos - prevPos));
      pos++;
      std::swap(pos, prevPos);
    }
    return;
  }
  LogI("Building directory cache: \"%s\"", path.C_str());
  BuildPackageList(path, DirCache);
  LogI("Done. Found %ld packages", DirCache.size());
}

uint16 FPackage::GetCoreVersion()
{
  return CoreVersion;
}

void FPackage::LoadPersistentData()
{
  if (std::shared_ptr<FPackage> package = GetPackageNamed(PersistentDataName))
  {
    package->Load();
    for (FObjectExport* exp : package->Exports)
    {
      if (exp->GetClassName() == UPersistentCookerData::StaticClassName())
      {
        if (UPersistentCookerData* data = Cast<UPersistentCookerData>(package->GetObject(exp->ObjectIndex, false)))
        {
          data->GetPersistentData(BulkDataMap, TextureCacheMap);
          break;
        }
      }
    }
    UnloadPackage(package);
  }
}

void FPackage::LoadClassPackage(const FString& name)
{
  LogI("Loading %s", name.C_str());
  if (auto package = GetPackageNamed(name))
  {
    package->Load();
    DefaultClassPackages.push_back(package);
    if (name == "Core.u")
    {
      CoreVersion = package->GetFileVersion();
      LogI("Core version: %u/%u", package->GetFileVersion(), package->GetLicenseeVersion());
    }
    else if (package->GetFileVersion() != CoreVersion)
    {
      UThrow("Package %s has different version %d/%d ", name.C_str(), package->GetFileVersion(), package->GetLicenseeVersion());
    }

    UClass::CreateBuiltInClasses(package.get());

    // Load package in memory and create a stream
    FILE_OFFSET packageSize = package->Stream->GetSize();
    void* rawPackageData = malloc(packageSize);
    package->Stream->SerializeBytesAt(rawPackageData, 0, packageSize);

    MReadStream packageStream(rawPackageData, true, packageSize);
    packageStream.SetPackage(package.get());

    // Don't load referenced objects
    packageStream.SetLoadSerializedObjects(false);
    package->Stream->SetLoadSerializedObjects(false);

    // List of root classes
    std::vector<UObject*> classes;
    // List of root defaults\templates
    std::vector<UObject*> defaults;
    // Leftovers
    std::vector<UObject*> other;
    // Allocate all objects
    for (FObjectExport* exp : package->Exports)
    {
      UObject* obj = package->GetObject(exp->ObjectIndex, false);
      if (!exp->OuterIndex)
      {
        if (exp->Object && exp->Object->IsTemplate(RF_ClassDefaultObject))
        {
          defaults.push_back(obj);
        }
        else if (exp->GetClassName() == NAME_Class)
        {
          classes.push_back(obj);
        }
        else
        {
          other.push_back(obj);
        }
      }
      if (exp->GetClassName() == UClass::StaticClassName())
      {
        std::scoped_lock<std::mutex> l(ClassMapMutex);
        ClassMap[exp->GetObjectName()] = obj;
      }
    }

    // Load objects from child to parent to prevent possible stack overflow
    std::function<void(UObject*, std::vector<UObject*>&)> loader;
    loader = [&loader](UObject* obj, std::vector<UObject*>& children) {
      for (const FObjectExport* child : obj->GetExportObject()->Inner)
      {
        if (child->Object)
        {
          loader(child->Object, children);
        }
      }
      children.push_back(obj);
    };

    // Serialize classes
#if MULTITHREADED_CLASS_SERIALIZATION
    concurrency::parallel_for(size_t(0), size_t(classes.size()), [&](size_t idx) {
#else
    for (size_t idx = 0; idx < classes.size(); ++idx) {
#endif
      std::vector<UObject*> objects;
      loader(classes[idx], objects);
      MReadStream s(packageStream.GetAllocation(), false, packageSize);
      s.SetPackage(package.get());
      s.SetLoadSerializedObjects(false);
      for (UObject* obj : objects)
      {
        obj->Load(s);
      }
#if MULTITHREADED_CLASS_SERIALIZATION
    });
#else
    }
#endif

    // Link fields
#if MULTITHREADED_CLASS_SERIALIZATION
    concurrency::parallel_for(size_t(0), size_t(classes.size()), [&](size_t idx) {
#else
    for (size_t idx = 0; idx < classes.size(); ++idx) {
#endif
      if (UClass* cls = Cast<UClass>(classes[idx]))
      {
        cls->Link();
      }
#if MULTITHREADED_CLASS_SERIALIZATION
    });
#else
    }
#endif

    // Serialize defaults
#if MULTITHREADED_CLASS_SERIALIZATION
    concurrency::parallel_for(size_t(0), size_t(defaults.size()), [&](size_t idx) {
#else
    for (size_t idx = 0; idx < defaults.size(); ++idx) {
#endif
      UObject* root = defaults[idx];
      MReadStream s(packageStream.GetAllocation(), false, packageSize);
      s.SetPackage(package.get());
      s.SetLoadSerializedObjects(false);
      if (root->GetExportObject()->Inner.size())
      {
        std::vector<UObject*> objects;
        loader(root, objects);
        for (UObject* obj : objects)
        {
          obj->Load(s);
        }
      }
      else
      {
        root->Load(s);
      }
#if MULTITHREADED_CLASS_SERIALIZATION
    });
#else
    }
#endif

    // It's safe to load references now
    package->Stream->SetLoadSerializedObjects(true);
    packageStream.SetLoadSerializedObjects(true);

#if MULTITHREADED_CLASS_SERIALIZATION
    concurrency::parallel_for(size_t(0), size_t(other.size()), [&](size_t idx) {
#else
    for (size_t idx = 0; idx < other.size(); ++idx) {
#endif
      UObject* root = other[idx];
      MReadStream s(packageStream.GetAllocation(), false, packageSize);
      s.SetPackage(package.get());
      s.SetLoadSerializedObjects(false);
      if (root->GetExportObject()->Inner.size())
      {
        std::vector<UObject*> objects;
        loader(root, objects);
        for (UObject* obj : objects)
        {
          obj->Load(s);
        }
      }
      else
      {
        root->Load(s);
      }
#if MULTITHREADED_CLASS_SERIALIZATION
    });
#else
    }
#endif

    if (package->ObjectNameToExportMap.count(NAME_MetaData))
    {
      std::vector<FObjectExport*> expList = package->ObjectNameToExportMap[NAME_MetaData];
      while (expList.size())
      {
        UObject* tmp = package->GetObject(expList.back());
        expList.pop_back();
        if (UMetaData* meta = Cast<UMetaData>(tmp))
        {
          const auto& objMap = meta->GetObjectMetaDataMap();
          for (const auto& pair : objMap)
          {
            if (UField* field = Cast<UField>(pair.first))
            {
              for (const auto& info : pair.second)
              {
                if (info.first == "ToolTip")
                {
                  field->SetToolTip(info.second);
                  break;
                }
              }
            }
          }
          break;
        }
      }
    }

#ifdef _DEBUG
    for (FObjectExport* exp : package->Exports)
    {
      UObject* obj = package->GetObject(exp->ObjectIndex, false);
      DBreakIf(!obj || !obj->IsLoaded());
    }
#endif
  }
}

void FPackage::UnloadDefaultClassPackages()
{
  ClassMap.clear();
  MissingClasses.clear();
  for (auto package : DefaultClassPackages)
  {
    UnloadPackage(package);
  }
  DefaultClassPackages.clear();
}

void FPackage::LoadPkgMapper()
{
  std::filesystem::path storagePath = std::filesystem::path(RootDir.WString()) / PackageMapperName;
  storagePath.replace_extension(".re");
  std::filesystem::path encryptedPath = std::filesystem::path(RootDir.WString()) / "CookedPC" / PackageMapperName;
  encryptedPath.replace_extension(".dat");
  LogI("Reading %s storage...", PackageMapperName);

  uint64 fts = GetFileTime(encryptedPath.wstring());
  if (std::filesystem::exists(storagePath))
  {
    FReadStream rs(storagePath.wstring());
    uint64 ts = 0;
    rs << ts;
    if (fts == ts || !fts)
    {
      rs << PkgMap;
      return;
    }
    else
    {
      LogW("%s storage is outdated! Updating...", PackageMapperName);
    }
  }

  if (!std::filesystem::exists(encryptedPath))
  {
    UThrow("Failed to open %ls", encryptedPath.string().c_str());
  }

  FString buffer;
  DecryptMapper(encryptedPath, buffer);

  size_t pos = 0;
  size_t prevPos = 0;
  while ((pos = buffer.Find('|', prevPos)) != std::string::npos)
  {
    size_t sepPos = buffer.Find(',', prevPos);
    if (sepPos == std::string::npos || sepPos > pos)
    {
      UThrow("%s is corrupted!", PackageMapperName);
    }
    FString key = buffer.Substr(prevPos, sepPos - prevPos);
    FString value = buffer.Substr(sepPos + 1, pos - sepPos - 1);
    PkgMap.emplace(key, value);
    pos++;
    std::swap(pos, prevPos);
  }

  LogI("Saving %s storage", PackageMapperName);
  FWriteStream ws(storagePath.wstring());
  ws << fts;
  ws << PkgMap;

#if DUMP_MAPPERS
  std::filesystem::path debugPath = storagePath;
  debugPath.replace_extension(".txt");
  std::ofstream os(debugPath.wstring());
  os.write(&buffer[0], buffer.Size());
#endif
}

void FPackage::LoadCompositePackageMapper()
{
  std::filesystem::path storagePath = std::filesystem::path(RootDir.WString()) / CompositePackageMapperName;
  storagePath.replace_extension(".re");
  std::filesystem::path encryptedPath = std::filesystem::path(RootDir.WString()) / "CookedPC" / CompositePackageMapperName;
  encryptedPath.replace_extension(".dat");
  LogI("Reading %s storage...", CompositePackageMapperName);

  uint64 fts = GetFileTime(encryptedPath.wstring());
  if (std::filesystem::exists(storagePath))
  {
    FReadStream s(storagePath.wstring());
    uint64 ts = 0;
    s << ts;
    if (fts == ts || !fts)
    {
      s << CompositPackageMap;
      return;
    }
    else
    {
      LogW("%s storage is outdated! Updating...", CompositePackageMapperName);
    }
  }

  FString buffer;
  DecryptMapper(encryptedPath, buffer);

  size_t pos = 0;
  size_t posEnd = 0;
  std::vector<FString> packages;
  while ((pos = buffer.Find('?', posEnd)) != std::string::npos)
  {
    FString fileName = buffer.Substr(posEnd + 1, pos - posEnd - 1);
    posEnd = buffer.Find('!', pos);
    
    do
    {
      pos++;
      FCompositePackageMapEntry entry;
      entry.FileName = fileName;

      size_t elementEnd = buffer.Find(',', pos);
      entry.ObjectPath = buffer.Substr(pos, elementEnd - pos);
      std::swap(pos, elementEnd);
      pos++;

      elementEnd = buffer.Find(',', pos);
      FString packageName = buffer.Substr(pos, elementEnd - pos);
      std::swap(pos, elementEnd);
      pos++;

      elementEnd = buffer.Find(',', pos);
      entry.Offset = (FILE_OFFSET)std::stoul(buffer.Substr(pos, elementEnd - pos).String());
      std::swap(pos, elementEnd);
      pos++;

      elementEnd = buffer.Find(',', pos);
      entry.Size = (FILE_OFFSET)std::stoul(buffer.Substr(pos, elementEnd - pos).String());
      std::swap(pos, elementEnd);
      pos++;

      DBreakIf(CompositPackageMap.count(packageName));
      CompositPackageMap[packageName] = entry;
    } while (buffer.Find('|', pos) < posEnd - 1);
  }

  LogI("Saving %s storage", CompositePackageMapperName);
  FWriteStream s(storagePath.wstring());
  s << fts;
  s << CompositPackageMap;

#if DUMP_MAPPERS
  std::filesystem::path debugPath = storagePath;
  debugPath.replace_extension(".txt");
  std::ofstream os(debugPath.wstring());
  os.write(&buffer[0], buffer.Size());
#endif
}

void FPackage::LoadObjectRedirectorMapper()
{
  std::filesystem::path storagePath = std::filesystem::path(RootDir.WString()) / ObjectRedirectorMapperName;
  storagePath.replace_extension(".re");
  std::filesystem::path encryptedPath = std::filesystem::path(RootDir.WString()) / "CookedPC" / ObjectRedirectorMapperName;
  encryptedPath.replace_extension(".dat");
  LogI("Reading %s storage...", ObjectRedirectorMapperName);
  uint64 fts = GetFileTime(encryptedPath.wstring());
  if (std::filesystem::exists(storagePath))
  {
    FReadStream rs(storagePath.wstring());
    uint64 ts = 0;
    rs << ts;
    if (fts == ts || !fts)
    {
      rs << ObjectRedirectorMap;
      return;
    }
    else
    {
      LogW("%s storage is outdated! Updating...", ObjectRedirectorMapperName);
    }
  }

  FString buffer;
  DecryptMapper(encryptedPath, buffer);
  
  size_t pos = 0;
  size_t prevPos = 0;
  while ((pos = buffer.Find('|', prevPos)) != std::string::npos)
  {
    size_t sepPos = buffer.Find(',', prevPos);
    if (sepPos == std::string::npos || sepPos > pos)
    {
      UThrow("%s is corrupted!", ObjectRedirectorMapperName);
    }
    FString key = buffer.Substr(prevPos, sepPos - prevPos);
    FString value = buffer.Substr(sepPos + 1, pos - sepPos - 1);
    ObjectRedirectorMap.emplace(key, value);
    pos++;
    std::swap(pos, prevPos);
  }
  
  LogI("Saving %s storage", ObjectRedirectorMapperName);
  FWriteStream ws(storagePath.wstring());
  ws << fts;
  ws << ObjectRedirectorMap;

#if DUMP_MAPPERS
  std::filesystem::path debugPath = storagePath;
  debugPath.replace_extension(".txt");
  std::ofstream os(debugPath.wstring(), std::ios::out | std::ios::binary | std::ios::trunc);
  os.write(&buffer[0], buffer.Size());
  os.close();
#endif
}

std::shared_ptr<FPackage> FPackage::GetPackage(const FString& path)
{
  std::shared_ptr<FPackage> found = nullptr;
  {
    std::scoped_lock<std::recursive_mutex> lock(PackagesMutex);
    for (auto package : LoadedPackages)
    {
      if (package->GetSourcePath() == path)
      {
        found = package;
        break;
      }
    }
    if (found)
    {
      LoadedPackages.push_back(found);
      return found;
    }
  }
  
  LogI("Opening package: %ls", path.WString().c_str());
  FStream *stream = new FReadStream(path);
  if (!stream->IsGood())
  {
    UThrow("Couldn't open the file!");
  }
  FPackageSummary sum;
  sum.SourcePath = path;
  sum.DataPath = path;
  sum.PackageName = std::filesystem::path(path.WString()).filename().wstring();
  (*stream) << sum;
  if (sum.CompressedChunks.size())
  {
    FILE_OFFSET startOffset = INT_MAX;
    FILE_OFFSET totalDecompressedSize = 0;
    void** compressedChunksData = new void*[sum.CompressedChunks.size()];
    {
      uint32 idx = 0;
      for (FCompressedChunk& chunk : sum.CompressedChunks)
      {
        compressedChunksData[idx] = malloc(chunk.CompressedSize);
        stream->SetPosition(chunk.CompressedOffset);
        stream->SerializeBytes(compressedChunksData[idx], chunk.CompressedSize);
        totalDecompressedSize += chunk.DecompressedSize;
        if (chunk.DecompressedOffset < startOffset)
        {
          startOffset = chunk.DecompressedOffset;
        }
        idx++;
      }
    }

    std::filesystem::path decompressedPath = std::filesystem::temp_directory_path() / std::tmpnam(nullptr);
    sum.DataPath = W2A(decompressedPath.wstring());
    LogI("Decompressing package %s to %ls", sum.PackageName.C_str(), decompressedPath.wstring().c_str());
    FStream* tempStream = new FWriteStream(sum.DataPath.WString());

    std::vector<FCompressedChunk> tmpChunks;
    std::swap(sum.CompressedChunks, tmpChunks);
    (*tempStream) << sum;
    std::swap(sum.CompressedChunks, tmpChunks);

    uint8* decompressedData = (uint8*)malloc(totalDecompressedSize);
    try
    {
      concurrency::parallel_for(size_t(0), size_t(sum.CompressedChunks.size()), [sum, compressedChunksData, decompressedData, startOffset](size_t idx) {
        const FCompressedChunk& chunk = sum.CompressedChunks[idx];
        uint8* dst = decompressedData + chunk.DecompressedOffset - startOffset;
        LZO::Decompress(compressedChunksData[idx], chunk.CompressedSize, dst, chunk.DecompressedSize);
      });
    }
    catch (const std::exception& e)
    {
      for (size_t idx = 0; idx < sum.CompressedChunks.size(); ++idx)
      {
        free(compressedChunksData[idx]);
      }
      delete[] compressedChunksData;
      free(decompressedData);
      delete tempStream;
      delete stream;
      throw e;
    }
    for (size_t idx = 0; idx < sum.CompressedChunks.size(); ++idx)
    {
      free(compressedChunksData[idx]);
    }
    delete[] compressedChunksData;
    tempStream->SerializeBytes(decompressedData, totalDecompressedSize);
    free(decompressedData);
    delete tempStream;
    delete stream;

    // Read decompressed header
    stream = new FReadStream(sum.DataPath);
    try
    {
      (*stream) << sum;
    }
    catch (const std::exception& e)
    {
      delete stream;
      throw e;
    }
    sum.PackageName = std::filesystem::path(path.WString()).filename().wstring();
  }
  
  delete stream;
  std::shared_ptr<FPackage> result = nullptr;
  {
    std::scoped_lock<std::recursive_mutex> lock(PackagesMutex);
    result = LoadedPackages.emplace_back(new FPackage(sum));
  }
  DBreakIf(sum.ThumbnailTableOffset);
  return result;
}

std::shared_ptr<FPackage> FPackage::GetPackageNamed(const FString& name, FGuid guid)
{
  std::shared_ptr<FPackage> found = nullptr;
  {
    std::scoped_lock<std::recursive_mutex> lock(PackagesMutex);
    for (auto package : LoadedPackages)
    {
      if (package->GetPackageName() == name)
      {
        if (guid.IsValid())
        {
          if (package->GetGuid() == guid)
          {
            found = package;
            break;
          }
        }
        else
        {
          found = package;
          break;
        }
      }
    }
    if (found)
    {
      return LoadedPackages.emplace_back(found);
    }
  }
  
  // Check and load if the package is a composit package
  if (CoreVersion > VER_TERA_CLASSIC && CompositPackageMap.count(name))
  {
    const FCompositePackageMapEntry& entry = CompositPackageMap[name];
    FString packagePath;
    for (FString& path : DirCache)
    {
      std::wstring filename = path.FilenameW();
      if (filename.size() < entry.FileName.Size())
      {
        continue;
      }
      std::wstring tmp = entry.FileName.WString();
      if (std::mismatch(tmp.begin(), tmp.end(), filename.begin()).first == tmp.end())
      {
        packagePath = RootDir.AppendPath(path);
        break;
      }
    }
    if (packagePath.Size())
    {
      LogI("Reading composite package %s from %s...", name.C_str(), entry.FileName.C_str());
      void* rawData = malloc(entry.Size);
      {
        FReadStream rs(packagePath);
        rs.SetPosition(entry.Offset);
        rs.SerializeBytes(rawData, entry.Size);
        if (!rs.IsGood())
        {
          UThrow("Failed to read %s", name.C_str());
        }
      }
      std::filesystem::path tmpPath = std::filesystem::temp_directory_path() / std::tmpnam(nullptr);
      LogI("Writing composite package %s to %ls...", name.C_str(), tmpPath.wstring().c_str());
      {
        FWriteStream ws(tmpPath);
        ws.SerializeBytes(rawData, entry.Size);
        if (!ws.IsGood())
        {
          UThrow("Failed to save composit package %s to %ls", name.C_str(), tmpPath.wstring().c_str());
        }
      }
      free(rawData);
      std::shared_ptr<FPackage> package = GetPackage(tmpPath.wstring());
      package->CompositeDataPath = tmpPath.wstring();
      package->CompositeSourcePath = packagePath.WString();
      package->Summary.PackageName = name;
      return package;
    }
  }

  std::wstring wname = name.WString();
  for (FString& path : DirCache)
  {
    std::wstring filename = path.FilenameW();
    if (filename.size() < wname.size())
    {
      continue;
    }
    if (std::mismatch(wname.begin(), wname.end(), filename.begin()).first == wname.end())
    {
      if (auto package = GetPackage(RootDir.AppendPath(path)))
      {
        if (!guid.IsValid() || guid == package->GetGuid())
        {
          return package;
        }
        UnloadPackage(package);
      }
    }
  }
  return nullptr;
}

void FPackage::UnloadPackage(std::shared_ptr<FPackage> package)
{
  if (!package)
  {
    return;
  }
  bool lastPackageRef = false;
  {
    std::scoped_lock<std::recursive_mutex> lock(PackagesMutex);
    for (size_t idx = 0; idx < LoadedPackages.size(); ++idx)
    {
      if (LoadedPackages[idx].get() == package.get())
      {
        if (!lastPackageRef)
        {
          LoadedPackages.erase(LoadedPackages.begin() + idx);
          lastPackageRef = true;
          idx--;
        }
        else
        {
          // There are more refs. No need to continue
          lastPackageRef = false;
          break;
        }
      }
    }
  }
  if (lastPackageRef)
  {
    LogI("Package %s unloaded", package->GetPackageName().C_str());
  }
}

FPackage::~FPackage()
{
  if (Stream)
  {
    delete Stream;
  }
  for (auto& obj : Objects)
  {
    delete obj.second;
  }
  for (FObjectExport* exp : Exports)
  {
    delete exp;
  }
  for (FObjectImport* imp : Imports)
  {
    delete imp;
  }
  for (VObjectExport* exp : VExports)
  {
    delete exp;
  }
  
  for (auto& pkg : ExternalPackages)
  {
    UnloadPackage(pkg);
  }
  ExternalPackages.clear();
  if (Summary.DataPath != Summary.SourcePath)
  {
    std::filesystem::remove(std::filesystem::path(Summary.DataPath.WString()));
  }
  if (CompositeDataPath.Size())
  {
    std::filesystem::remove(std::filesystem::path(CompositeDataPath.WString()));
  }
}

void FPackage::Load()
{
#define CheckCancel() if (Cancelled.load()) { Loading.store(false); return; } //
  if (Ready.load() || Loading.load())
  {
    return;
  }
  Loading.store(true);
  Stream = new FReadStream(Summary.DataPath);
  Stream->SetPackage(this);
  FStream& s = GetStream();
  if (Summary.NamesOffset != s.GetPosition())
  {
    s.SetPosition(Summary.NamesOffset);
  }
  Names.clear();
  for (uint32 idx = 0; idx < Summary.NamesCount; ++idx)
  {
    FNameEntry& e = Names.emplace_back(FNameEntry());
    s << e;
    CheckCancel();
  }

  if (Summary.ImportsOffset != s.GetPosition())
  {
    s.SetPosition(Summary.ImportsOffset);
  }
  Imports.clear();
  for (uint32 idx = 0; idx < Summary.ImportsCount; ++idx)
  {
    FObjectImport* imp = Imports.emplace_back(new FObjectImport(this));
    imp->ObjectIndex = -(PACKAGE_INDEX)idx - 1;
    s << *imp;
    CheckCancel();
  }

  if (Summary.ExportsOffset != s.GetPosition())
  {
    s.SetPosition(Summary.ExportsOffset);
  }
  Exports.clear();
  for (uint32 idx = 0; idx < Summary.ExportsCount; ++idx)
  {
    FObjectExport* exp = Exports.emplace_back(new FObjectExport(this));
    exp->ObjectIndex = (PACKAGE_INDEX)idx + 1;
    s << *exp;
#ifdef _DEBUG
    exp->ClassNameValue = exp->GetClassName();
#endif
    CheckCancel();
  }

  if (Summary.DependsOffset != s.GetPosition())
  {
    s.SetPosition(Summary.DependsOffset);
  }
  Depends.clear();
  for (uint32 idx = 0; idx < Summary.ExportsCount; ++idx)
  {
    std::vector<int>& arr = Depends.emplace_back(std::vector<int>());
    s << arr;
    CheckCancel();
  }

  for (uint32 index = 0; index < Summary.ExportsCount; ++index)
  {
    FObjectExport* exp = Exports[index];
#ifdef _DEBUG
    exp->Path = exp->GetObjectPath();
#endif
    if (exp->OuterIndex)
    {
      FObjectExport* outer = GetExportObject(exp->OuterIndex);
      outer->Inner.push_back(exp);
    }
    else
    {
      RootExports.push_back(exp);
    }
    CheckCancel();
    ObjectNameToExportMap[exp->GetObjectName()].push_back(exp);
  }

  for (uint32 idx = 0; idx < Summary.ImportsCount; ++idx)
  {
    CheckCancel();
    FObjectImport* imp = Imports[idx];
#ifdef _DEBUG
    imp->Path = imp->GetObjectPath();
#endif
    if (imp->OuterIndex)
    {
      if (imp->OuterIndex < 0)
      {
        FObjectImport* outer = GetImportObject(imp->OuterIndex);
        outer->Inner.push_back(imp);
      }
    }
    else
    {
      RootImports.push_back(imp);
    }
  }

#if DUMP_PACKAGES
  _DebugDump();
#endif

  Ready.store(true);
  Loading.store(false);
}

VObjectExport* FPackage::CreateVirtualExport(const char* objName, const char* clsName)
{
  VObjectExport* exp = new VObjectExport(this, objName, clsName);
  VExports.push_back(exp);
  return exp;
}

FString FPackage::GetPackageName(bool extension) const
{
  if (extension)
  {
    return Summary.PackageName;
  }
  else
  {
    FString name = Summary.PackageName;
    if (name.Length())
    {
      size_t start = 0;
      size_t idx = name.FindFirstOf('.', start);
      while (idx == 0)
      {
        start++;
        idx = name.FindFirstOf('.', start);
      }
      if (idx != std::string::npos)
      {
        name = name.Substr(start, idx);
      }
    }
    return name;
  }
}

UObject* FPackage::GetObject(PACKAGE_INDEX index, bool load)
{
  if (Objects.count(index))
  {
    return Objects[index];
  }
  if (index > 0)
  {
    FObjectExport* exp = GetExportObject(index);
    if (exp->Object)
    {
      return exp->Object;
    }
    UObject* obj = UObject::Object(exp);
    Objects[index] = obj;
    if (load)
    {
      obj->Load();
    }
    return obj;
  }
  else if (index < 0)
  {
    FObjectImport* imp = GetImportObject(index);
    if (UObject* obj = GetObject(imp))
    {
      imp->Object = obj;
      return obj;
    }
  }
  return nullptr;
}

UObject* FPackage::GetObject(FObjectImport* imp)
{
  if (imp->Object)
  {
    return imp->Object;
  }
  if (imp->Package == this)
  {
    FString impPkgName = imp->GetPackageName();

    if (impPkgName != GetPackageName(false))
    {
      {
        std::scoped_lock<std::mutex> lock(ExternalPackagesMutex);
        for (std::shared_ptr<FPackage>& external : ExternalPackages)
        {
          if (impPkgName == external->GetPackageName(false))
          {
            return external->GetObject(imp);
          }
        }
      }
      if (auto package = GetPackageNamed(impPkgName))
      {
        package->Load();
        if (UObject* obj = package->GetObject(imp))
        {
          std::scoped_lock<std::mutex> lock(ExternalPackagesMutex);
          ExternalPackages.emplace_back(package);
          return obj;
        }
        UnloadPackage(package);
      }
      LogW("%s: Couldn't find import %s(%s)", GetPackageName().C_str(), imp->GetObjectName().C_str(), imp->GetClassName().C_str());
      return nullptr;
    }
  }
  for (VObjectExport* vexp : VExports)
  {
    if (vexp->GetObjectName() == imp->GetObjectName() && vexp->GetClassName() == imp->GetClassName())
    {
      return vexp->Object;
    }
  }
  std::vector<FObjectExport*> exps = GetExportObject(imp->GetObjectName());
  for (FObjectExport* exp : exps)
  {
    if (exp->GetClassName() == imp->GetClassName())
    {
      return GetObject(exp);
    }
  }
  return nullptr;
}

UObject* FPackage::GetObject(FObjectExport* exp)
{
  if (!Objects.count(exp->ObjectIndex))
  {
    for (VObjectExport* vexp : VExports)
    {
      if (vexp == exp)
      {
        return vexp->Object;
      }
    }
    Objects[exp->ObjectIndex] = UObject::Object(exp);
  }
  return Objects[exp->ObjectIndex];
}

UObject* FPackage::GetObject(FObjectExport* exp) const
{
  if (!Objects.count(exp->ObjectIndex))
  {
    for (VObjectExport* vexp : VExports)
    {
      if (vexp == exp)
      {
        return vexp->Object;
      }
    }
    return nullptr;
  }
  return Objects.at(exp->ObjectIndex);
}

PACKAGE_INDEX FPackage::GetObjectIndex(UObject* object) const
{
  if (!object)
  {
    return 0;
  }
  if (object->GetPackage() != this)
  {
    for (FObjectImport* imp : Imports)
    {
      if (imp->Object == object)
      {
        return imp->ObjectIndex;
      }
    }
    UThrow("%s does not have import object for %ls", GetPackageName().C_str(), object->GetObjectName().WString().c_str());
  }
  return object->GetExportObject()->ObjectIndex;
}

PACKAGE_INDEX FPackage::GetNameIndex(const FString& name, bool insert)
{
  for (PACKAGE_INDEX index = 0; index < Names.size(); ++index)
  {
    if (Names[index].GetString() == name)
    {
      return index;
    }
  }
  if (insert)
  {
    Names.push_back(FNameEntry(name));
    return (PACKAGE_INDEX)Names.size() - 1;
  }
  return INDEX_NONE;
}

UClass* FPackage::LoadClass(PACKAGE_INDEX index)
{
  if (!index)
  {
    return nullptr;
  }
  if (index > 0)
  {
    UObject* obj = GetObject(index);
    FString objName = obj->GetObjectName();
    {
      std::scoped_lock<std::mutex> l(ClassMapMutex);
      if (!ClassMap.count(objName))
      {
        ClassMap[objName] = obj;
      }
    }
    return (UClass*)obj;
  }
  FObjectImport* imp = GetImportObject(index);
  FString name = imp->GetObjectName();
  if (imp->Object)
  {
    return (UClass*)imp->Object;
  }
  else 
  {
    {
      std::scoped_lock<std::mutex> l(ClassMapMutex);
      if (ClassMap.count(name))
      {
        UObject* obj = ClassMap[name];
        imp->Object = obj;
        return (UClass*)obj;
      }
    }
    FString pkgName = imp->GetPackageName();
    if (pkgName == GetPackageName())
    {
      UObject* obj = GetObject(imp);
      if (obj)
      {
        std::scoped_lock<std::mutex> l(ClassMapMutex);
        ClassMap[name] = obj;
        return (UClass*)obj;
      }
    }
    {
      std::scoped_lock<std::mutex> l(ExternalPackagesMutex);
      for (auto pkg : ExternalPackages)
      {
        if (pkg->GetPackageName() == pkgName)
        {
          UObject* obj = pkg->GetObject(imp);
          if (obj)
          {
            std::scoped_lock<std::mutex> l(ClassMapMutex);
            ClassMap[name] = obj;
            ExternalPackages.push_back(pkg);
            return (UClass*)obj;
          }
          break;
        }
      }
    }
    if (auto pkg = GetPackageNamed(pkgName))
    {
      UObject* obj = pkg->GetObject(imp);
      if (obj)
      {
        std::scoped_lock<std::mutex> l(ClassMapMutex);
        ClassMap[name] = obj;
        std::scoped_lock<std::mutex> lock(ExternalPackagesMutex);
        ExternalPackages.push_back(pkg);
        return (UClass*)obj;
      }
      FPackage::UnloadPackage(pkg);
    }
  }
  
  if (!MissingClasses.count(name))
  {
    LogE("Failed to load class %s", name.C_str());
    MissingClasses.insert(name);
  }
  return nullptr;
}

void FPackage::AddNetObject(UObject* object)
{
  NetIndexMap[object->GetNetIndex()] = object;
}

std::vector<FObjectExport*> FPackage::GetExportObject(const FString& name)
{
  if (ObjectNameToExportMap.count(name))
  {
    return ObjectNameToExportMap[name];
  }
  for (VObjectExport* vexp : VExports)
  {
    if (vexp->GetObjectName() == name)
    {
      return { vexp };
    }
  }
  return std::vector<FObjectExport*>();
}

FObjectImport* FPackage::GetImportObject(const FString& objectName, const FString& className) const
{
  for (FObjectImport* imp : Imports)
  {
    if (imp->GetObjectName() == objectName && imp->GetClassName() == className)
    {
      return imp;
    }
  }
  return nullptr;
}

void FPackage::_DebugDump() const
{
#if DUMP_PACKAGES
  std::filesystem::path path = std::filesystem::path(DUMP_PATH) / GetPackageName().String();
  std::filesystem::create_directories(path);
  path /= "Info.txt";
  std::ofstream ds(path.wstring());
  ds << "SourcePath: \"" << Summary.SourcePath.UTF8() << "\"\n";
  ds << "DataPath: \"" << Summary.DataPath.UTF8() << "\"\n";
  if (GetFileVersion() > VER_TERA_CLASSIC && CompositeDataPath.Size())
  {
    ds << "CompositeSourcePath: \"" << CompositeSourcePath.UTF8() << "\"\n";
    ds << "CompositeDataPath: \"" << CompositeDataPath.UTF8() << "\"\n";
  }
  ds << "Version: " << Summary.FileVersion << "/" << Summary.LicenseeVersion << std::endl;
  ds << "HeaderSize: " << Summary.HeaderSize << std::endl;
  ds << "FolderName: \"" << Summary.FolderName.UTF8() << "\"\n";
  ds << "PackageFlags: " << PackageFlagsToString(Summary.PackageFlags).UTF8() << std::endl;
  ds << "CompressionFlags: " << Sprintf("0x%08X", Summary.CompressionFlags) << std::endl;
  ds << "PackageSource: " << Sprintf("0x%08X", Summary.PackageSource) << std::endl;
  ds << "Guid: " << Summary.Guid.String().UTF8() << std::endl;
  ds << "EngineVersion: " << Summary.EngineVersion << " ContentVersion: " << Summary.ContentVersion << std::endl;
  ds << "Names Count: " << Summary.NamesCount << " Offset: " << Summary.NamesOffset << std::endl;
  ds << "Exports Count: " << Summary.ExportsCount << " Offset: " << Summary.ExportsOffset << std::endl;
  ds << "Imports Count: " << Summary.ImportsCount << " Offset: " << Summary.ImportsOffset << std::endl;
  ds << "Depends Count: " << Summary.ExportsCount << " Offset: " << Summary.DependsOffset << std::endl;
  if (GetFileVersion() > VER_TERA_CLASSIC)
  {
    ds << "ImportGuids Count: " << Summary.ImportGuidsCount << " ExportGuids Count: " << Summary.ExportGuidsCount << " Offset: " << Summary.ImportExportGuidsOffset << std::endl;
    ds << "ThumbnailTable Offset: " << Summary.ThumbnailTableOffset << std::endl;
  }
  ds << "Generations:\n";
  for (const FGeneration& generation : Summary.Generations)
  {
    ds << "\tExports: " << generation.Exports << " Names: " << generation.Names << " Nets: " << generation.NetObjects << std::endl;
  }
  if (Summary.TextureAllocations.TextureTypes.size() && GetFileVersion() > VER_TERA_CLASSIC)
  {
    ds << "TextureAllocations:\n";
    for (const FTextureAllocations::FTextureType& ttype : Summary.TextureAllocations.TextureTypes)
    {
      ds << "\tSizeX: " << ttype.SizeX << " SizeY: " << ttype.SizeY << " NumMips: " << ttype.NumMips;
      ds << " Format: " << PixelFormatToString(ttype.Format).UTF8() << " TexFlags: " << Sprintf("0x%08X", ttype.TexCreateFlags);
      ds << " ExportIndices: ";
      for (const int32& idx : ttype.ExportIndices)
      {
        ds << idx << ", ";
      }
      ds << std::endl;
    }
  }
  if (Summary.AdditionalPackagesToCook.size())
  {
    ds << "AdditionalPackagesToCook:\n";
    for (const FString& pkg : Summary.AdditionalPackagesToCook)
    {
      ds << "\t" << pkg.UTF8() << std::endl;
    }
  }

  ds << "Names:\n";
  for (int32 index = 0; index < Names.size(); ++index)
  {
    ds << "\t[" << index << "]" << Names[index].GetString().UTF8() << "\n";
  }
  ds << "\n\nExports:\n";

  std::function<void(FObjectExport*, int32)> printExp;
  printExp = [&ds, &printExp](FObjectExport* exp, int32 depth) {
    for (int32 i = 0; i < depth; ++i)
    {
      ds << "\t";
    }
    ds << "[" << exp->ObjectIndex << "]" << exp->GetFullObjectName().UTF8();
    ds << "(Offset: " << exp->SerialOffset << ", Size: " << exp->SerialSize;
    if (exp->ExportFlags)
    {
      ds << ", EF: " << ExportFlagsToString(exp->ExportFlags).UTF8();
    }
    ds << ", OF: " << ObjectFlagsToString(exp->ObjectFlags).UTF8();
    ds << ")\n";
    depth++;
    for (FObjectExport* cexp : exp->Inner)
    {
      printExp(cexp, depth);
    }
  };
  for (FObjectExport* e : RootExports)
  {
    printExp(e, 1);
  }
  ds << "\n\nImports:\n";
  std::function<void(FObjectImport*, int32)> printImp;
  printImp = [&ds, &printImp](FObjectImport* imp, int32 depth) {
    for (int32 i = 0; i < depth; ++i)
    {
      ds << "\t";
    }
    ds << "[" << imp->ObjectIndex << "]" << imp->GetFullObjectName().UTF8() << "\n";
    depth++;
    for (FObjectImport* cimp : imp->Inner)
    {
      printImp(cimp, depth);
    }
  };
  for (FObjectImport* i : RootImports)
  {
    printImp(i, 1);
  }
#endif
}