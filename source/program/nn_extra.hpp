#pragma once

#include <nn/result.h>
#include <cstdint>

namespace nn {

namespace oe {
    void Initialize();
}

namespace nifm {
    nn::Result Initialize();
    nn::Result SubmitNetworkRequestAndWait();
    bool       IsNetworkAvailable();
}

namespace fs {

    struct FileHandle { void* handle; };

    enum OpenMode {
        OpenMode_Read      = (1 << 0),
        OpenMode_Write     = (1 << 1),
        OpenMode_Append    = (1 << 2),
        OpenMode_ReadWrite = OpenMode_Read | OpenMode_Write,
    };

    enum WriteOptionFlag {
        WriteOptionFlag_None  = 0,
        WriteOptionFlag_Flush = 1,
    };

    struct WriteOption {
        int flags;
        static WriteOption CreateOption(int f) { WriteOption o; o.flags = f; return o; }
    };

    nn::Result MountSdCard(const char* name);
    nn::Result Unmount(const char* name);
    nn::Result CreateFile(const char* path, int64_t size);
    nn::Result DeleteFile(const char* path);
    nn::Result OpenFile(FileHandle* outHandle, const char* path, int32_t mode);
    nn::Result SetFileSize(FileHandle handle, int64_t size);
    nn::Result WriteFile(FileHandle handle, int64_t offset, const void* buf,
                         uint64_t size, const WriteOption& option);
    nn::Result FlushFile(FileHandle handle);
    void       CloseFile(FileHandle handle);

} 

}
