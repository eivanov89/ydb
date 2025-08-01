#pragma once

#include <yql/essentials/utils/rand_guid.h>

#include <util/generic/hash.h>
#include <util/folder/path.h>

namespace NYql {
namespace NCommon {

/*
  Resembles sandbox for external UDFs
*/
class TFilesBox {
public:
    TFilesBox(TFsPath dir, TRandGuid randGuid);
    ~TFilesBox();

    TString MakeLinkFrom(const TString& source, const TString& filename = {});
    TString GetDir() const;

    void Destroy();

private:
    TFsPath Dir_;
    TRandGuid RandGuid_;
    THashMap<TString, TString> Mapping_;
};

THolder<TFilesBox> CreateFilesBox(const TFsPath& baseDir);

}
}
