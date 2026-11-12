#include <cstdint>
#include <cstdlib>
#include <HTTPClient.h>
#include <Arduino.h>

namespace {
[[noreturn]] void unexpected_http_call() { std::abort(); }
}
bool HTTPClient::begin(WiFiClient&, const char*) { unexpected_http_call(); }
bool HTTPClient::begin(WiFiClient&, String) { unexpected_http_call(); }
void HTTPClient::setTimeout(uint16_t) { unexpected_http_call(); }
void HTTPClient::setConnectTimeout(int32_t) { unexpected_http_call(); }
void HTTPClient::addHeader(const char*, const char*) { unexpected_http_call(); }
void HTTPClient::addHeader(const String&, const String&, bool, bool) { unexpected_http_call(); }
void HTTPClient::collectHeaders(const char**, size_t) { unexpected_http_call(); }
String HTTPClient::header(const char*) { unexpected_http_call(); }
int HTTPClient::POST(uint8_t*, size_t) { unexpected_http_call(); }
int HTTPClient::GET() { unexpected_http_call(); }
int HTTPClient::getSize() { unexpected_http_call(); }
Stream* HTTPClient::getStreamPtr() { unexpected_http_call(); }
int HTTPClient::writeToStream(Stream*) { unexpected_http_call(); }
void HTTPClient::end() { unexpected_http_call(); }
uint32_t millis() { unexpected_http_call(); }
