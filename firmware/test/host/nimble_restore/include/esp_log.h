#pragma once
/* Consume tag but never log security material. */
#define ESP_LOGD(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
