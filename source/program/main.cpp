#include "lib.hpp"

#include <curl/curl.h>
#include <nn/ssl.h>
#include <nn/socket.h>
#include "nn_extra.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/*
 * Example exlaunch module for the nnsdk libcurl port.
 *
 * We hook nn::oe::Initialize to just stub the game booting up, and then
 * we run a full curl request to https://flexlion3.herokuapp.com/ 
 *
 *   1. nn::socket::Initialize  - init socket (some games may do this for you, for some you'd need to 
 *                                do it yourself, or even hook the game as it may want to init/deinit it conditionally)
 *   2. nn::nifm                - request the network interface and wait for it
 *   3. libcurl                 - GET https://flexlion3.herokuapp.com/ over SSL,
 *                                with the nn::ssl context created in the
 *                                CURLOPT_SSL_CTX_FUNCTION callback (necessary for SSL and possibly HTTP too)
 *   4. nn::fs                  - write the response body to the SD card
 *   5. idle forever
 *
 * Memory note: both the 6 MB nn::socket pool and all of libcurl's allocations
 * run on newlib malloc, backed by the exlaunch fake heap (see
 * exl::setting::HeapSize in setting.hpp, enlarged to 15 MB for this).
 * 
 * If successful, curlsdk_example.txt will be created in the SD card and say "hewwo".
 */

namespace {

constexpr const char* kRequestUrl  = "https://flexlion3.herokuapp.com/";
constexpr const char* kSdMountName = "sd";
constexpr const char* kLogPath     = "sd:/curlsdk_example.txt";

constexpr size_t kSocketPoolSize    = 6 * 1024 * 1024; /* 6 MB    */
constexpr size_t kSocketAllocSize   = 0x180000;        /* 1.5 MB  */
constexpr int    kSocketConcurrency = 14;

char   g_ResponseBuf[8192];
size_t g_ResponseLen = 0;

/* CURLOPT_SSL_CTX_FUNCTION: the nnssl backend hands us the nn::ssl::Context it
   will use for this connection; we can just create it with SslVersion::Auto. */
CURLcode SslCtxCallback(CURL* /*easy*/, void* ssl_ctx, void* /*userptr*/) {
    auto* ctx = static_cast<nn::ssl::Context*>(ssl_ctx);
    nn::Result r = ctx->Create(nn::ssl::Context::SslVersion::Auto);
    return r.IsSuccess() ? CURLE_OK : CURLE_SSL_CTX_FATAL;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* /*userp*/) {
    const size_t total = size * nmemb;
    const size_t space = (g_ResponseLen < sizeof(g_ResponseBuf) - 1)
                         ? (sizeof(g_ResponseBuf) - 1 - g_ResponseLen) : 0;
    const size_t copy = total < space ? total : space;
    if(copy) {
        memcpy(g_ResponseBuf + g_ResponseLen, contents, copy);
        g_ResponseLen += copy;
        g_ResponseBuf[g_ResponseLen] = '\0';
    }
    return total; 
}

/* Optional (CURLOPT_SOCKOPTFUNCTION): curl calls this for every socket it opens,
   before connecting. Enlarging the TCP send/receive buffers and using nodelay
   gives noticeably faster transfers, but it is not necessary */
int SockOptCallback(void* /*clientp*/, curl_socket_t curlfd, curlsocktype /*purpose*/) {
    int sndBufSize = 256 * 1024; /* 256 KB */
    int rcvBufSize = 64 * 1024;  /* 64 KB  */
    int tcpNoDelay = 1;
    nn::socket::SetSockOpt(curlfd, nn::socket::Level::Sol_Socket,
                           nn::socket::Option::So_SndBuf, &sndBufSize, sizeof(sndBufSize));
    nn::socket::SetSockOpt(curlfd, nn::socket::Level::Sol_Socket,
                           nn::socket::Option::So_RcvBuf, &rcvBufSize, sizeof(rcvBufSize));
    nn::socket::SetSockOpt(curlfd, nn::socket::Level::Sol_Tcp,
                           nn::socket::Option::Tcp_NoDelay, &tcpNoDelay, sizeof(tcpNoDelay));
    return CURL_SOCKOPT_OK;
}

bool InitNetwork() {
    static void* socketMem = nullptr;
    if(!socketMem) {
        socketMem = aligned_alloc(0x1000, kSocketPoolSize);
        if(!socketMem)
            return false;
    }
    nn::socket::Initialize(socketMem, kSocketPoolSize,
                           kSocketAllocSize, kSocketConcurrency);

    nn::nifm::Initialize();
    nn::nifm::SubmitNetworkRequestAndWait();
    return nn::nifm::IsNetworkAvailable();
}

CURLcode DoRequest() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    CURLcode res = CURLE_FAILED_INIT;
    CURL* curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, kRequestUrl);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "CurlSdkExample/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, SslCtxCallback); // mandatory
        curl_easy_setopt(curl, CURLOPT_SSL_CTX_DATA, nullptr);
        //curl_easy_setopt(curl, CURLOPT_SSL_SKIP_DEFAULT_VERIFY, 1L);
        //curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        //curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);

        /* Optional for higher speed */
        curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, SockOptCallback);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024L); /* 128 KB */

        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    return res;
}

void WriteLog(CURLcode res, bool netUp) {
    char msg[8192];
    int len;
    if(res == CURLE_OK && g_ResponseLen > 0)
        len = snprintf(msg, sizeof(msg), "%s", g_ResponseBuf);
    else if(!netUp)
        len = snprintf(msg, sizeof(msg), "network unavailable\n");
    else
        len = snprintf(msg, sizeof(msg), "curl failed: %d (%s)\n",
                       (int)res, curl_easy_strerror(res));
    if(len < 0)
        return;

    nn::fs::MountSdCard(kSdMountName);  /* no-op/!success if already mounted */
    nn::fs::DeleteFile(kLogPath);       /* ignore: file may not exist yet */
    if(nn::fs::CreateFile(kLogPath, len).IsFailure())
        return;

    nn::fs::FileHandle handle;
    if(nn::fs::OpenFile(&handle, kLogPath, nn::fs::OpenMode_Write).IsFailure())
        return;
    nn::fs::SetFileSize(handle, len);
    nn::fs::WriteFile(handle, 0, msg, static_cast<uint64_t>(len),
                      nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
    nn::fs::FlushFile(handle);
    nn::fs::CloseFile(handle);
}

void RunCurlExample() {
    const bool netUp = InitNetwork();
    const CURLcode res = netUp ? DoRequest() : CURLE_COULDNT_CONNECT;
    WriteLog(res, netUp);
}

} // namespace

HOOK_DEFINE_TRAMPOLINE(OeInitializeHook) {
    static void Callback() {
        Orig();

        static bool ran = false;
        if(ran)
            return;
        ran = true;

        RunCurlExample();

        while(true)
            svcSleepThread(1000000000LL);
    }
};

extern "C" void exl_main(void* x0, void* x1) {
    exl::hook::Initialize();
    OeInitializeHook::InstallAtFuncPtr(nn::oe::Initialize);
}

extern "C" NORETURN void exl_exception_entry() {
    /* Note: this is only applicable in the context of applets/sysmodules. */
    EXL_ABORT("Default exception handler called!");
}
