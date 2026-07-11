#pragma once

// TEST-ONLY Http::Client implementation over POSIX sockets (plain HTTP,
// Connection: close, Content-Length bodies). Lets the Gateway transport
// logic run for real on the CI/dev machine; production uses WinHTTP.

#include <memory>

#include "http/HttpClient.h"

std::unique_ptr<Http::Client> MakePosixHttpClient();
