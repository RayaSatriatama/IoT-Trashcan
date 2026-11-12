#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "WiFiClientSecure.h"
#include "Arduino.h"
class Stream { public: virtual ~Stream()=default; virtual int available()=0; virtual int read()=0; virtual int peek()=0; virtual void flush()=0; virtual size_t write(uint8_t)=0; virtual size_t readBytes(uint8_t*,size_t){return 0;} virtual size_t readBytes(char* p,size_t n){return readBytes(reinterpret_cast<uint8_t*>(p),n);} };
class HTTPClient { public: bool begin(WiFiClient&,const char*); bool begin(WiFiClient&,String); void setTimeout(uint16_t); void setConnectTimeout(int32_t); void addHeader(const char*,const char*); void addHeader(const String&,const String&,bool=false,bool=false); void collectHeaders(const char**,size_t); String header(const char*); int POST(uint8_t*,size_t); int GET(); int getSize(); Stream* getStreamPtr(); int writeToStream(Stream*); void end(); };
